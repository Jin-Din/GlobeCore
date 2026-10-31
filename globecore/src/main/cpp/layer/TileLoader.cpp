#include "layer/TileLoader.h"

#include "layer/TileCache.h"
#include "globe/MercatorProjection.h"
#include "net/HttpClient.h"
#include "raster/RasterRenderer.h"
#include "render/ImageDecoder.h"
#include "render/PngEncoder.h" // @Jin 4326/4490 地理源重采样结果编码（GDAL 无关，LITE 可用）
#include "util/Log.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace globecore {

namespace {
/// 失败退避冷却时长：读盘+联网均失败后，同 key 请求在此窗口内直接忽略（见 TileLoader::request 注释）
constexpr auto kFailCooldown = std::chrono::seconds(30);

/// 就绪队列高水位：ready_ 是 GL 线程 drainReady（每帧仅取 kMaxDecodePerFrame）与工作线程 push 之间的
/// 缓冲。3D 倾斜视域下 coverage-first 使可见叶暴涨到数千，工作线程下载速度远超 GL 解码消费，无界 ready_
/// 会堆积上万块下载字节（每块 ~10-20KB）撑爆 native 堆（实测 Scudo exhausted 256M 崩溃）。超水位时丢弃
/// 本次交付——联网字节已 writeTile 落盘，后续重请求走读盘秒得，仅牺牲一批陈旧字节的即时性，杜绝 OOM。
constexpr size_t kMaxReadyTiles = 1024;
/// @Jin drape20 实验参数：跨级别 flyTo 后旧视口最多保留最近 64 块已解码 RGBA，避免稳定后继续向 GPU 上传上千块不可见旧瓦。
constexpr size_t kMaxStaleReadyTiles = 64;
constexpr uint64_t kMaxStaleDecodeGenerationAge = 2; // @Jin worker 解码前最多容忍落后 2 代；更老结果只保留磁盘缓存，不再占用 RGBA 内存和 GL 上传。
/// 请求队列上限：3D 连续手势下视角每帧一换（一帧可见集可达数百块），过期请求会在队列里
/// 大量堆积；超限时从队首丢弃最旧的（属于已滑出的旧视角，下帧起不再需要），为当前视角让路。
constexpr size_t kMaxQueue = 1200;

/// 图像格式魔数校验（PNG/JPEG/WEBP/GIF/BMP）：拦截图源「HTTP 200 + 异常 XML/HTML 错误页」
/// 被当作有效瓦片落盘污染缓存（实测天地图限流时回 200+约 100B 的 ExceptionReport）。
bool looksLikeImage(const std::vector<uint8_t> &b) {
    if (b.size() < 12) return false;
    const auto *p = b.data();
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return true;                       // PNG
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;                                     // JPEG
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8') return true;                         // GIF
    if (p[0] == 'B' && p[1] == 'M') return true;                                                       // BMP
    if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') return true;                       // WEBP
    return false;
}

/// 全球墨卡托瓦片 (z,x,y) → WGS84 经纬度四至（标准 XYZ，y 自北向南）。
/// 归一化世界坐标下瓦片覆盖 [x/2^z,(x+1)/2^z]×[y/2^z,(y+1)/2^z]；wy 小端为北侧（大纬度）。
void tileBounds(int z, int x, int y, double &minLon, double &minLat, double &maxLon, double &maxLat) {
    const double n = static_cast<double>(1LL << z); // 2^z（与 TileMatrix::tilesAtLevel 同口径）
    double lonNw = 0.0, latNw = 0.0, lonSe = 0.0, latSe = 0.0;
    MercatorProjection::worldToLonLat(x / n, y / n, lonNw, latNw);            // 西北角 → maxLat
    MercatorProjection::worldToLonLat((x + 1) / n, (y + 1) / n, lonSe, latSe); // 东南角 → minLat
    minLon = lonNw; maxLon = lonSe; maxLat = latNw; minLat = latSe;
}
} // namespace
/// @Jin EPSG:4326/4490 地理在线源：把 URL 模板（{z}/{x}/{y} 与 {rand=a,b,..}）实例化。
/// 普通墨卡托层与地理适配层共用同一套占位符替换，保证两种图源的子域轮询/Token 行为一致。
std::string formatTileUrl(std::string url, int z, int x, int y, std::atomic<uint32_t> &randCounter) {
    if (url.empty()) return std::string();
    const size_t rp = url.find("{rand=");
    if (rp != std::string::npos) {
        const size_t rpEnd = url.find('}', rp);
        if (rpEnd != std::string::npos) {
            const std::string list = url.substr(rp + 6, rpEnd - (rp + 6));
            std::vector<std::string> opts;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                if (comma == std::string::npos) { opts.push_back(list.substr(start)); break; }
                opts.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
            std::string chosen;
            if (!opts.empty()) chosen = opts[randCounter.fetch_add(1) % static_cast<uint32_t>(opts.size())];
            url.replace(rp, rpEnd - rp + 1, chosen);
        }
    }
    auto replaceAll = [](std::string &s, const char *from, const std::string &to) {
        const size_t flen = std::string(from).size();
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, flen, to); pos += to.size(); }
    };
    replaceAll(url, "{z}", std::to_string(z));
    replaceAll(url, "{x}", std::to_string(x));
    replaceAll(url, "{y}", std::to_string(y));
    return url;
}

