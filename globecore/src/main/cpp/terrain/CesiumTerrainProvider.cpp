#include "terrain/CesiumTerrainProvider.h"

#include "net/HttpClient.h"
#include "util/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace globecore {

namespace {

// @Jin 已交付服务 DEM 的 LRU 上限略低于 Renderer 的 240 块，避免 Provider 误以为已被淘汰的块仍可复用。
constexpr size_t kDeliveredTileCapacity = 220;
// @Jin 单个 DEM 请求超时；与图片瓦片口径一致，避免弱网任务长期占用 worker。
constexpr long kTileTimeoutMs = 20000;
// @Jin 元数据通常约数百 KB，给更宽松的超时以适配移动网络。
constexpr long kMetadataTimeoutMs = 30000;
constexpr int kNetworkRetryCount = 1;

void mkdirRecursive(const std::string &dir) {
    // @Jin 先逐级创建父目录，再创建末级目录；已存在时忽略 EEXIST。
    if (dir.empty()) return;
    for (size_t pos = dir.find('/', 1); pos != std::string::npos; pos = dir.find('/', pos + 1)) {
        mkdir(dir.substr(0, pos).c_str(), 0777);
    }
    mkdir(dir.c_str(), 0777);
}

bool readFileBytes(const std::string &path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t read = fread(out.data(), 1, out.size(), file);
    fclose(file);
    if (read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

bool readTextFile(const std::string &path, std::string &out) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) return false;
    out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return !out.empty();
}

bool writeFileAtomic(const std::string &path, const uint8_t *data, size_t size) {
    // @Jin 先写临时文件再 rename，进程中断时不会留下半截 layer.json/DEM 污染后续启动。
    if (data == nullptr || size == 0) return false;
    const std::string tmp = path + ".tmp";
    FILE *file = fopen(tmp.c_str(), "wb");
    if (file == nullptr) return false;
    const size_t written = fwrite(data, 1, size, file);
    fflush(file);
    fclose(file);
    if (written != size) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::string &path, const std::vector<uint8_t> &bytes) {
    return writeFileAtomic(path, bytes.data(), bytes.size());
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

void replaceAll(std::string &text, const std::string &from, const std::string &to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string tileKey(const TerrainServiceTile &tile) {
    return std::to_string(tile.z) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
}

} // namespace

CesiumTerrainProvider::CesiumTerrainProvider(TerrainProviderOptions options,
                                             ViewportProvider viewportProvider,
                                             TileSink tileSink)
    : options_(std::move(options)),
      viewportProvider_(std::move(viewportProvider)),
      tileSink_(std::move(tileSink)) {
    options_.baseUrl = trimTrailingSlash(options_.baseUrl);
    options_.concurrency = std::max(1, std::min(16, options_.concurrency));
    options_.tileRadius = std::max(0, std::min(8, options_.tileRadius));
    options_.maxZoom = std::max(0, std::min(22, options_.maxZoom));
    effectiveConcurrency_ = options_.concurrency;
    dispatcherThread_ = std::thread(&CesiumTerrainProvider::dispatcherLoop, this);
}

CesiumTerrainProvider::~CesiumTerrainProvider() {
    stopping_.store(true);
    abortInFlight();
    waitCv_.notify_all();
    if (dispatcherThread_.joinable()) {
        dispatcherThread_.join();
    }
}

void CesiumTerrainProvider::setEnabled(bool enabled) {
    enabled_.store(enabled);
    if (enabled) {
        requestRefresh();
        return;
    }
    // @Jin 关闭后立即取消在途网络和排队任务；迟到的 worker 结果会被 generation 检查丢弃。
    ++generation_;
    refreshPending_.store(false);
    metadataPrefetchPending_.store(false);
    abortInFlight();
    resetDeliveredTiles();
    waitCv_.notify_all();
    LOGI("CesiumTerrainProvider: disabled");
}

void CesiumTerrainProvider::requestRefresh() {
    if (!enabled_.load() || stopping_.load()) return;
    // @Jin 新视口优先于旧视口：中断旧请求，调度线程只处理最新一次刷新。
    ++generation_;
    abortInFlight();
    refreshPending_.store(true);
    waitCv_.notify_all();
}

void CesiumTerrainProvider::prefetchMetadata() {
    if (stopping_.load()) return;
    metadataPrefetchPending_.store(true);
    waitCv_.notify_all();
}

void CesiumTerrainProvider::resetDeliveredTiles() {
    std::lock_guard<std::mutex> lock(deliveredMtx_);
    delivered_.clear();
    deliveredOrder_.clear();
}

void CesiumTerrainProvider::dispatcherLoop() {
    while (!stopping_.load()) {
        bool doPrefetch = false;
        bool doRefresh = false;
        {
            std::unique_lock<std::mutex> lock(waitMtx_);
            waitCv_.wait(lock, [this]() {
                return stopping_.load() || refreshPending_.load() || metadataPrefetchPending_.load();
            });
            if (stopping_.load()) break;
            doPrefetch = metadataPrefetchPending_.exchange(false);
            doRefresh = refreshPending_.exchange(false);
        }

        if (doPrefetch) {
            auto abort = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lock(abortMtx_);
                currentAbort_ = abort;
            }
            loadMetadata(abort);
            {
                std::lock_guard<std::mutex> lock(abortMtx_);
                if (currentAbort_ == abort) currentAbort_.reset();
            }
        }
        if (doRefresh && enabled_.load() && !stopping_.load()) {
            TerrainViewportSnapshot viewport = viewportProvider_ ? viewportProvider_() : TerrainViewportSnapshot{};
            if (viewport.valid && viewport.viewMode == 1) {
                runRefresh(viewport, generation_.load());
            }
        }
    }
}

bool CesiumTerrainProvider::loadMetadata(const std::shared_ptr<std::atomic<bool>> &abort) {
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        if (metadataLoaded_ && meta_.ok) return true;
    }

