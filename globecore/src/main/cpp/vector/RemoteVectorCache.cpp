#include "vector/RemoteVectorCache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include "net/HttpClient.h"
#include "util/Log.h"
#include "vector/VectorSourceResolver.h"

namespace globecore {
namespace {

// @Jin 读取首尾探测时统一跳过 UTF-8 BOM 与 JSON 外围空白。
uint8_t firstNonSpace(const std::vector<uint8_t> &data, size_t start = 0) {
    size_t pos = start;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) pos = 3;
    while (pos < data.size()) {
        const uint8_t ch = data[pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return ch;
        ++pos;
    }
    return 0;
}

// @Jin 用最后一个非空白字符快速识别被截断的根对象/数组。
uint8_t lastNonSpace(const std::vector<uint8_t> &data) {
    for (auto it = data.rbegin(); it != data.rend(); ++it) {
        const uint8_t ch = *it;
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return ch;
    }
    return 0;
}

} // @Jin anonymous namespace

bool RemoteVectorCache::validateCachedFile(const std::string &path, size_t &bytes, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "在线 GeoJSON 缓存读取失败";
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff total = in.tellg();
    if (total <= 0) {
        error = "在线 GeoJSON 缓存为空";
        return false;
    }
    bytes = static_cast<size_t>(total);

    // @Jin 缓存文件由本类完整嗅探后原子落盘；缓存命中只检查首尾，避免 extent 重载时反复整文件读取。
    constexpr std::streamoff kProbeBytes = 256;
    std::vector<uint8_t> head(static_cast<size_t>(std::min<std::streamoff>(total, kProbeBytes)));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
    const uint8_t first = firstNonSpace(head);
    if (first != '{' && first != '[') {
        error = "在线 GeoJSON 缓存格式无效";
        return false;
    }

    std::vector<uint8_t> tail(static_cast<size_t>(std::min<std::streamoff>(total, kProbeBytes)));
    in.seekg(total - static_cast<std::streamoff>(tail.size()), std::ios::beg);
    in.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tail.size()));
    const uint8_t last = lastNonSpace(tail);
    if (first == '{' && last != '}' ) {
        error = "在线 GeoJSON 缓存可能已损坏";
        return false;
    }
    if (first == '[' && last != ']') {
        error = "在线 GeoJSON 缓存可能已损坏";
        return false;
    }
    return true;
}

bool RemoteVectorCache::validateGeoJsonBytes(const std::vector<uint8_t> &bytes, std::string &error) {
    if (bytes.empty()) {
        error = "在线 GeoJSON 内容为空";
        return false;
    }

    // @Jin 跳过 UTF-8 BOM，内网常见文本服务可能直接返回带 BOM 的 JSON。
    const unsigned char first = firstNonSpace(bytes);
    if (first == 0) {
        error = "在线 GeoJSON 内容为空";
        return false;
    }
    if (first == '<') {
        error = "在线地址返回的不是 GeoJSON，可能是登录页或错误页";
        return false;
    }
    if (first != '{' && first != '[') {
        error = "在线数据不是有效 GeoJSON";
        return false;
    }

    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const size_t typePos = text.find("\"type\"");
    if (typePos == std::string::npos) {
        error = "在线数据不是有效 GeoJSON：缺少 type 字段";
        return false;
    }
    // @Jin P0 只做轻嗅探：接受 FeatureCollection/Feature 和常见 Geometry 根对象，最终仍由 OGR 严格解析。
    if (text.find("FeatureCollection") == std::string::npos &&
        text.find("\"Feature\"") == std::string::npos &&
        text.find("Point") == std::string::npos &&
        text.find("LineString") == std::string::npos &&
        text.find("Polygon") == std::string::npos &&
        text.find("GeometryCollection") == std::string::npos) {
        error = "在线数据不是有效 GeoJSON";
        return false;
    }
    return true;
}

