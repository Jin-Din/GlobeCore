#ifndef GLOBECORE_LAYER_TILE_LOADER_H
#define GLOBECORE_LAYER_TILE_LOADER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace globecore {

class TileCache;

/**
 * 后台瓦片加载器，对应 参考实现 layer.cache 的「磁盘/网络 TileStore + 异步加载管线」。
 *
 * 职责：异步取瓦片字节（先读磁盘缓存，未命中再联网下载并写盘）→ 交付 GL 线程上传纹理。
 * 关键：渲染（GL）线程绝不同步读盘/联网，只通过 [request] 投递需求、[drainReady] 取就绪字节（对齐 参考实现
 * getTexture 不阻塞渲染线程）；未就绪的瓦片由 Renderer 用祖先纹理兜底显示。
 *
 * 线程模型：构造时启动 numThreads 个工作线程共享一个请求队列；[request] 按 (z,x,y) 去重，
 * 已在队列或进行中的瓦片不会重复请求。工作线程先用 [TileCache] 读盘，未命中再用 [HttpClient] 拉取，
 * 成功则（联网时）原子写盘并放入 ready 队列；GL 线程每帧调用 [drainReady] 取走就绪字节做解码上传。
 * 所有公开方法线程安全。
 *
 * URL 模板：形如天地图 WMTS，含 `{x}`/`{y}`/`{z}` 与 `{rand=0,1,...}`（子域轮询）；
 * `$tiandituToken` 等 Token 由上层（Kotlin resolveUrl）替换后再经 [setUrlTemplate] 传入。
 * 空模板表示禁用联网（Phase B 行为：只读磁盘缓存）。
 */
class TileLoader {
public:
    /// 一块下载并解码就绪的 RGBA 瓦片，交由 GL 线程上传纹理。
    /// 解码（PNG/JPEG→RGBA）在 worker 线程完成，GL 线程只做 glTexImage2D 上传，
    /// 避免一扎瓦片同时就绪时 GL 线程集中解码丢帧（对齐 Cesium/maplibre 的 off-GL-thread 解码）。
    struct ReadyTile {
        int z = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        uint64_t generation = 0; // @Jin: 标记瓦片由哪一个视口代际产出，GL 线程优先交付当前代际结果。
        std::vector<uint8_t> rgba;  // 已解码 RGBA8888（width*height*4）
    };

    /// @Jin 瓦片管线快照：Renderer 按固定周期聚合打印，用于量化每帧上传、缓存命中、网络完成与失败。
    struct StatsSnapshot {
        uint64_t processed = 0;
        uint64_t cacheHits = 0;
        uint64_t networkHits = 0;
        uint64_t rasterGenerated = 0;
        uint64_t failures = 0;
        size_t pending = 0;
        size_t inflight = 0;
        size_t ready = 0;
    };

    explicit TileLoader(TileCache &cache, int numThreads = 8);
    ~TileLoader();

    TileLoader(const TileLoader &) = delete;
    TileLoader &operator=(const TileLoader &) = delete;

    /// 设置 URL 模板（含占位符，Token 已替换）。空模板禁用联网。线程安全。
    void setUrlTemplate(std::string urlTemplate);
    bool hasUrlTemplate() const;

    /// 设置栅格数据源路径（本地 tif/img）。非空时，磁盘缓存未命中的瓦片由 [RasterRenderer] 按 (z,x,y)
    /// 的全球墨卡托经纬度四至即时重投影生成 PNG 字节（GDAL），成功后回写缓存。空路径禁用栅格生成。
    /// 与 URL 模板互斥使用（栅格层不设 URL）。线程安全。
    /// [hasEnv]+[minLon,minLat,maxLon,maxLat]：栅格 WGS84 四至（调用方已由 info() 取得），hasEnv=true 时
    /// request() 在派发端对无交集瓦片直接剪枝（复刻 WWK TiledSurfaceImage 的 sector 剪枝，免空白瓦进 GDAL）；
    /// hasEnv=false 则不剪枝（保守全放行）。
    void setRasterSource(std::string path, bool hasEnv = false,
                         double minLon = 0.0, double minLat = 0.0,
                         double maxLon = 0.0, double maxLat = 0.0);
    bool hasRasterSource() const;

    /// 请求某瓦片（后台异步拉取）。已排队/进行中的会去重跳过；无模板或关机中直接忽略。
    /// 失败退避：读盘+联网（或栅格生成）均失败的瓦片在冷却窗口内（kFailCooldown）直接忽略重复请求，
    /// 防止大范围视图（3D 球面）触发服务限流（HTTP 429）后「失败→纹理驱逐→重请求→再失败」请求风暴死循环。
    void request(int z, int x, int y);