    const std::string cachePath = metadataCachePath();
    std::string text;
    if (readTextFile(cachePath, text)) {
        CesiumTerrainMeta cached = parseCesiumTerrainLayerJson(text, options_.maxZoom);
        if (cached.ok) {
            statMetadataDiskHits_++;
            {
                std::lock_guard<std::mutex> lock(metaMtx_);
                meta_ = std::move(cached);
                metadataLoaded_ = true;
            }
            LOGI("CesiumTerrainProvider: layer.json disk hit bytes=%zu maxzoom=%d", text.size(), meta_.maxZoom);
            return true;
        }
    }

    std::vector<uint8_t> bytes;
    const std::string url = options_.baseUrl + "/layer.json";
    statMetadataNetworkRequests_++;
    const bool ok = HttpClient::get(url, bytes, kMetadataTimeoutMs, abort.get(), options_.headers);
    if (!ok || bytes.empty() || abort->load()) {
        LOGE("CesiumTerrainProvider: layer.json download failed url=%s", url.c_str());
        return false;
    }

    text.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    CesiumTerrainMeta downloaded = parseCesiumTerrainLayerJson(text, options_.maxZoom);
    if (!downloaded.ok) {
        LOGE("CesiumTerrainProvider: layer.json parse failed: %s", downloaded.error.c_str());
        return false;
    }
    // @Jin 在 move 前保存日志字段，避免 std::string 被移走后输出空模板。
    const int downloadedMaxZoom = downloaded.maxZoom;
    const std::string downloadedTemplate = downloaded.tileTemplate;

    mkdirRecursive(options_.cacheDir);
    if (writeFileAtomic(cachePath, bytes)) {
        LOGI("CesiumTerrainProvider: layer.json cached bytes=%zu path=%s", bytes.size(), cachePath.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        meta_ = std::move(downloaded);
        metadataLoaded_ = true;
    }
    LOGI("CesiumTerrainProvider: layer.json network ok bytes=%zu maxzoom=%d template=%s",
         bytes.size(), downloadedMaxZoom, downloadedTemplate.c_str());
    return true;
}

std::vector<CesiumTerrainProvider::Task> CesiumTerrainProvider::buildTasks(
    const TerrainViewportSnapshot &viewport, int &engineZoom) {
    CesiumTerrainMeta meta;
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        meta = meta_;
    }
    if (!meta.ok) return {};