/// @Jin 0 基地理层级 s（根 2×1）：列数 2^(s+1)、行数 2^s；标准 XYZ（y=0 在北）。
inline void geoTileBoundsXYZ(int s, int x, int y,
                              double &lonMin, double &latMin, double &lonMax, double &latMax) {
    const double cols = std::ldexp(1.0, s + 1);
    const double rows = std::ldexp(1.0, s);
    lonMin = -180.0 + 360.0 * static_cast<double>(x) / cols;
    lonMax = -180.0 + 360.0 * static_cast<double>(x + 1) / cols;
    latMax =  90.0 - 180.0 * static_cast<double>(y) / rows;
    latMin =  90.0 - 180.0 * static_cast<double>(y + 1) / rows;
}

/// @Jin 单个引擎墨卡托瓦允许映射的最大地理服务瓦数，防止高纬/异常请求放大（正常近赤道约 1~4 块）。
constexpr int kMaxGeoSourceTiles = 64;


uint64_t TileLoader::key(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 40) |
           (static_cast<uint64_t>(x) << 20) |
           static_cast<uint64_t>(y);
}

TileLoader::TileLoader(TileCache &cache, int numThreads)
    : cache_(cache), numThreads_(numThreads > 0 ? numThreads : 1) {
    HttpClient::globalInit();
    workers_.reserve(static_cast<size_t>(numThreads_));
    for (int i = 0; i < numThreads_; ++i) {
        workers_.emplace_back(&TileLoader::workerLoop, this);
    }
    LOGI("TileLoader started threads=%d", numThreads_);
}

TileLoader::~TileLoader() {
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable()) t.join();
    }
    LOGI("TileLoader stopped");
}

void TileLoader::setUrlTemplate(std::string urlTemplate) {
    std::lock_guard<std::mutex> lk(urlMtx_);
    urlTemplate_ = std::move(urlTemplate);
    LOGI("TileLoader urlTemplate set (len=%zu, net=%s)",
         urlTemplate_.size(), urlTemplate_.empty() ? "off" : "on");
}

bool TileLoader::hasUrlTemplate() const {
    std::lock_guard<std::mutex> lk(urlMtx_);
    return !urlTemplate_.empty();
}

void TileLoader::setRasterSource(std::string path, bool hasEnv,
                                 double minLon, double minLat, double maxLon, double maxLat) {
    std::lock_guard<std::mutex> lk(urlMtx_);
    rasterPath_ = std::move(path);
    // 四至仅在栅格源有效时参与剪枝；换源即重置（空路径禁用生成，亦不剪枝）
    rasterEnvOk_ = hasEnv && !rasterPath_.empty();
    rasterEnv_[0] = minLon; rasterEnv_[1] = minLat;
    rasterEnv_[2] = maxLon; rasterEnv_[3] = maxLat;
    LOGI("TileLoader rasterSource set (len=%zu, gen=%s, envOk=%d [%.5f,%.5f,%.5f,%.5f])",
         rasterPath_.size(), rasterPath_.empty() ? "off" : "on",
         rasterEnvOk_ ? 1 : 0, rasterEnv_[0], rasterEnv_[1], rasterEnv_[2], rasterEnv_[3]);
}

bool TileLoader::hasRasterSource() const {
    std::lock_guard<std::mutex> lk(urlMtx_);
    return !rasterPath_.empty();
}

void TileLoader::beginView(uint64_t viewSignature) {
    // @Jin 视口变化只推进代际，不立刻清空队列/已解码结果：连续手势时同 key 可原地升级，
    // @Jin 已经下载并解码的可见瓦片也不会因相邻两帧视口微调而反复丢弃、重读、重解码。
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (hasViewSignature_ && viewSignature_ == viewSignature) return;
        hasViewSignature_ = true;
        viewSignature_ = viewSignature;
        ++viewGeneration_;
    }
}

void TileLoader::request(int z, int x, int y) {
    // 不再因无 URL 模板而跳过：磁盘读取也走本异步管线（GL 线程绝不同步读盘，对齐 参考实现 getTexture 不阻塞渲染线程）。

    // 栅格层 envelope 预判（复刻 WWK TiledSurfaceImage 的 sector 剪枝）：瓦片四至与栅格四至无交集
    // 则根本不入队——这类瓦片生成端注定无字节（永久失败），剪枝免其占队列/工作线程并进 GDAL 空转。
    // 判据保守：边界接触算相交（防误剔边缘瓦）；经度跨度>350° 疑跨 180° 经线时停用剪枝（取极值
    // 包络不可信，全放行交给生成端自行判交集）。先取 urlMtx_ 完成判断后释放，再取 mtx_，不嵌套锁。
    bool prune = false;
    {
        std::lock_guard<std::mutex> lk(urlMtx_);
        // 跨度>350° 视为疑跨 180° 经线（取极值包络不可信），rasterEnvOk_ 置伪同效：不剪枝全放行
        if (rasterEnvOk_ && rasterEnv_[2] - rasterEnv_[0] <= 350.0) {
            double tMinLon = 0.0, tMinLat = 0.0, tMaxLon = 0.0, tMaxLat = 0.0;
            tileBounds(z, x, y, tMinLon, tMinLat, tMaxLon, tMaxLat);
            prune = tMaxLon < rasterEnv_[0] || tMinLon > rasterEnv_[2] ||
                    tMaxLat < rasterEnv_[1] || tMinLat > rasterEnv_[3];
        }
    }
    if (prune) return;

    const uint64_t k = key(z, x, y);
    {
        // @Jin 先在 ready 集合去重：瓦片已解码但受单帧预算限制尚未上传时，下一帧不应再次读盘/联网。
        std::lock_guard<std::mutex> readyLock(readyMtx_);
        if (readyKeys_.count(k) != 0) return;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        if (pending_.count(k) != 0) {
            // @Jin 新视口仍需要同 key 时只升级排队代际，保留原队列位置并避免重复请求。
            pendingGeneration_[k] = viewGeneration_;
            return;
        }
        auto inFlight = inflight_.find(k);
        if (inFlight != inflight_.end()) {
            // @Jin: 旧视口已在读取/下载、但新视口仍需要该瓦时，直接把在途任务升级为新代际，避免重复请求。
            inFlight->second = viewGeneration_;
            return;
        }
        // 失败退避：冷却窗口内不重复请求；到期则放行本次重试（下次仍失败会重新记入）
        auto fit = failedUntil_.find(k);
        if (fit != failedUntil_.end()) {
            if (std::chrono::steady_clock::now() < fit->second) return;
            failedUntil_.erase(fit);
        }
        pending_.insert(k);
        pendingGeneration_[k] = viewGeneration_;
        queue_.push_back(Request{z, x, y, viewGeneration_});
        // 超限丢最旧：被丢者同步移出 pending_，若仍可见下一帧会重新入队（去重不受损）
        while (queue_.size() > kMaxQueue) {
            const Request &oldest = queue_.front();
            const uint64_t oldestKey = key(oldest.z, oldest.x, oldest.y);
            pending_.erase(oldestKey);
            pendingGeneration_.erase(oldestKey);
            queue_.pop_front();
        }
    }
    cv_.notify_one();
}