    /// @Jin: 声明当前帧/当前视口真正需要的瓦片集合签名；签名变化时取消尚未开工的旧请求，
    /// 避免初始化相机或连续手势期间把多个过期视口的瓦片全部下载，造成请求量突增和首屏 GL 上传抖动。
    void beginView(uint64_t viewSignature);

    /// 取走就绪瓦片（GL 线程每帧调用）。out 追加，不清空原有内容。
    /// @Jin [maxCount]/[maxBytes] 共同限制本次上传量；当前视口代际优先，未取走的留到后续帧。
    /// @Jin maxBytes 让 Renderer 可跨多个瓦片层共享同一帧显存上传预算，避免双层各自 32 块造成 GL 尖峰。
    void drainReady(std::vector<ReadyTile> &out, int maxCount = -1,
                    size_t maxBytes = std::numeric_limits<size_t>::max());

    /// @Jin 线程安全读取聚合统计，供 Renderer 输出请求/缓存/上传诊断。
    StatsSnapshot stats() const;

    /// 是否仍有未完成的瓦片工作（排队/下载中 或 就绪待上传）。供 Renderer 判断是否需再画一帧：
    /// RENDERMODE_WHEN_DIRTY 省电模式下，瓦片异步到位需据此触发重绘；全部就绪后返回 false 停绘。
    bool hasPendingOrReady();

    /// @Jin: 丢弃尚未开工的全部请求与已就绪字节（层被宿主移除时用），并递增视口代际；
    /// @Jin: 排队项/去重集/在途表/失败退避表全部清空，在途任务即使返回也会因代际过旧被丢弃。
    void cancelPending();

private:
    struct Request {
        int z, x, y;
        uint64_t generation = 0; // @Jin: 入队时所属视口代际，出队后用于识别过期请求。
    };

    static uint64_t key(int z, int x, int y);
    void workerLoop();
    std::string buildUrl(int z, int x, int y);

    TileCache &cache_;
    const int numThreads_;

    // 请求队列 + 去重集合（同一把锁保护）
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<uint64_t> pending_;
    // @Jin 排队项 key -> 最新需要它的视口代际；新视口复用同 key 时原地升级，无需清空重排。
    std::unordered_map<uint64_t, uint64_t> pendingGeneration_;
    // @Jin: 在途瓦片 key -> 视口代际；视口切换后仍被新视口需要时可原地升级，避免同 key 重复联网。
    std::unordered_map<uint64_t, uint64_t> inflight_;
    uint64_t viewGeneration_ = 1;
    uint64_t viewSignature_ = 0;
    bool hasViewSignature_ = false;
    // 失败退避表（mtx_ 保护）：key → 冷却截止时刻；成功交付或到期后移除
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> failedUntil_;
    bool stop_ = false;
    std::vector<std::thread> workers_;

    // 下载就绪队列（独立锁，GL 线程 drain、工作线程 push）
    mutable std::mutex readyMtx_; // @Jin stats() 为 const 诊断接口，仍需读取 ready_ 数量。
    std::deque<ReadyTile> ready_;
    mutable std::unordered_set<uint64_t> readyKeys_; // @Jin 已解码待上传瓦片去重，const stats 诊断时也可防御性修复集合。

    // URL 模板（独立锁）+ 子域轮询计数 + 栅格数据源路径与四至（共用 urlMtx_：二者互斥、均后台读）
    mutable std::mutex urlMtx_;
    std::string urlTemplate_;
    std::string rasterPath_;
    // 栅格四至（minLon,minLat,maxLon,maxLat）及有效性：供 request() 交集预判（urlMtx_ 保护）
    bool rasterEnvOk_ = false;
    double rasterEnv_[4] = {0.0, 0.0, 0.0, 0.0};
    std::atomic<uint32_t> randCounter_{0};

    // 关机中断标志：置位后 curl 传输尽快中断，令 join 不长时间阻塞
    std::atomic<bool> abort_{false};

    // 诊断采样计数（排查瓦片去向）：本 loader 前若干块处理瓦片打一条带来源标识的日志，之后静默
    std::atomic<int> diagSample_{32};

    // @Jin: 聚合统计瓦片来源，用来核实重复浏览是否命中磁盘缓存，避免只凭后台请求总量误判缓存失效。
    std::atomic<uint64_t> statCacheHits_{0};
    std::atomic<uint64_t> statNetworkHits_{0};
    std::atomic<uint64_t> statRasterGenerated_{0};
    std::atomic<uint64_t> statFailures_{0};
    std::atomic<uint64_t> statProcessed_{0};
};

} // namespace globecore

#endif // GLOBECORE_LAYER_TILE_LOADER_H