    engineZoom = static_cast<int>(std::llround(viewport.displayZoom));
    engineZoom = std::max(0, std::min(meta.maxZoom, engineZoom));
    lastEngineZoom_.store(engineZoom);

    int centerX = 0;
    int centerY = 0;
    lonLatToWebMercatorTile(engineZoom, viewport.longitude, viewport.latitude, centerX, centerY);
    const int tileCount = static_cast<int>(std::ldexp(1.0, engineZoom));

    std::vector<Task> tasks;
    std::unordered_map<std::string, bool> wanted;
    for (int radius = 0; radius <= options_.tileRadius; ++radius) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int engineX = centerX + dx;
                const int engineY = centerY + dy;
                if (engineX < 0 || engineX >= tileCount || engineY < 0 || engineY >= tileCount) continue;
                statRequestedEngineTiles_++;

                const std::vector<TerrainServiceTile> serviceTiles =
                    cesiumServiceTilesForEngineTile(meta, engineZoom, engineX, engineY);
                if (serviceTiles.empty()) statAvailableSkips_++;
                for (const TerrainServiceTile &service : serviceTiles) {
                    const std::string key = tileKey(service);
                    if (wanted.find(key) != wanted.end() || isDelivered(key)) {
                        statDuplicateSkips_++;
                        continue;
                    }
                    wanted[key] = true;
                    Task task;
                    task.engineZ = engineZoom;
                    task.engineX = engineX;
                    task.engineY = engineY;
                    task.service = service;
                    task.bounds = cesiumGeoTileBounds(service.z, service.x, service.y);
                    tasks.push_back(task);
                }
            }
        }
    }
    statRequestedServiceTiles_ += static_cast<int>(tasks.size());
    return tasks;
}

