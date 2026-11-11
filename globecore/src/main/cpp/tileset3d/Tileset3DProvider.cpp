#include "tileset3d/Tileset3DProvider.h"

#include "tileset3d/Tileset3DLoader.h"
#include "util/Log.h"

#include <cmath>

namespace globecore {
namespace {

// @Jin 量化眼点，轻微相机抖动不触发整套 Tileset 重新选择。
uint64_t quantizedEyeSignature(const Tileset3DViewport &vp) {
    auto quantize = [](double v) {
        return static_cast<int64_t>(std::llround(v / 5.0));
    };
    const int64_t x = quantize(vp.eyeEcef.x);
    const int64_t y = quantize(vp.eyeEcef.y);
    const int64_t z = quantize(vp.eyeEcef.z);
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(x));
    mix(static_cast<uint64_t>(y));
    mix(static_cast<uint64_t>(z));
    mix(static_cast<uint64_t>(vp.viewportHeightPx));
    return hash;
}

} // namespace

Tileset3DProvider::Tileset3DProvider(int layerId, std::string rootUrl,
                                     ViewportProvider viewportProvider, ModelSink sink,
                                     RenderRequest renderRequest, ActiveUrlSink activeUrlSink)
    : layerId_(layerId), rootUrl_(std::move(rootUrl)),
      viewportProvider_(std::move(viewportProvider)), sink_(std::move(sink)),
      renderRequest_(std::move(renderRequest)), activeUrlSink_(std::move(activeUrlSink)) {}

Tileset3DProvider::~Tileset3DProvider() {
    stop();
}

void Tileset3DProvider::start() {
    if (worker_.joinable()) return;
    stopping_ = false;
    refreshPending_ = true;
    worker_ = std::thread([this] { workerLoop(); });
}

void Tileset3DProvider::stop() {
    if (!worker_.joinable()) return;
    stopping_ = true;
    waitCv_.notify_all();
    worker_.join();
}

void Tileset3DProvider::updateViewport(const Tileset3DViewport &viewport) {
    if (!viewport.valid) return;
    const uint64_t signature = quantizedEyeSignature(viewport);
    if (signature == lastViewportSignature_.load()) return;
    lastViewportSignature_ = signature;
    requestRefresh();
}

void Tileset3DProvider::requestRefresh() {
    refreshPending_ = true;
    waitCv_.notify_all();
}

void Tileset3DProvider::workerLoop() {
    while (!stopping_) {
        {
            std::unique_lock<std::mutex> lock(waitMtx_);
            waitCv_.wait(lock, [this] { return stopping_ || refreshPending_.load(); });
            if (stopping_) return;
            refreshPending_ = false;
        }

        Tileset3DViewport viewport;
        if (viewportProvider_) viewport = viewportProvider_();
        if (!viewport.valid) continue;
        refresh(viewport);
    }
}

bool Tileset3DProvider::ensureRootGraph() {
    std::lock_guard<std::mutex> lock(dataMtx_);
    if (rootGraph_) return true;

    auto loaded = Tileset3DLoader::loadTileset(rootUrl_);
    if (!loaded.ok) {
        lastError_ = loaded.error;
        LOGW("Tileset3D root load failed: %s", loaded.error.c_str());
        return false;
    }
    auto graph = std::make_unique<Tileset3DGraph>();
    *graph = Tileset3DGraphBuilder::build(rootUrl_, loaded.tileset);
    rootGraph_ = std::move(graph);
    LOGI("Tileset3D root graph ready: url=%s", rootUrl_.c_str());
    return true;
}

bool Tileset3DProvider::ensureExternalGraph(const Tileset3DSelection &selection) {
    if (!selection.isExternalTileset) return true;
    {
        std::lock_guard<std::mutex> lock(dataMtx_);
        if (externalGraphs_.find(selection.contentUrl) != externalGraphs_.end()) return true;
    }

    auto loaded = Tileset3DLoader::loadTileset(selection.contentUrl);
    if (!loaded.ok) {
        LOGW("Tileset3D external graph failed: %s", loaded.error.c_str());
        return false;
    }

    auto external = std::make_unique<ExternalGraph>();
    external->url = selection.contentUrl;
    auto graph = std::make_unique<Tileset3DGraph>(
        Tileset3DGraphBuilder::build(selection.contentUrl, loaded.tileset,
                                    selection.worldTransform));
    external->graph = std::move(graph);

    std::lock_guard<std::mutex> lock(dataMtx_);
    externalGraphs_.emplace(selection.contentUrl, std::move(external));
    return true;
}