RemoteVectorFetchResult RemoteVectorCache::prepareGeoJson(
    const std::string &url,
    const std::string &cachePath,
    const RemoteVectorFetchOptions &options,
    const std::atomic<bool> &abortFlag) {
    const auto started = std::chrono::steady_clock::now();
    RemoteVectorFetchResult result;
    result.localPath = cachePath;
    const std::string safeUrl = VectorSourceResolver::redactUrlForLog(url);

    if (!kExperimentalRemoteGeoJsonSource) {
        result.error = "在线 GeoJSON 实验能力已关闭";
        return result;
    }
    if (!VectorSourceResolver::isHttpUrl(url)) {
        result.error = "在线矢量地址协议不支持";
        return result;
    }
    const std::string hostMarker = "://";
    const size_t schemeEnd = url.find(hostMarker);
    const size_t hostStart = schemeEnd + hostMarker.size();
    const size_t pathStart = url.find('/', hostStart);
    const std::string host = pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    if (host.empty()) {
        result.error = "在线矢量地址无效";
        return result;
    }
    if (cachePath.empty() || options.cacheDir.empty()) {
        result.error = "在线 GeoJSON 缓存目录未配置";
        return result;
    }

    // @Jin 同一 URL 的 extent 重载只允许命中缓存，不再访问网络；缓存损坏时才重新下载修复。
    size_t cachedBytes = 0;
    std::string cacheError;
    if (std::filesystem::exists(cachePath) && validateCachedFile(cachePath, cachedBytes, cacheError)) {
        result.ok = true;
        result.cacheHit = true;
        result.bytes = cachedBytes;
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        LOGI("[RemoteGeoJson] cache hit source=%s bytes=%zu elapsed=%lldms", safeUrl.c_str(), result.bytes, static_cast<long long>(result.elapsedMs));
        return result;
    }
    if (abortFlag.load()) return result;

    std::vector<uint8_t> bytes;
    long httpCode = 0;
    bool sizeLimitExceeded = false;
    // @Jin maxBytes 下传到 curl 写回调，避免错误服务无限写入内存。
    const bool fetched = HttpClient::get(url, bytes, options.timeoutMs, &abortFlag, options.headers,
                                         options.maxBytes, &httpCode, &sizeLimitExceeded);
    if (abortFlag.load()) {
        result.error = "在线 GeoJSON 加载已取消";
        return result;
    }
    if (!fetched) {
        if (sizeLimitExceeded) result.error = "在线 GeoJSON 超过大小限制";
        else if (httpCode > 0) result.error = "在线 GeoJSON 请求失败：HTTP " + std::to_string(httpCode);
        else result.error = "在线 GeoJSON 下载失败";
        return result;
    }
    result.bytes = bytes.size();
    if (result.bytes > options.maxBytes) {
        result.error = "在线 GeoJSON 超过大小限制";
        return result;
    }
    if (!validateGeoJsonBytes(bytes, result.error)) return result;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);
    if (ec) {
        result.error = "在线 GeoJSON 缓存目录创建失败";
        return result;
    }

    const std::string tmpPath = cachePath + ".tmp." + std::to_string(static_cast<long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id())));
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.error = "在线 GeoJSON 缓存写入失败";
            return result;
        }
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out.good()) {
            result.error = "在线 GeoJSON 缓存写入失败";
            std::filesystem::remove(tmpPath, ec);
            return result;
        }
    }
    std::filesystem::rename(tmpPath, cachePath, ec);
    if (ec) {
        // @Jin Android/Linux 上 rename 通常可原子覆盖；失败再尝试 remove+rename，失败则拒绝读取半成品。
        std::filesystem::remove(cachePath, ec);
        std::filesystem::rename(tmpPath, cachePath, ec);
        if (ec) {
            result.error = "在线 GeoJSON 缓存写入失败";
            return result;
        }
    }

    result.ok = true;
    result.localPath = cachePath;
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    LOGI("[RemoteGeoJson] downloaded source=%s bytes=%zu elapsed=%lldms cache=%s",
         safeUrl.c_str(), result.bytes, static_cast<long long>(result.elapsedMs), cachePath.c_str());
    return result;
}

} // @Jin namespace globecore
