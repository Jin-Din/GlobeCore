#include "layer/TileLoader.h"

#include "layer/TileCache.h"
#include "globe/MercatorProjection.h"
#include "net/HttpClient.h"
#include "raster/RasterRenderer.h"
#include "render/ImageDecoder.h"
#include "util/Log.h"

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
    // @Jin: 相同视口的补帧/续帧不清队列，保证已下载瓦片能按预算渐进上传；视口签名变化才切换代际。
    bool sameView = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sameView = hasViewSignature_ && viewSignature_ == viewSignature;
    }
    if (sameView) return;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (hasViewSignature_ && viewSignature_ == viewSignature) return;
        hasViewSignature_ = true;
        viewSignature_ = viewSignature;
        ++viewGeneration_;
        // @Jin: 只清理尚未开工的排队项；在途项由 inflight_ 代际判断，能复用的升级、不能复用的不交付。
        queue_.clear();
        pending_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        ready_.clear(); // @Jin: 旧视口已解码结果不再抢占本帧 GL 纹理上传预算。
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
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        if (pending_.count(k) != 0) return; // @Jin: 已排队的同 key 不重复入队。
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
        queue_.push_back(Request{z, x, y, viewGeneration_});
        // 超限丢最旧：被丢者同步移出 pending_，若仍可见下一帧会重新入队（去重不受损）
        while (queue_.size() > kMaxQueue) {
            const Request &oldest = queue_.front();
            pending_.erase(key(oldest.z, oldest.x, oldest.y));
            queue_.pop_front();
        }
    }
    cv_.notify_one();
}

void TileLoader::drainReady(std::vector<ReadyTile> &out, int maxCount) {
    std::lock_guard<std::mutex> lk(readyMtx_);
    int taken = 0;
    uint64_t currentGeneration = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        currentGeneration = viewGeneration_;
    }
    // @Jin: 从队首逐项检查，过期结果直接丢弃；同视口结果仍遵循单帧上传预算。
    while (!ready_.empty() && (maxCount < 0 || taken < maxCount)) {
        if (ready_.front().generation != currentGeneration) {
            ready_.pop_front();
            continue;
        }
        out.push_back(std::move(ready_.front()));
        ready_.pop_front();
        ++taken;
    }
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
        inflight_.clear();
        ++viewGeneration_;
        hasViewSignature_ = false;
        failedUntil_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        ready_.clear();
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
            // @Jin: 出队策略按数据源区分：在线标准 XYZ 首屏先取 z-1/z-2 祖先粗瓦，用少量瓦片快速铺满屏幕；
            // @Jin: 粗瓦请求完成后会从队列消失，worker 自然转入可见级叶子，形成“先完整低清、再局部变清”。
            // @Jin: 本地 GDAL 栅格仍保持最细叶子优先，因为无金字塔粗瓦可能触发整幅重投影，单块曾实测数秒以上。
            // @Jin: 同级都取下标更大的新请求，保留连续手势期间当前视口优先的跟手性。
            size_t best = 0;
            for (size_t i = 1; i < queue_.size(); ++i) {
                if (preferCoarseOnlineTile) {
                    if (queue_[i].z <= queue_[best].z) best = i; // @Jin: 在线图源低 z 优先，同级取更新。
                } else {
                    if (queue_[i].z >= queue_[best].z) best = i; // @Jin: 本地栅格高 z 优先，同级取更新。
                }
            }
            req = queue_[best];
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
            const uint64_t k = key(req.z, req.x, req.y);
            pending_.erase(k);
            inflight_[k] = req.generation;
        }

        // 诊断采样：本 loader 前若干块处理瓦片逐块打印来源（排查「栅格层零日志」归属问题），超预算自动静默
        std::string rasterPath;
        std::string urlTemplateSnapshot;
        {
            std::lock_guard<std::mutex> lk(urlMtx_);
            rasterPath = rasterPath_;
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
        // 3) 仍未命中 → 联网拉取（可被 abort_ 中断），成功则原子写盘。
        //    响应体须过图像魔数校验才落盘/交付：图源限流时以 HTTP 200 回异常 XML，不可当瓦片污染缓存。
        if (!haveBytes && rasterPath.empty()) {
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
    if (tmpl.empty()) return std::string();

    std::string url = std::move(tmpl);

    // {rand=a,b,c,...} → 轮询取一个子域（均衡负载，避免随机碰撞）
    const size_t rp = url.find("{rand=");
    if (rp != std::string::npos) {
        const size_t rpEnd = url.find('}', rp);
        if (rpEnd != std::string::npos) {
            const std::string list = url.substr(rp + 6, rpEnd - (rp + 6));
            std::vector<std::string> opts;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                if (comma == std::string::npos) {
                    opts.push_back(list.substr(start));
                    break;
                }
                opts.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
            std::string chosen;
            if (!opts.empty()) {
                const uint32_t idx = randCounter_.fetch_add(1) %
                                     static_cast<uint32_t>(opts.size());
                chosen = opts[idx];
            }
            url.replace(rp, rpEnd - rp + 1, chosen);
        }
    }

    // {z} / {x} / {y} → 瓦片坐标
    auto replaceAll = [](std::string &s, const char *from, const std::string &to) {
        const size_t flen = std::string(from).size();
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, flen, to);
            pos += to.size();
        }
    };
    replaceAll(url, "{z}", std::to_string(z));
    replaceAll(url, "{x}", std::to_string(x));
    replaceAll(url, "{y}", std::to_string(y));
    return url;
}

} // namespace globecore