void TileLoader::drainReady(std::vector<ReadyTile> &out, int maxCount, size_t maxBytes) {
    int taken = 0;
    size_t bytes = 0;
    uint64_t currentGeneration = 0;
    {
        // @Jin 先在独立临界区读取代际，避免持有 readyMtx_ 后再嵌套 mtx_，与 stats() 的加锁顺序保持一致。
        std::lock_guard<std::mutex> lk(mtx_);
        currentGeneration = viewGeneration_;
    }
    std::lock_guard<std::mutex> lk(readyMtx_);
    // @Jin drape20：当前视口结果全部优先；旧视口仅保留最近一小段，防止全球 flyTo 到近景后上千块旧 RGBA 被逐帧上传。
    size_t staleReadyCount = 0;
    for (const auto &tile : ready_) {
        if (tile.generation != currentGeneration) ++staleReadyCount;
    }
    if (staleReadyCount > kMaxStaleReadyTiles) {
        size_t droppedStale = 0;
        const size_t needDrop = staleReadyCount - kMaxStaleReadyTiles;
        for (auto it = ready_.begin(); it != ready_.end() && droppedStale < needDrop;) {
            if (it->generation == currentGeneration) {
                ++it;
                continue;
            }
            readyKeys_.erase(key(it->z, it->x, it->y));
            it = ready_.erase(it);
            ++droppedStale;
        }
        if (droppedStale > 0) {
            LOGI("TileLoader @Jin drop stale ready tiles=%zu remaining=%zu currentGen=%llu",
                 droppedStale, ready_.size(), static_cast<unsigned long long>(currentGeneration));
        }
    }
    // @Jin 两遍扫描：先交付当前视口结果，再用剩余预算接收最近旧视口结果；旧结果仍可进入纹理缓存，
    // @Jin 不再像旧实现那样在相邻手势帧之间直接丢弃，从而消除“加载中越操作越不显示”的抖动。
    const auto takePass = [&](bool currentPass) {
        for (auto it = ready_.begin(); it != ready_.end();) {
            if (maxCount >= 0 && taken >= maxCount) break;
            const bool isCurrent = (it->generation == currentGeneration);
            if (isCurrent != currentPass) {
                ++it;
                continue;
            }
            const size_t tileBytes = static_cast<size_t>(std::max(0, it->width)) *
                                     static_cast<size_t>(std::max(0, it->height)) * 4ULL;
            // @Jin 至少允许首块通过，避免单张高位图大于整帧预算时长期饿死；其余块严格受剩余字节限制。
            const size_t remainingBytes = bytes < maxBytes ? maxBytes - bytes : 0;
            if (taken > 0 && tileBytes > remainingBytes) {
                break;
            }
            readyKeys_.erase(key(it->z, it->x, it->y));
            out.push_back(std::move(*it));
            it = ready_.erase(it);
            ++taken;
            bytes += tileBytes;
        }
    };
    takePass(true);
    takePass(false);
}

TileLoader::StatsSnapshot TileLoader::stats() const {
    StatsSnapshot snapshot;
    snapshot.processed = statProcessed_.load();
    snapshot.cacheHits = statCacheHits_.load();
    snapshot.networkHits = statNetworkHits_.load();
    snapshot.rasterGenerated = statRasterGenerated_.load();
    snapshot.failures = statFailures_.load();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshot.pending = pending_.size();
        snapshot.inflight = inflight_.size();
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        snapshot.ready = ready_.size();
        if (readyKeys_.size() != ready_.size()) {
            // @Jin 防御性修复集合：若历史异常造成不一致，以 ready_ 为准重建，避免 key 永远被误判为就绪。
            readyKeys_.clear();
            for (const auto &tile : ready_) readyKeys_.insert(key(tile.z, tile.x, tile.y));
        }
    }
    return snapshot;
}

bool TileLoader::hasPendingOrReady() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // @Jin: pending_ 只表示排队项；worker 出队后进入 inflight_，在途网络未完成时也必须持续触发补帧。
        if (!pending_.empty() || !inflight_.empty()) return true;
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        if (!ready_.empty()) return true;
    }
    return false;
}

