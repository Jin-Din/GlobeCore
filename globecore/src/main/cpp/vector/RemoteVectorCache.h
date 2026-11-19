#ifndef GLOBECORE_REMOTE_VECTOR_CACHE_H
#define GLOBECORE_REMOTE_VECTOR_CACHE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace globecore {

struct RemoteVectorFetchOptions {
    std::string cacheDir;
    std::vector<std::pair<std::string, std::string>> headers;
    long timeoutMs = 20000;
    size_t maxBytes = 80ULL * 1024ULL * 1024ULL;
};

struct RemoteVectorFetchResult {
    bool ok = false;
    bool cacheHit = false;
    std::string localPath;
    std::string error;
    size_t bytes = 0;
    int64_t elapsedMs = 0;
};

/**
 * @Jin P0 在线 GeoJSON 下载器：公开 URL → 校验 → 原子写私有缓存 → 复用 GDAL/OGR 本地读取管线。
 */
class RemoteVectorCache {
public:
    // @Jin 实验开关：如线上数据源出现兼容问题，改为 false 即可回退到旧的纯本地路径语义。
    static constexpr bool kExperimentalRemoteGeoJsonSource = true;
    // @Jin addGeoJSONLayer 专用接口实验开关：关闭后 url/data 新通路直接返回中文错误，回退到 addVectorLayer 旧行为。
    static constexpr bool kExperimentalGeoJsonLayerApi = true;

    static RemoteVectorFetchResult prepareGeoJson(
        const std::string &url,
        const std::string &cachePath,
        const RemoteVectorFetchOptions &options,
        const std::atomic<bool> &abortFlag);

    // @Jin GeoJSON 字符串（addGeoJSONLayer 的 data 参数）：校验后原子写 inline 缓存，同内容哈希天然去重。
    // 命中已存在且完整的缓存时不重复落盘；行为/错误文案口径与 prepareGeoJson 对齐。
    static RemoteVectorFetchResult prepareInlineGeoJson(
        const std::string &geoJsonText,
        const std::string &cachePath,
        const RemoteVectorFetchOptions &options,
        const std::atomic<bool> &abortFlag);

private:
    static bool validateGeoJsonBytes(const std::vector<uint8_t> &bytes, std::string &error);
    static bool validateCachedFile(const std::string &path, size_t &bytes, std::string &error);
    // @Jin 下载与 inline 字符串共用的原子落盘：tmp 写入 + rename，失败清理半成品。
    static bool atomicWriteCacheFile(const std::vector<uint8_t> &bytes,
                                     const std::string &cachePath, std::string &error);
};

} // @Jin namespace globecore

#endif
