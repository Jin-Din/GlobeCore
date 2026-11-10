#ifndef GLOBECORE_TILESET3D_TILESET3DPROVIDER_H
#define GLOBECORE_TILESET3D_TILESET3DPROVIDER_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "geom/Matrix4.h" // @Jin FrustumPlane 定义于此，供视锥裁剪使用。
#include "geom/Vec3.h"
#include "tileset3d/Tileset3DGraph.h"
#include "tileset3d/Tileset3DModel.h"

namespace globecore {

/**
 * @Jin 3D Tiles Provider 的相机快照；眼点/视口高/FOV 用于 SSE，视锥 6 平面用于保守剔除。
 */
struct Tileset3DViewport {
    bool valid = false;
    Vec3 eyeEcef;
    int viewportHeightPx = 1;
    double fovYRad = 0.7853981633974483;
    // @Jin hasFrustum=false 时选择器跳过视锥裁剪（离线验证或相机未就绪场景），避免零平面误剔。
    bool hasFrustum = false;
    std::array<FrustumPlane, 6> frustumPlanes{};
};

/**
 * @Jin 最小异步 3D Tiles Provider：加载根/外部 tileset，按 SSE 选择 b3dm，后台下载并构建 CPU 模型。
 * @Jin P2 暂不做磁盘缓存和 LRU 卸载，先验证渲染闭环；后续在本类扩展，不影响 Renderer 接口。
 */
class Tileset3DProvider {
public:
    using ViewportProvider = std::function<Tileset3DViewport()>;
    using ModelSink = std::function<void(Tileset3DModelCpu &&)>;
    using RenderRequest = std::function<void()>;

    Tileset3DProvider(int layerId, std::string rootUrl,
                      ViewportProvider viewportProvider, ModelSink sink,
                      RenderRequest renderRequest);
    ~Tileset3DProvider();

    Tileset3DProvider(const Tileset3DProvider &) = delete;
    Tileset3DProvider &operator=(const Tileset3DProvider &) = delete;

    void start();
    void stop();
    // @Jin GL 帧线程调用；仅当相机量化签名变化时唤醒 worker，避免静态画面空转联网。
    void updateViewport(const Tileset3DViewport &viewport);
    void requestRefresh();

private:
    struct ExternalGraph {
        std::string url;
        std::unique_ptr<Tileset3DGraph> graph;
    };

    void workerLoop();
    bool ensureRootGraph();
    bool ensureExternalGraph(const Tileset3DSelection &selection);
    void refresh(const Tileset3DViewport &viewport);
    // @Jin gltfUpAxis 在锁内选择阶段复制出来，锁外下载/构建不再回查图，规避图指针生命周期问题。
    bool loadModel(const std::string &gltfUpAxis, const Tileset3DSelection &selection);

    int layerId_ = -1;
    std::string rootUrl_;
    ViewportProvider viewportProvider_;
    ModelSink sink_;
    RenderRequest renderRequest_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> refreshPending_{true};
    std::atomic<uint64_t> lastViewportSignature_{0};
    std::thread worker_;
    mutable std::mutex waitMtx_;
    std::condition_variable waitCv_;

    std::mutex dataMtx_;
    std::unique_ptr<Tileset3DGraph> rootGraph_;
    std::unordered_map<std::string, std::unique_ptr<ExternalGraph>> externalGraphs_;
    std::unordered_set<std::string> loadedModelUrls_;
    std::string lastError_;
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DPROVIDER_H