bool CesiumTerrainProvider::fetchTile(const TerrainServiceTile &tile,
                                      const std::shared_ptr<std::atomic<bool>> &abort,
                                      std::vector<uint8_t> &bytes) {
    const std::string path = cachePath(tile);
    if (readFileBytes(path, bytes)) {
        statDiskHits_++;
        return true;
    }
    if (abort->load()) return false;

    const std::string url = tileUrl(tile);
    for (int attempt = 0; attempt <= kNetworkRetryCount; ++attempt) {
        if (abort->load()) return false;
        statNetworkRequests_++;
        if (fetchNetwork(url, abort, bytes)) {
            mkdirRecursive(path.substr(0, path.find_last_of('/')));
            if (writeFileAtomic(path, bytes)) statCacheWrites_++;
            return true;
        }
        if (attempt < kNetworkRetryCount && !abort->load()) {
            statRetries_++;
            // @Jin 网络抖动重试前短暂退避，避免同一服务瞬时形成重试风暴。
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    }
    return false;
}

bool CesiumTerrainProvider::fetchNetwork(const std::string &url,
                                         const std::shared_ptr<std::atomic<bool>> &abort,
                                         std::vector<uint8_t> &bytes) {
    return HttpClient::get(url, bytes, kTileTimeoutMs, abort.get(), options_.headers) && !bytes.empty();
}

void CesiumTerrainProvider::runRefresh(const TerrainViewportSnapshot &viewport, uint64_t generation) {
    const auto start = std::chrono::steady_clock::now();
    // @Jin 新视口开始时先清掉上一轮耗时，轮询期间不会把旧首块时间误报成本轮结果。
    lastFirstTileMs_.store(-1);
    lastElapsedMs_.store(0);
    auto abort = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(abortMtx_);
        currentAbort_ = abort;
    }

    bool metaOk = false;
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        metaOk = metadataLoaded_ && meta_.ok;
    }
    if (!metaOk && !loadMetadata(abort)) {
        std::lock_guard<std::mutex> lock(abortMtx_);
        if (currentAbort_ == abort) currentAbort_.reset();
        lastElapsedMs_.store(0);
        LOGE("CesiumTerrainProvider: refresh skipped because layer.json is unavailable");
        return;
    }

    int engineZoom = 0;
    const int engineTilesBefore = statRequestedEngineTiles_.load();
    std::vector<Task> tasks = buildTasks(viewport, engineZoom);
    const int runEngineTiles = statRequestedEngineTiles_.load() - engineTilesBefore;
    const int queued = static_cast<int>(tasks.size());
    LOGI("CesiumTerrainProvider: refresh start z=%d center=%.4f,%.4f tilt=%.1f engineTiles=%d serviceTasks=%d concurrency=%d",
         engineZoom, viewport.latitude, viewport.longitude, viewport.tilt,
         runEngineTiles, queued, effectiveConcurrency_);

    if (tasks.empty()) {
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        lastElapsedMs_.store(elapsed);
        {
            std::lock_guard<std::mutex> lock(abortMtx_);
            if (currentAbort_ == abort) currentAbort_.reset();
        }
        LOGI("CesiumTerrainProvider: refresh complete z=%d no new DEM elapsed=%lldms", engineZoom, elapsed);
        return;
    }

    std::atomic<size_t> cursor{0};
    std::atomic<int> runDelivered{0};
    std::atomic<int> runFailures{0};
    std::atomic<long long> firstTileMs{-1};
    const int workers = std::max(1, std::min(effectiveConcurrency_, queued));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([this, &tasks, &cursor, &runDelivered, &runFailures, &firstTileMs, abort, generation, start]() {
            while (!stopping_.load() && !abort->load() && generation_.load() == generation) {
                const size_t index = cursor.fetch_add(1);
                if (index >= tasks.size()) break;
                const Task &task = tasks[index];
                std::vector<uint8_t> bytes;
                if (!fetchTile(task.service, abort, bytes)) {
                    if (!abort->load()) {
                        runFailures++;
                        statFailures_++;
                    }
                    continue;
                }
                if (stopping_.load() || abort->load() || generation_.load() != generation) continue;
                bool sinkOk = true;
                if (tileSink_) {
                    try {
                        tileSink_(task.engineZ, task.engineX, task.engineY, bytes, task.bounds);
                    } catch (...) {
                        // @Jin 平台注入回调异常不得终止 C++ worker；失败不写交付 LRU，后续刷新可重试。
                        sinkOk = false;
                        runFailures++;
                        statFailures_++;
                    }
                }
                if (!sinkOk) continue;
                markDelivered(tileKey(task.service));
                const int deliveredNow = runDelivered.fetch_add(1) + 1;
                statDeliveredTiles_++;
                if (deliveredNow == 1) {
                    firstTileMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count());
                }
            }
        });
    }
    for (std::thread &thread : threads) thread.join();

    const int delivered = runDelivered.load();
    const int failures = runFailures.load();

    // @Jin 根据本轮成功率轻量自适应并发：连续失败降档，稳定成功逐步恢复到宿主配置。
    if (failures > 0 && failures >= delivered) {
        effectiveConcurrency_ = std::max(1, effectiveConcurrency_ / 2);
    } else if (delivered > 0 && failures * 8 < delivered && effectiveConcurrency_ < options_.concurrency) {
        effectiveConcurrency_++;
    }

    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    lastElapsedMs_.store(elapsed);
    lastFirstTileMs_.store(firstTileMs.load());
    {
        std::lock_guard<std::mutex> lock(abortMtx_);
        if (currentAbort_ == abort) currentAbort_.reset();
    }
    LOGI("CesiumTerrainProvider: refresh complete z=%d tasks=%d delivered=%d firstTileMs=%lld diskHits=%d network=%d retries=%d failures=%d elapsed=%lldms",
         engineZoom, queued, delivered, firstTileMs.load(), statDiskHits_.load(), statNetworkRequests_.load(),
         statRetries_.load(), failures, elapsed);
}

std::string CesiumTerrainProvider::tileUrl(const TerrainServiceTile &tile) const {
    std::string path = meta_.tileTemplate;
    replaceAll(path, "{z}", std::to_string(tile.z));
    replaceAll(path, "{x}", std::to_string(tile.x));
    replaceAll(path, "{y}", std::to_string(tile.y));
    replaceAll(path, "{reverseY}", std::to_string((1 << tile.z) - 1 - tile.y));
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) return path;
    return options_.baseUrl + "/" + path;
}