void TileLoader::cancelPending() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
        pending_.clear();
        pendingGeneration_.clear();
        inflight_.clear();
        ++viewGeneration_;
        hasViewSignature_ = false;
        failedUntil_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        ready_.clear();
        readyKeys_.clear();
    }
    // @Jin: 不置 stop_/abort_：墓碑槽位仍持有本对象，工作线程无活可干时回到 cv_.wait 挂起。
    // @Jin: 代际已经递增，旧 GDAL/网络任务即使跑完，其结果也不会进入当前 ready 队列。
}

void TileLoader::workerLoop() {
    while (true) {
        Request req{0, 0, 0};
        bool preferCoarseOnlineTile = false;
        {
            std::lock_guard<std::mutex> urlLock(urlMtx_); // @Jin: 出队优先级只读取数据源类型，先拿 url 锁快照避免与 setUrlTemplate/setRasterSource 竞争。
            preferCoarseOnlineTile = !urlTemplate_.empty() && rasterPath_.empty();
        }
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (queue_.empty()) continue;
            // @Jin 出队前先升级仍被当前视口需要的同 key 请求，并清掉已经滑出视口的旧代际项；
            // @Jin 这样连续手势不会让过期请求占住 worker，也不会误删已由本帧 request() 原地复用的任务。
            for (auto it = queue_.begin(); it != queue_.end();) {
                const uint64_t queuedKey = key(it->z, it->x, it->y);
                const auto pendingGen = pendingGeneration_.find(queuedKey);
                if (pendingGen != pendingGeneration_.end()) it->generation = pendingGen->second;
                if (it->generation != viewGeneration_) {
                    pending_.erase(queuedKey);
                    pendingGeneration_.erase(queuedKey);
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
            if (queue_.empty()) continue;
            // @Jin 出队策略按数据源区分：在线标准 XYZ 首屏先取 z-1/z-2 祖先粗瓦，用少量瓦片快速铺满屏幕；
            // @Jin 粗瓦请求完成后会从队列消失，worker 自然转入可见级叶子，形成“先完整低清、再局部变清”。
            // @Jin 本地 GDAL 栅格仍保持最细叶子优先，因为无金字塔粗瓦可能触发整幅重投影，单块曾实测数秒以上。
            // @Jin 同级保持 request() 的插入顺序，配合 Renderer 的“屏幕中心优先”排序，先补中心再补外围。
            size_t best = 0;
            for (size_t i = 1; i < queue_.size(); ++i) {
                if (preferCoarseOnlineTile) {
                    if (queue_[i].z < queue_[best].z) best = i; // @Jin 在线图源低 z 优先。
                } else {
                    if (queue_[i].z > queue_[best].z) best = i; // @Jin 本地栅格高 z 优先。
                }
            }
            req = queue_[best];
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
            const uint64_t k = key(req.z, req.x, req.y);
            const auto pendingGen = pendingGeneration_.find(k);
            if (pendingGen != pendingGeneration_.end()) {
                req.generation = pendingGen->second; // @Jin 使用新视口升级后的代际，避免可复用任务被误判过期。
                pendingGeneration_.erase(pendingGen);
            }
            pending_.erase(k);
            inflight_[k] = req.generation;
        }

        // 诊断采样：本 loader 前若干块处理瓦片逐块打印来源（排查「栅格层零日志」归属问题），超预算自动静默
        bool geoScheme = false; // @Jin EPSG4326/4490 地理在线源标记。
        std::string rasterPath;
        std::string urlTemplateSnapshot;
        {
            std::lock_guard<std::mutex> lk(urlMtx_);
            rasterPath = rasterPath_;
            geoScheme = geoScheme_;
            urlTemplateSnapshot = urlTemplate_;
        }
        const bool sampled = diagSample_.fetch_sub(1) > 0;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto inFlight = inflight_.find(key(req.z, req.x, req.y));
            if (inFlight == inflight_.end() || inFlight->second != viewGeneration_) {
                stale = true;
            } else {
                req.generation = inFlight->second;
            }
        }
        if (stale) {
            std::lock_guard<std::mutex> lk(mtx_);
            inflight_.erase(key(req.z, req.x, req.y));
            continue;
        }
        if (sampled) {
            LOGI("TileLoader process z=%d x=%d y=%d raster=%s net=%s cacheDir=%s",
                 req.z, req.x, req.y,
                 rasterPath.empty() ? "-" : rasterPath.c_str(),
                 urlTemplateSnapshot.empty() ? "off" : "on",
                 cache_.baseDir().c_str());
        }

        // 1) 先读磁盘缓存（异步，避免 GL 线程阻塞）；命中则直接交付，不联网。
        //    魔数校验防坏文件（历史污染的异常 XML 瓦片）：检出即删除自愈（removeTile），按未命中
        //    继续走下载/失败冷却——若不过滤，读盘命中坏瓦会被解码失败却计为成功，令退避失效并逐帧重刷。
        std::vector<uint8_t> bytes;
        bool haveBytes = cache_.readTile(req.z, req.x, req.y, bytes) && !bytes.empty();
        // @Jin: 记录每块瓦片的第一来源；磁盘命中后不会进入网络分支，可直接验证缓存是否真正节省请求。
        bool fromCache = haveBytes;
        bool fromNetwork = false;
        bool fromRaster = false;
        if (haveBytes && !looksLikeImage(bytes)) {
            LOGW("磁盘瓦片损坏，已清除 z=%d x=%d y=%d size=%zu", req.z, req.x, req.y, bytes.size());
            cache_.removeTile(req.z, req.x, req.y);
            bytes.clear();
            haveBytes = false;
            // @Jin: 坏缓存会自愈为网络重取，不能计入有效缓存命中，否则会高估缓存节省的请求数。
            fromCache = false;
        }
        // 2) 磁盘未命中 → 栅格层：按 (z,x,y) 四至用 GDAL 重投影即时生成 PNG 字节，成功则回写缓存
        if (!haveBytes && !rasterPath.empty()) {
            bytes.clear();
            double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
            tileBounds(req.z, req.x, req.y, minLon, minLat, maxLon, maxLat);
            const bool genOk = RasterRenderer::renderTile(rasterPath, minLon, minLat, maxLon, maxLat, 256, bytes);
            if (sampled) {
                LOGI("TileLoader raster gen z=%d x=%d y=%d ok=%d bytes=%zu tile=[%.5f,%.5f,%.5f,%.5f]",
                     req.z, req.x, req.y, genOk ? 1 : 0, bytes.size(), minLon, minLat, maxLon, maxLat);
            }
            if (genOk && !bytes.empty()) {
                cache_.writeTile(req.z, req.x, req.y, bytes);
                haveBytes = true;
                fromRaster = true;
            }
            // 无交集/生成失败：不联网（栅格层无 URL），本瓦片无字节，由祖先兜底或保持透明
        }
        // @Jin 3′) EPSG:4326/4490 地理在线源：把墨卡托瓦映射到相交地理服务瓦，下载拼贴并重采样为该瓦 PNG。
        if (!haveBytes && rasterPath.empty() && geoScheme) {
            bytes.clear();
            bool geoAnyNet = false;
            const bool geoOk = generateGeographicTile(req.z, req.x, req.y, bytes, geoAnyNet);
            if (geoOk && !bytes.empty()) {
                cache_.writeTile(req.z, req.x, req.y, bytes); // @Jin 结果按墨卡托 (z,x,y) 落常规缓存，二次浏览零重采样。
                haveBytes = true;
                if (geoAnyNet) fromNetwork = true; else fromCache = true;
                if (sampled) {
                    LOGI("TileLoader geo4490 reproject z=%d x=%d y=%d online=%d bytes=%zu",
                         req.z, req.x, req.y, geoAnyNet ? 1 : 0, bytes.size());
                }
            }
        }

        // 3) 仍未命中 → 联网拉取（可被 abort_ 中断），成功则原子写盘。
        //    响应体须过图像魔数校验才落盘/交付：图源限流时以 HTTP 200 回异常 XML，不可当瓦片污染缓存。
        if (!haveBytes && rasterPath.empty() && !geoScheme) { // @Jin 地理源走 3′)，不再按墨卡托直连。
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto inFlight = inflight_.find(key(req.z, req.x, req.y));
                if (inFlight == inflight_.end() || inFlight->second != viewGeneration_) {
                    stale = true;
                } else {
                    req.generation = inFlight->second;
                }
            }
            if (stale) {
                std::lock_guard<std::mutex> lk(mtx_);
                inflight_.erase(key(req.z, req.x, req.y));
                continue;
            }
            bytes.clear();
            const std::string url = buildUrl(req.z, req.x, req.y);
            bool ok = !url.empty() &&
                      HttpClient::get(url, bytes, 15000, &abort_) && !bytes.empty();
            if (ok && !looksLikeImage(bytes)) {
                if (!abort_.load()) {
                    LOGW("响应非图像（图源异常/限流页）z=%d x=%d y=%d size=%zu 不落盘",
                         req.z, req.x, req.y, bytes.size());
                }
                bytes.clear();
                ok = false;
            }
            if (ok) {
                cache_.writeTile(req.z, req.x, req.y, bytes);
                haveBytes = true;
                fromNetwork = true;
            } else if (!abort_.load()) {
                LOGW("瓦片读盘/下载均失败 z=%d x=%d y=%d", req.z, req.x, req.y);
            }
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto inFlight = inflight_.find(key(req.z, req.x, req.y));
            if (inFlight == inflight_.end() || inFlight->second != viewGeneration_) {
                stale = true;
            } else {
                req.generation = inFlight->second;
            }
        }
        if (haveBytes && !stale) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                uint64_t latestGeneration = viewGeneration_;
                auto inFlight = inflight_.find(key(req.z, req.x, req.y));
                if (inFlight == inflight_.end() || inFlight->second != latestGeneration) {
                    stale = true;
                } else if (latestGeneration > inFlight->second + kMaxStaleDecodeGenerationAge) {
                    // @Jin 连续 flyTo/手势后，旧视口瓦片即使已读盘也不再解码；磁盘缓存保留，下一次真正可见时会低代价重读。
                    stale = true;
                } else {
                    req.generation = inFlight->second;
                }
            }
        }
        if (haveBytes && !stale) {
            // worker 线程解码 RGBA（替代 GL 线程解码）：PNG/JPEG→RGBA8888 在此完成，
            // GL 线程 drainReady 后只做 glTexImage2D 上传，免一扎瓦片同时就绪时 GL 线程集中解码丢帧。
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (ImageDecoder::decodeRGBA(bytes.data(), bytes.size(), w, h, rgba)) {
                std::lock_guard<std::mutex> lk(readyMtx_);
                // 高水位背压：ready_ 逼近上限时丢弃本次交付（字节已落盘，下帧重请求读盘秒得），防无界堆积 OOM
                if (ready_.size() < kMaxReadyTiles) {
                    ReadyTile rt;
                    rt.z = req.z;
                    rt.x = req.x;
                    rt.y = req.y;
                    rt.width = w;
                    rt.height = h;
                    rt.generation = req.generation;
                    rt.rgba = std::move(rgba);
                    readyKeys_.insert(key(req.z, req.x, req.y));
                    ready_.push_back(std::move(rt));
                }
            } else {
                // 解码失败：磁盘缓存里的瓦片可能损坏（looksLikeImage 过了但 stb 解不开），删盘自愈；
                // 计为未取到字节，走失败退避，避免逐帧重刷坏瓦片
                LOGW("瓦片解码失败，清除缓存 z=%d x=%d y=%d size=%zu", req.z, req.x, req.y, bytes.size());
                cache_.removeTile(req.z, req.x, req.y);
                haveBytes = false;
                // @Jin: 只有成功解码并交付才算有效命中；坏缓存进入失败分支后会按冷却策略重试。
                fromCache = false;
                fromRaster = false;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            const uint64_t k = key(req.z, req.x, req.y);
            pending_.erase(k);
            inflight_.erase(k);
            if (stale) continue; // @Jin: 过期视口不计失败、不写退避，把 worker 立即让给当前视口。
            // 失败退避以「取字节是否成功」为准（非是否交付）：高水位丢弃的瓦片字节已在盘，
            // 不应误记冷却——否则会在 30s 内压制其重请求，远景兜底图迟迟补不上。
            if (haveBytes) {
                failedUntil_.erase(k); // 成功取到字节：清除旧失败标记（下帧可立即重请求读盘秒得）
                const uint64_t processed = statProcessed_.fetch_add(1) + 1;
                if (fromCache) statCacheHits_.fetch_add(1);
                if (fromNetwork) statNetworkHits_.fetch_add(1);
                if (fromRaster) statRasterGenerated_.fetch_add(1);
                // @Jin: 前 32 块逐块诊断之后，每处理 64 块输出一次聚合统计，方便真机 hilog 对比缓存/网络占比。
                if (!sampled && (processed % 64 == 0)) {
                    LOGI("TileLoader stats processed=%llu cache=%llu network=%llu raster=%llu failed=%llu cacheDir=%s",
                         static_cast<unsigned long long>(processed),
                         static_cast<unsigned long long>(statCacheHits_.load()),
                         static_cast<unsigned long long>(statNetworkHits_.load()),
                         static_cast<unsigned long long>(statRasterGenerated_.load()),
                         static_cast<unsigned long long>(statFailures_.load()),
                         cache_.baseDir().c_str());
                }
            } else if (!abort_.load()) {
                // 失败退避：记录冷却截止时刻，窗口内 request() 直接忽略（防限流风暴死循环）
                failedUntil_[k] = std::chrono::steady_clock::now() + kFailCooldown;
                const uint64_t processed = statProcessed_.fetch_add(1) + 1;
                statFailures_.fetch_add(1);
                // @Jin: 失败也计入聚合统计，限流或异常 XML 时能从 hilog 直接看到网络失败规模。
                if (!sampled && (processed % 64 == 0 || processed <= 8)) {
                    LOGI("TileLoader stats processed=%llu cache=%llu network=%llu raster=%llu failed=%llu cacheDir=%s",
                         static_cast<unsigned long long>(processed),
                         static_cast<unsigned long long>(statCacheHits_.load()),
                         static_cast<unsigned long long>(statNetworkHits_.load()),
                         static_cast<unsigned long long>(statRasterGenerated_.load()),
                         static_cast<unsigned long long>(statFailures_.load()),
                         cache_.baseDir().c_str());
                }
            }
        }
    }
}