void Tileset3DProvider::refresh(const Tileset3DViewport &viewport) {
    if (!ensureRootGraph()) return;

    constexpr double kDefaultSse = 16.0;
    const std::array<FrustumPlane, 6> *frustum =
            viewport.hasFrustum ? &viewport.frustumPlanes : nullptr;

    // @Jin 单个 b3dm 下载任务；只复制 upAxis 字符串，不再让 Tileset3DGraph 裸指针逃出锁区。
    struct B3dmTask {
        Tileset3DSelection selection;
        std::string gltfUpAxis;
    };

    // @Jin 第一阶段（锁内）：用工作清单递归展开所有已就绪的 tileset 图，
    // 收集 b3dm 任务与缺失的外部 tileset；锁内只遍历内存结构，不做任何网络 IO。
    std::vector<B3dmTask> b3dmTasks;
    std::vector<Tileset3DSelection> missingExternal;
    {
        std::lock_guard<std::mutex> lock(dataMtx_);
        if (!rootGraph_) return;

        std::vector<const Tileset3DGraph *> graphWorklist;
        graphWorklist.push_back(rootGraph_.get());
        // @Jin 同一外部 tileset URL 可能被多个 tile 节点引用，去重后只下载/入队一次。
        std::unordered_set<std::string> seenExternal;

        while (!graphWorklist.empty()) {
            const Tileset3DGraph *graph = graphWorklist.back();
            graphWorklist.pop_back();
            if (graph == nullptr) continue;

            const std::vector<Tileset3DSelection> selected = Tileset3DGraphBuilder::select(
                    *graph, viewport.eyeEcef, viewport.viewportHeightPx,
                    viewport.fovYRad, kDefaultSse, frustum);
            for (const Tileset3DSelection &item : selected) {
                if (item.isB3dm) {
                    // @Jin 叶子 b3dm：连同所属图的 gltfUpAxis 一起入队，锁外下载时无需再回查图指针。
                    b3dmTasks.push_back(B3dmTask{item, graph->tileset->gltfUpAxis}); // @Jin tileset 已改为 unique_ptr 堆托管
                } else if (item.isExternalTileset) {
                    if (!seenExternal.insert(item.contentUrl).second) continue;
                    auto it = externalGraphs_.find(item.contentUrl);
                    if (it == externalGraphs_.end() || !it->second || !it->second->graph) {
                        // @Jin 外部 JSON 尚未下载：记录后在锁外补齐，下一轮 refresh 再继续向下展开。
                        missingExternal.push_back(item);
                    } else {
                        // @Jin 已就绪的子图重新入队，支持 root→external→external→b3dm 任意深度嵌套。
                        graphWorklist.push_back(it->second->graph.get());
                    }
                }
            }
        }
    }

    // @Jin 第二阶段（锁外）：联网下载缺失的外部 tileset，绝不持锁等待网络，避免阻塞 GL/帧线程。
    bool externalLoaded = false;
    for (const Tileset3DSelection &item : missingExternal) {
        if (ensureExternalGraph(item)) externalLoaded = true;
    }
    if (externalLoaded) {
        // @Jin 新子图就绪后立刻请求下一轮，递归选择直到叶子 b3dm；静态画面由量化签名去重兜底。
        requestRefresh();
    }

    // @Jin REPLACE 语义：把本轮所有图递归选择出的 b3dm URL 整体发布给图层，Renderer 据此跳过已不在选择集内的历史 LOD。
    if (activeUrlSink_) {
        std::unordered_set<std::string> activeUrls;
        activeUrls.reserve(b3dmTasks.size());
        for (const B3dmTask &task : b3dmTasks) activeUrls.insert(task.selection.contentUrl);
        activeUrlSink_(std::move(activeUrls));
    }

    // @Jin 第三阶段（锁外）：单帧最多处理 8 个 b3dm，避免首次进入视角时网络/解码冲击过大。
    constexpr size_t kMaxModelsPerRefresh = 8;
    size_t processed = 0;
    for (const B3dmTask &task : b3dmTasks) {
        if (processed >= kMaxModelsPerRefresh) break;
        if (loadModel(task.gltfUpAxis, task.selection)) ++processed;
    }
}

bool Tileset3DProvider::loadModel(const std::string &gltfUpAxis,
                                  const Tileset3DSelection &selection) {
    {
        std::lock_guard<std::mutex> lock(dataMtx_);
        if (loadedModelUrls_.count(selection.contentUrl) != 0) return true;
        loadedModelUrls_.insert(selection.contentUrl);
    }

    B3dmContent b3dm = Tileset3DLoader::loadB3dm(selection.contentUrl);
    if (!b3dm.ok) {
        LOGW("Tileset3D b3dm failed: url=%s err=%s", selection.contentUrl.c_str(), b3dm.error.c_str());
        std::lock_guard<std::mutex> lock(dataMtx_);
        loadedModelUrls_.erase(selection.contentUrl);
        return false;
    }

    GltfModel gltf = decodeGlb(b3dm.glb);
    if (!gltf.ok || gltf.primitives.empty()) {
        LOGW("Tileset3D embedded glb failed: url=%s err=%s", selection.contentUrl.c_str(), gltf.error.c_str());
        std::lock_guard<std::mutex> lock(dataMtx_);
        loadedModelUrls_.erase(selection.contentUrl);
        return false;
    }

    Tileset3DModelCpu model = Tileset3DModelBuilder::build(
        selection.contentUrl, selection.contentUrl, b3dm, gltf,
        selection.worldTransform, gltfUpAxis,
        selection.boundsCenter, selection.boundsRadius);
    if (sink_) sink_(std::move(model));
    if (renderRequest_) renderRequest_();
    LOGI("Tileset3D model delivered: layer=%d url=%s", layerId_, selection.contentUrl.c_str());
    return true;
}
} // namespace globecore