std::string CesiumTerrainProvider::cachePath(const TerrainServiceTile &tile) const {
    return options_.cacheDir + "/" + std::to_string(tile.z) + "/" +
           std::to_string(tile.x) + "_" + std::to_string(tile.y) + ".terrain";
}

std::string CesiumTerrainProvider::metadataCachePath() const {
    return options_.cacheDir + "/layer.json";
}

bool CesiumTerrainProvider::isDelivered(const std::string &key) {
    std::lock_guard<std::mutex> lock(deliveredMtx_);
    return delivered_.find(key) != delivered_.end();
}

void CesiumTerrainProvider::markDelivered(const std::string &key) {
    // @Jin 关闭/换源后的迟到 worker 不再写回交付 LRU，避免刚清空又被单块结果污染。
    if (!enabled_.load() || stopping_.load()) return;
    std::lock_guard<std::mutex> lock(deliveredMtx_);
    auto existing = delivered_.find(key);
    if (existing != delivered_.end()) {
        deliveredOrder_.erase(existing->second);
    }
    deliveredOrder_.push_back(key);
    delivered_[key] = std::prev(deliveredOrder_.end());
    while (deliveredOrder_.size() > kDeliveredTileCapacity) {
        const std::string oldest = deliveredOrder_.front();
        deliveredOrder_.pop_front();
        delivered_.erase(oldest);
    }
}

void CesiumTerrainProvider::abortInFlight() {
    std::lock_guard<std::mutex> lock(abortMtx_);
    if (currentAbort_) currentAbort_->store(true);
}

TerrainProviderStats CesiumTerrainProvider::stats() const {
    TerrainProviderStats out;
    out.enabled = enabled_.load();
    out.engineZoom = lastEngineZoom_.load();
    out.requestedEngineTiles = statRequestedEngineTiles_.load();
    out.requestedServiceTiles = statRequestedServiceTiles_.load();
    out.deliveredTiles = statDeliveredTiles_.load();
    out.diskHits = statDiskHits_.load();
    out.metadataDiskHits = statMetadataDiskHits_.load();
    out.metadataNetworkRequests = statMetadataNetworkRequests_.load();
    out.networkRequests = statNetworkRequests_.load();
    out.retries = statRetries_.load();
    out.cacheWrites = statCacheWrites_.load();
    out.availableSkips = statAvailableSkips_.load();
    out.duplicateSkips = statDuplicateSkips_.load();
    out.failures = statFailures_.load();
    out.firstTileMs = lastFirstTileMs_.load();
    out.elapsedMs = lastElapsedMs_.load();
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        out.metadataLoaded = metadataLoaded_ && meta_.ok;
        out.maxZoom = meta_.ok ? meta_.maxZoom : options_.maxZoom;
    }
    return out;
}

std::string CesiumTerrainProvider::statsJson() const {
    const TerrainProviderStats s = stats();
    std::ostringstream json;
    json << "{\"enabled\":" << (s.enabled ? 1 : 0)
         << ",\"metadataLoaded\":" << (s.metadataLoaded ? 1 : 0)
         << ",\"maxZoom\":" << s.maxZoom
         << ",\"engineZoom\":" << s.engineZoom
         << ",\"requestedEngineTiles\":" << s.requestedEngineTiles
         << ",\"requestedServiceTiles\":" << s.requestedServiceTiles
         << ",\"deliveredTiles\":" << s.deliveredTiles
         << ",\"diskHits\":" << s.diskHits
         << ",\"metadataDiskHits\":" << s.metadataDiskHits
         << ",\"metadataNetworkRequests\":" << s.metadataNetworkRequests
         << ",\"networkRequests\":" << s.networkRequests
         << ",\"retries\":" << s.retries
         << ",\"cacheWrites\":" << s.cacheWrites
         << ",\"availableSkips\":" << s.availableSkips
         << ",\"duplicateSkips\":" << s.duplicateSkips
         << ",\"failures\":" << s.failures
         << ",\"firstTileMs\":" << s.firstTileMs
         << ",\"elapsedMs\":" << s.elapsedMs
         << "}";
    return json.str();
}

} // namespace globecore