std::string TileLoader::buildUrl(int z, int x, int y) {
    std::string tmpl;
    {
        std::lock_guard<std::mutex> lk(urlMtx_);
        tmpl = urlTemplate_;
    }
    // @Jin 普通墨卡托层与地理适配层共用 formatTileUrl，保证 {z}/{x}/{y} 与子域轮询口径一致。
    return formatTileUrl(tmpl, z, x, y, randCounter_);
}

void TileLoader::setGeographicSource(std::string rawCacheDir, std::string urlTemplate,
                                     int maxGeoLevel0Based, bool tmsY, int tileMatrixOffset) {
    std::lock_guard<std::mutex> lk(urlMtx_);
    geoScheme_ = !urlTemplate.empty();
    geoUrlTemplate_ = std::move(urlTemplate);
    geoMaxLevel0Based_ = std::max(0, maxGeoLevel0Based);
    geoTmsY_ = tmsY;
    geoTileMatrixOffset_ = tileMatrixOffset;
    // @Jin 原始地理瓦独立缓存：与结果（墨卡托瓦）缓存目录隔离，同一编号两套切片绝不串图。
    geoRawCache_.setBaseDir(std::move(rawCacheDir));
    // @Jin 地理适配层不再按墨卡托 (z,x,y) 直连；结果缓存 miss 时改由 generateGeographicTile 取图。
    urlTemplate_.clear();
    LOGI("TileLoader geographic source(EPSG4326/4490) on=%d maxGeoLevel0=%d tmsY=%d matrixOffset=%d rawDir=%s",
         geoScheme_ ? 1 : 0, geoMaxLevel0Based_, geoTmsY_ ? 1 : 0, geoTileMatrixOffset_,
         geoRawCache_.baseDir().c_str());
}

