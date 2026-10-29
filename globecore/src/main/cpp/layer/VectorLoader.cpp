#include "layer/VectorLoader.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include "util/Log.h"
#include "vector/RemoteVectorCache.h"
#include "vector/VectorSourceResolver.h"
#include "vector/VectorBuilder.h"
#include "vector/VectorReader.h"

namespace globecore {

VectorLoader::VectorLoader(std::string path, VectorStyle style,
                           bool hasExtent, double minLon, double minLat,
                           double maxLon, double maxLat, int maxFeatures,
                           std::string vectorCacheDir)
    : style_(style),
      vectorCacheDir_(std::move(vectorCacheDir)),
      hasExtent_(hasExtent), minLon_(minLon), minLat_(minLat), maxLon_(maxLon), maxLat_(maxLat),
      maxFeatures_(maxFeatures) {
    // @Jin 在后台线程启动前完成 URL 识别；http/https 走下载缓存，本地路径沿用旧 GDALOpenEx 行为。
    ResolvedVectorSource resolved = VectorSourceResolver::resolve(path, vectorCacheDir_);
    remoteSource_ = resolved.remote;
    path_ = std::move(resolved.originalSource);
    cachePath_ = resolved.cachePath;
    effectivePath_ = remoteSource_ ? std::string() : path_;
    sourceLogName_ = resolved.safeLogName;
}

VectorLoader::VectorLoader(VectorStyle style)
    : style_(std::move(style)), memMode_(true) {}

VectorLoader::~VectorLoader() {
    abort_.store(true);
    if (thread_.joinable()) thread_.join();
}

void VectorLoader::start() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (memMode_) return; // 内存模式无文件可加载，几何由 setData 推送
    if (state_ != State::Idle) return;
    ensureWorkerLocked();
}

void VectorLoader::reload(bool hasExtent, double minLon, double minLat,
                          double maxLon, double maxLat, int maxFeatures) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (memMode_) return;
    // 记录最新范围（供 worker 下一轮读取）；worker 存活则复用（置 rerunRequested_），否则新起。
    hasExtent_ = hasExtent;
    minLon_ = minLon; minLat_ = minLat; maxLon_ = maxLon; maxLat_ = maxLat;
    maxFeatures_ = maxFeatures;
    // [诊断] 记录本次重载请求的 extent 与 worker 状态：workerRunning_=true 表示当前仍在读上一轮，
    // 本次仅置 rerunRequested_ 复用（排队到本轮完成后再来一轮），false 表示新起线程立即读。
    LOGI("[VecReload] VectorLoader reload 请求: %s hasExtent=%d extent=[%.6f,%.6f,%.6f,%.6f] workerRunning=%d",
         sourceLogName_.c_str(), hasExtent ? 1 : 0, minLon, minLat, maxLon, maxLat, workerRunning_ ? 1 : 0);
    ensureWorkerLocked();
}

void VectorLoader::ensureWorkerLocked() {
    // 后台 worker 存活：记录最新范围并置抢占标志，令其当前读/建在阶段边界提前中断后立即重跑（latest-wins），
    // 避免一笔过期的巨型加载把最新的快速重载饿死。
    if (workerRunning_) { rerunRequested_ = true; stale_.store(true); return; }
    // 回收上一轮已结束的线程（workerRunning_==false 意味 run() 已不再取锁、即将返回，join 立即返回）。
    if (thread_.joinable()) thread_.join();
    workerRunning_ = true;
    state_ = State::Loading;
    thread_ = std::thread(&VectorLoader::run, this);
}

void VectorLoader::setData(VectorReadResult data) {
    if (!memMode_) return;
    // 同步构建（纯 CPU、小数据量）；构建期间不持锁，避免阻塞 GL 线程的 drainReady。
    // 完成后于锁内移交 geom_ 并置 Ready：GL 线程下一次 drainReady 取走上传（后到者覆盖）。
    VectorGeometry g = buildGeometry(data, style_);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        geom_ = std::move(g);
        state_ = State::Ready;
    }
}

bool VectorLoader::drainReady(VectorGeometry &out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_ != State::Ready) return false;
    out = std::move(geom_);
    geom_ = VectorGeometry{};
    state_ = State::Consumed;
    return true;
}

bool VectorLoader::hasPendingOrReady() {
    std::lock_guard<std::mutex> lk(mtx_);
    // @Jin 下载、GDAL 读取、几何构建任一阶段未完成都要驱动按需渲染。
    return state_ == State::Loading || state_ == State::Downloading ||
           state_ == State::Reading || state_ == State::Building || state_ == State::Ready;
}

std::string VectorLoader::error() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return error_;
}

