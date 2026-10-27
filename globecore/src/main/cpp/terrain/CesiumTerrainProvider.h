#ifndef GLOBECORE_TERRAIN_CESIUMTERRAINPROVIDER_H
#define GLOBECORE_TERRAIN_CESIUMTERRAINPROVIDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "terrain/CesiumTerrain.h"

namespace globecore {

/**
 * @Jin C++ 地形调度所需的相机视口快照。
 * @Jin C++ Provider 自行决定请求中心瓦片与外围瓦片，平台侧不再计算 7×7 窗口。
 */
struct TerrainViewportSnapshot {
    bool valid = false;
    int viewMode = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double tilt = 0.0;
    double displayZoom = 0.0;
};

/** @Jin Cesium quantized-mesh 地形源配置；地址和请求头由宿主注入。 */
struct TerrainProviderOptions {
    std::string baseUrl;
    std::string cacheDir;
    int maxZoom = 15;
    int concurrency = 4;
    int tileRadius = 3;
    std::vector<std::pair<std::string, std::string>> headers;
};

/** @Jin Provider 可观测状态快照，用于 Harmony/Android/Qt 统一诊断缓存与请求行为。 */
struct TerrainProviderStats {
    bool enabled = false;
    bool metadataLoaded = false;
    int maxZoom = 15;
    int engineZoom = 0;
    int requestedEngineTiles = 0;
    int requestedServiceTiles = 0;
    int deliveredTiles = 0;
    int diskHits = 0;
    int metadataDiskHits = 0;
    int metadataNetworkRequests = 0;
    int networkRequests = 0;
    int retries = 0;
    int cacheWrites = 0;
    int availableSkips = 0;
    int duplicateSkips = 0;
    int failures = 0;
    // @Jin 本轮首块 DEM 注入耗时；用于区分“首块出现慢”和“外围补齐慢”。
    long long firstTileMs = -1;
    long long elapsedMs = 0;
};

/**
 * @Jin 平台无关的 Cesium 地形 Provider。
 * @Jin 调度线程按当前相机中心由内向外生成任务；固定并发 worker 先查磁盘缓存，未命中才下载，
 * @Jin 成功块立即通过回调注入引擎，同一服务 DEM 在窗口内和跨刷新 LRU 中均不会重复下载/解码。
 */
class CesiumTerrainProvider {
public:
    using ViewportProvider = std::function<TerrainViewportSnapshot()>;
    using TileSink = std::function<void(int z, int x, int y,
                                         const std::vector<uint8_t> &bytes,
                                         const TerrainGeoBounds &bounds)>;
    /// @Jin 统一地形事件回调：参数为单行 JSON，含 event 与完整 stats；平台桥接只负责线程投递。
    using EventCallback = std::function<void(const std::string &eventJson)>;

    CesiumTerrainProvider(TerrainProviderOptions options,
                          ViewportProvider viewportProvider,
                          TileSink tileSink);
    ~CesiumTerrainProvider();

    CesiumTerrainProvider(const CesiumTerrainProvider &) = delete;
    CesiumTerrainProvider &operator=(const CesiumTerrainProvider &) = delete;

    /** @Jin 开关地形；关闭时中断在途 curl、丢弃未开始任务并清空已交付 LRU。 */
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_.load(); }

    /** @Jin 请求一次最新视口刷新；连续请求自动合并，只保留最后一次。 */
    void requestRefresh();

    /** @Jin 计算当前视口实际会选出的中心引擎瓦片签名。
     * @Jin 签名只包含 viewMode、引擎 z 和中心 x/y；当前固定半径选块不消费 tilt/heading，避免无谓中断 DEM 下载。
     * @Jin */
    uint64_t viewportTaskSignature(const TerrainViewportSnapshot &viewport) const;

    /** @Jin 空闲时预取 layer.json，供用户首次打开地形时直接复用内存/磁盘元数据。 */
    void prefetchMetadata();

    /** @Jin 引擎清空地形缓存后同步清空 Provider 的已交付记忆。 */
    void resetDeliveredTiles();

    /** @Jin 注册/替换统一地形事件回调；传空 function 反注册。回调可能来自调度或 worker 线程。 */
    void setEventCallback(EventCallback callback);

    /** @Jin 线程安全读取状态快照。 */
    TerrainProviderStats stats() const;

    /** @Jin 返回稳定的单行 JSON，供 NAPI/JNI 以字符串向宿主透出诊断状态。 */
    std::string statsJson() const;

private:
    struct Task {
        int engineZ = 0;
        int engineX = 0;
        int engineY = 0;
        TerrainServiceTile service;
        TerrainGeoBounds bounds;
    };

    void dispatcherLoop();
    bool loadMetadata(const std::shared_ptr<std::atomic<bool>> &abort);
    bool fetchTile(const TerrainServiceTile &tile,
                   const std::shared_ptr<std::atomic<bool>> &abort,
                   std::vector<uint8_t> &bytes);
    bool fetchNetwork(const std::string &url,
                      const std::shared_ptr<std::atomic<bool>> &abort,
                      std::vector<uint8_t> &bytes);
    void runRefresh(const TerrainViewportSnapshot &viewport, uint64_t generation);
    std::vector<Task> buildTasks(const TerrainViewportSnapshot &viewport, int &engineZoom);
    std::string tileUrl(const TerrainServiceTile &tile) const;
    std::string cachePath(const TerrainServiceTile &tile) const;
    std::string metadataCachePath() const;

    bool isDelivered(const std::string &key);
    void markDelivered(const std::string &key);
    void abortInFlight();
    void emitEvent(const std::string &event, const std::string &extraJson = {}) const;

    TerrainProviderOptions options_;
    ViewportProvider viewportProvider_;
    TileSink tileSink_;

    mutable std::mutex eventMtx_;
    EventCallback eventCallback_;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> refreshPending_{false};
    std::atomic<bool> metadataPrefetchPending_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<long long> lastElapsedMs_{0};
    std::atomic<long long> lastFirstTileMs_{-1};
    std::atomic<int> lastEngineZoom_{0};

    mutable std::mutex waitMtx_;
    std::condition_variable waitCv_;
    std::thread dispatcherThread_;

    mutable std::mutex abortMtx_;
    std::shared_ptr<std::atomic<bool>> currentAbort_;

    mutable std::mutex metaMtx_;
    CesiumTerrainMeta meta_;
    bool metadataLoaded_ = false;

    mutable std::mutex deliveredMtx_;
    std::list<std::string> deliveredOrder_;
    std::unordered_map<std::string, std::list<std::string>::iterator> delivered_;

    std::atomic<int> statRequestedEngineTiles_{0};
    std::atomic<int> statRequestedServiceTiles_{0};
    std::atomic<int> statDeliveredTiles_{0};
    std::atomic<int> statDiskHits_{0};
    std::atomic<int> statMetadataDiskHits_{0};
    std::atomic<int> statMetadataNetworkRequests_{0};
    std::atomic<int> statNetworkRequests_{0};
    std::atomic<int> statRetries_{0};
    std::atomic<int> statCacheWrites_{0};
    std::atomic<int> statAvailableSkips_{0};
    std::atomic<int> statDuplicateSkips_{0};
    std::atomic<int> statFailures_{0};

    int effectiveConcurrency_ = 4;
};

} // namespace globecore

#endif // GLOBECORE_TERRAIN_CESIUMTERRAINPROVIDER_H