bool TileLoader::isGeographicSource() const {
    std::lock_guard<std::mutex> lk(urlMtx_);
    return geoScheme_;
}

bool TileLoader::generateGeographicTile(int z, int x, int y,
                                        std::vector<uint8_t> &outPng, bool &anySrcNetwork) {
    outPng.clear();
    anySrcNetwork = false;
    std::string geoTmpl;
    int maxS = 0, matrixOffset = 0;
    bool tmsY = false;
    {
        std::lock_guard<std::mutex> lk(urlMtx_);
        if (!geoScheme_) return false;
        geoTmpl = geoUrlTemplate_;
        maxS = geoMaxLevel0Based_;
        tmsY = geoTmsY_;
        matrixOffset = geoTileMatrixOffset_;
    }

    // @Jin 目标引擎墨卡托瓦地理四至（角纬度为墨卡托反投影；瓦内按线性经纬处理，与 3D 网格/本地栅格同口径）。
    double tLonMin, tLatMin, tLonMax, tLatMax;
    tileBounds(z, x, y, tLonMin, tLatMin, tLonMax, tLatMax);

    // @Jin 服务 0 基层级选 s=z（不是 z-1）：地理切片像元在地面上是“水平 cosφ、垂直 1”的扁矩形，
    // @Jin 若按 s=z-1 取图，水平虽 1:1，但垂直源地面分辨率只有需求的 cosφ 倍（34°≈0.83），
    // @Jin 输出时垂直被整体放大约 1/cosφ≈1.2，注记文字会上下拉高（与层级无关，肉眼可见）。
    // @Jin s=z 时每个输出像元从源的约 2(宽)×2cosφ(高) 区域重采样，恰好抵消源像元扁率，地面各向同性。
    const int s = std::max(0, std::min(maxS, z));
    const double cols = std::ldexp(1.0, s + 1);
    const double rows = std::ldexp(1.0, s);
    constexpr double kEps = 1e-9;
    int gx0 = static_cast<int>(std::floor((tLonMin + 180.0) / 360.0 * cols));
    int gx1 = static_cast<int>(std::floor((tLonMax + 180.0) / 360.0 * cols - kEps));
    int gy0 = static_cast<int>(std::floor((90.0 - tLatMax) / 180.0 * rows));
    int gy1 = static_cast<int>(std::floor((90.0 - tLatMin) / 180.0 * rows - kEps));
    gx0 = std::max(0, std::min(gx0, static_cast<int>(cols) - 1));
    gx1 = std::max(0, std::min(gx1, static_cast<int>(cols) - 1));
    gy0 = std::max(0, std::min(gy0, static_cast<int>(rows) - 1));
    gy1 = std::max(0, std::min(gy1, static_cast<int>(rows) - 1));
    const int spanX = gx1 - gx0 + 1;
    const int spanY = gy1 - gy0 + 1;
    if (spanX * spanY > kMaxGeoSourceTiles) {
        LOGW("地理源映射瓦数超限 z=%d x=%d y=%d count=%d，跳过本瓦（祖先兜底）", z, x, y, spanX * spanY);
        return false;
    }

    constexpr int kTile = 256; // @Jin 天地图 c 为 256px；拼贴画布按每块 256 线性排列。
    const int mosaicW = spanX * kTile;
    const int mosaicH = spanY * kTile;
    // @Jin 拼贴缓冲（预乘 alpha）：缺失处保持 0（透明），由祖先兜底，不污染邻区。
    std::vector<uint8_t> mosaic(static_cast<size_t>(mosaicW) * mosaicH * 4u, 0);
    const double originLon = -180.0 + 360.0 * static_cast<double>(gx0) / cols; // 拼贴西边界
    const double originLatN = 90.0 - 180.0 * static_cast<double>(gy0) / rows;   // 拼贴北边界
    const int matrixZ = s + matrixOffset; // @Jin 服务 TILEMATRIX（天地图 c 根 2×1 记 1）。

    for (int gy = gy0; gy <= gy1; ++gy) {
        for (int gx = gx0; gx <= gx1; ++gx) {
            const int urlY = tmsY ? (static_cast<int>(rows) - 1 - gy) : gy; // @Jin XYZ 不翻；TMS 翻转。
            std::vector<uint8_t> srcBytes;
            // ① 先读原始地理瓦独立缓存。
            bool have = geoRawCache_.readTile(matrixZ, gx, gy, srcBytes) && !srcBytes.empty() &&
                        looksLikeImage(srcBytes);
            // ② 未命中联网下载并原子写盘。
            if (!have) {
                if (abort_.load()) return false;
                const std::string url = formatTileUrl(geoTmpl, matrixZ, gx, urlY, randCounter_);
                std::vector<uint8_t> net;
                if (!url.empty() && HttpClient::get(url, net, 15000, &abort_) &&
                    !net.empty() && looksLikeImage(net)) {
                    geoRawCache_.writeTile(matrixZ, gx, gy, net);
                    srcBytes = std::move(net);
                    have = true;
                    anySrcNetwork = true;
                }
            }
            if (!have) {
                LOGW("地理源服务瓦获取失败 m=%d x=%d y=%d(urlY=%d)，本墨卡托瓦暂用祖先兜底",
                     matrixZ, gx, gy, urlY);
                return false;
            }
            int sw = 0, sh = 0;
            std::vector<uint8_t> rgba;
            if (!ImageDecoder::decodeRGBA(srcBytes.data(), srcBytes.size(), sw, sh, rgba) ||
                sw != kTile || sh != kTile) {
                LOGW("地理源服务瓦解码/尺寸异常 m=%d x=%d y=%d %dx%d", matrixZ, gx, gy, sw, sh);
                return false;
            }
            // @Jin 贴入拼贴缓冲并预乘 alpha，供双线性插值（注记 PNG 透明边不发黑）。
            const int ox = (gx - gx0) * kTile;
            const int oy = (gy - gy0) * kTile;
            for (int r = 0; r < kTile; ++r) {
                const uint8_t *srcLine = rgba.data() + static_cast<size_t>(r) * kTile * 4u;
                uint8_t *dstLine = mosaic.data() +
                    (static_cast<size_t>(oy + r) * mosaicW + ox) * 4u;
                for (int cc = 0; cc < kTile; ++cc) {
                    const uint8_t a = srcLine[cc * 4 + 3];
                    dstLine[cc * 4 + 0] = static_cast<uint8_t>(srcLine[cc * 4 + 0] * a / 255);
                    dstLine[cc * 4 + 1] = static_cast<uint8_t>(srcLine[cc * 4 + 1] * a / 255);
                    dstLine[cc * 4 + 2] = static_cast<uint8_t>(srcLine[cc * 4 + 2] * a / 255);
                    dstLine[cc * 4 + 3] = a;
                }
            }
        }
    }

    // @Jin 双线性重采样到目标墨卡托瓦 256²。经度方向墨卡托本身线性；纬度方向逐行按真实墨卡托
    // @Jin 反投影取该像元中心纬度（而不是在南北角纬度间线性插值），使输出 PNG 的纬向像素密度与
    // @Jin 引擎纹理 UV 严格一致——早期“瓦内线性纬度”近似在中纬度低层级会产生 ±1%~3% 的垂直尺度
    // @Jin 误差，注记文字肉眼可见上下轻微拉伸（3857 原生注记无此问题）；逐行反算后误差为 0。
    std::vector<uint8_t> dst(static_cast<size_t>(kTile) * kTile * 4u, 0);
    const double tileLonSpan = tLonMax - tLonMin;
    const double invTilesZ = std::ldexp(1.0, -z); // @Jin 1/2^z：目标瓦一行对应的归一化世界坐标跨度。
    const double pixelsPerLon = cols * kTile / 360.0; // 拼贴每经度像素数
    const double pixelsPerLat = rows * kTile / 180.0;
    auto samplePM = [&](double fx, double fy, uint8_t out4[4]) {
        if (fx < 0.0) fx = 0.0;
        if (fx > mosaicW - 1.0001) fx = mosaicW - 1.0001;
        if (fy < 0.0) fy = 0.0;
        if (fy > mosaicH - 1.0001) fy = mosaicH - 1.0001;
        const int x0i = static_cast<int>(fx), y0i = static_cast<int>(fy);
        const double tt = fx - x0i, ty = fy - y0i;
        const uint8_t *p00 = &mosaic[(static_cast<size_t>(y0i) * mosaicW + x0i) * 4u];
        const uint8_t *p10 = &mosaic[(static_cast<size_t>(y0i) * mosaicW + (x0i + 1)) * 4u];
        const uint8_t *p01 = &mosaic[(static_cast<size_t>(y0i + 1) * mosaicW + x0i) * 4u];
        const uint8_t *p11 = &mosaic[(static_cast<size_t>(y0i + 1) * mosaicW + (x0i + 1)) * 4u];
        for (int b = 0; b < 4; ++b) {
            const double top = p00[b] * (1.0 - tt) + p10[b] * tt;
            const double bot = p01[b] * (1.0 - tt) + p11[b] * tt;
            const double v = top * (1.0 - ty) + bot * ty;
            out4[b] = static_cast<uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v + 0.5));
        }
    };
    for (int py = 0; py < kTile; ++py) {
        // @Jin 第 py 行像元中心的归一化墨卡托 wy=(y+(py+0.5)/256)/2^z，反投影得精确纬度；
        // @Jin 与纹理 UV 的线性映射（2D 轴对齐瓦、3D 网格 UV）完全对齐，消除瓦内垂直拉伸。
        const double wy = (static_cast<double>(y) + (static_cast<double>(py) + 0.5) / kTile) * invTilesZ;
        double rowLonUnused = 0.0;
        double lat = 0.0;
        MercatorProjection::worldToLonLat(0.0, wy, rowLonUnused, lat);
        const double fy = (originLatN - lat) * pixelsPerLat - 0.5;
        for (int px = 0; px < kTile; ++px) {
            const double lon = tLonMin + (px + 0.5) / kTile * tileLonSpan;
            const double fx = (lon - originLon) * pixelsPerLon - 0.5;
            uint8_t pm[4];
            samplePM(fx, fy, pm);
            uint8_t *o = &dst[(static_cast<size_t>(py) * kTile + px) * 4u];
            const uint8_t a = pm[3];
            o[3] = a;
            if (a == 0) { o[0] = o[1] = o[2] = 0; } // @Jin 透明像素 RGB 置 0，避免预乘垃圾色。
            else { // @Jin 去预乘
                o[0] = static_cast<uint8_t>(std::min(255.0, pm[0] * 255.0 / a + 0.5));
                o[1] = static_cast<uint8_t>(std::min(255.0, pm[1] * 255.0 / a + 0.5));
                o[2] = static_cast<uint8_t>(std::min(255.0, pm[2] * 255.0 / a + 0.5));
            }
        }
    }
    // @Jin 编码 PNG 后完全复用「结果缓存→looksLikeImage→ImageDecoder→GL 上传」既有链路。
    return PngEncoder::encodeRGBA(kTile, kTile, dst.data(), outPng);
}

} // namespace globecore