void VectorLoader::run() {
    for (;;) {
        // 取本轮要用的屏幕范围快照（持锁短暂）；清 rerunRequested_，若读取期间又有新范围会重新置位。
        bool hasExtent; double a, b, c, d; int cap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            hasExtent = hasExtent_; a = minLon_; b = minLat_; c = maxLon_; d = maxLat_;
            cap = maxFeatures_;
            rerunRequested_ = false;
            stale_.store(false); // 本轮开始清抢占标志（上一轮 reload 触发的置真随旧结果一并作废）
            state_ = remoteSource_ ? State::Downloading : State::Reading;
        }
        const auto t0 = std::chrono::steady_clock::now();

        std::string readPath = path_;
        if (remoteSource_) {
            if (cachePath_.empty() || vectorCacheDir_.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                error_ = "在线 GeoJSON 缓存目录未配置";
                state_ = State::Failed;
                workerRunning_ = false;
                return;
            }
            if (effectivePath_.empty() || !std::filesystem::exists(effectivePath_)) {
                RemoteVectorFetchOptions fetchOptions;
                fetchOptions.cacheDir = vectorCacheDir_;
                // @Jin P0 只支持公开 URL；headers/ETag/refresh 等列入后续阶段。
                const RemoteVectorFetchResult fetched = RemoteVectorCache::prepareGeoJson(
                    path_, cachePath_, fetchOptions, abort_);
                if (abort_.load()) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    workerRunning_ = false;
                    return;
                }
                if (!fetched.ok) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    error_ = fetched.error;
                    state_ = State::Failed;
                    workerRunning_ = false;
                    LOGW("[RemoteGeoJson] 加载失败 source=%s error=%s", sourceLogName_.c_str(), error_.c_str());
                    return;
                }
                effectivePath_ = fetched.localPath;
            }
            // @Jin extent 变化只让 GDAL 重读本地缓存，不取消/重启 URL 下载，不产生重复网络请求。
            if (stale_.load()) continue;
            readPath = effectivePath_;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = State::Reading;
        }
        // 逐要素标注覆盖：从 style_.featureOverrides 提出 fid→标注字段名（含空串=显式关闭），下传读取器按 fid 取文本。
        // 无覆盖（空）传 nullptr → 读取器仅按整层 labelField（零回归）。
        std::unordered_map<long long, std::string> labelOv;
        for (const auto &kv : style_.featureOverrides)
            if (kv.second.hasLabel) labelOv[kv.first] = kv.second.labelField;
        VectorReadResult r = readVectorFile(readPath, style_.labelField, hasExtent, a, b, c, d, cap,
                                            labelOv.empty() ? nullptr : &labelOv);
        const auto t1 = std::chrono::steady_clock::now();
        if (abort_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            workerRunning_ = false;
            return;
        }
        if (!r.ok) {
            std::lock_guard<std::mutex> lk(mtx_);
            error_ = r.error.empty() ? "矢量读取失败" : r.error;
            state_ = State::Failed;
            workerRunning_ = false;
            LOGW("VectorLoader 读取失败: %s (%s)", sourceLogName_.c_str(), error_.c_str());
            return;
        }
        // [VecReload] 抢占检查点①：读完到建几何前，若已收到更新的屏幕范围，直接丢弃本轮过期结果，不进入 build。
        if (stale_.load()) {
            LOGI("[VecReload] 抢占中断(读后): %s read=%lldms 已过期，按最新范围重跑",
                 sourceLogName_.c_str(), std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = State::Building;
        }
        VectorGeometry g = buildGeometry(r, style_, &stale_);
        const auto t2 = std::chrono::steady_clock::now();
        // [VecReload] 抢占检查点②：建几何期间被 reload 置真，本轮结果过期且可能不完整（buildGeometry 已在要素边界 break），
        // 丢弃不发布（旧几何由渲染层继续显示，无空窗），立即按最新范围重跑——这是“移到新位置秒级跟上”的关键。
        if (stale_.load()) {
            LOGI("[VecReload] 抢占中断(建后): %s build=%lldms 过期结果丢弃，按最新范围重跑",
                 sourceLogName_.c_str(), std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
            continue;
        }
        // 在 move 入 geom_ 前捕获顶点量（geom_ 移交后可能被 GL 线程 drainReady 并发取走，锁外不可再读）
        const size_t fillVertN = g.fillVerts.size() / kVecFloatsPerVertex;
        const size_t lineVertN = g.lineVerts.size() / kLineFloatsPerVertex;
        if (abort_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            workerRunning_ = false;
            return;
        }
        bool rerun;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Swap-on-ready：新几何建好后于锁内一次性替换 geom_ 并置 Ready；在此之前旧几何
            // （已被 drainReady 取走则由渲染层 vl.geom 保留）仍可绘制，GL 线程 drainReady 后同帧重传→无空窗。
            geom_ = std::move(g);
            state_ = State::Ready;
            rerun = rerunRequested_;
            if (rerun) continue; // 加载期间又收到新范围：按最新范围再来一轮
            workerRunning_ = false;
        }
        // [诊断] 新位置显示慢定位：read=GDAL 打开+空间过滤扫描+重投影；build=earcut 三角剖分+顶点烘焙（已不简化，逐要素并行）。
        // features/顶点量偏大时 build 与后续 GL 上传会是主要耗时。rerun=1 表示本轮未上传即又收到新范围重读。
        LOGI("[VecReload] VectorLoader 就绪: %s extent=%d features=%zu read=%lldms build=%lldms total=%lldms fillVert=%zu lineVert=%zu rerun=%d",
             sourceLogName_.c_str(), hasExtent ? 1 : 0, r.features.size(),
             std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count(),
             fillVertN, lineVertN,
             rerun ? 1 : 0);
        return;
    }
}

} // namespace globecore
