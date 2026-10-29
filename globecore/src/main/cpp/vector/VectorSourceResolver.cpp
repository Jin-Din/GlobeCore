#include "vector/VectorSourceResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace globecore {

namespace {
// @Jin FNV-1a 64 位哈希足以生成单机缓存文件名；不输出完整 URL，避免 query token 进入路径或日志。
uint64_t fnv1a64(const std::string &text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string toHex(uint64_t value) {
    static const char *kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[value & 0x0FULL];
        value >>= 4;
    }
    return out;
}
} // @Jin anonymous namespace

bool VectorSourceResolver::isHttpUrl(const std::string &source) {
    return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

std::string VectorSourceResolver::stableCacheKey(const std::string &url) {
    // @Jin 文件名使用 URL 的稳定哈希，后续同 URL 的 extent 重载只读这一份缓存。
    return "geojson-" + toHex(fnv1a64(url)) + ".geojson";
}

std::string VectorSourceResolver::redactUrlForLog(const std::string &url) {
    const size_t queryPos = url.find('?');
    if (queryPos == std::string::npos) return url;
    std::string base = url.substr(0, queryPos);
    const size_t fragmentPos = base.find('#');
    if (fragmentPos != std::string::npos) base.resize(fragmentPos);
    return base + "?***";
}

ResolvedVectorSource VectorSourceResolver::resolve(const std::string &source, const std::string &vectorCacheDir) {
    ResolvedVectorSource result;
    result.originalSource = source;
    result.effectivePath = source;
    result.safeLogName = source;

    if (!isHttpUrl(source)) return result;

    result.remote = true;
    result.safeLogName = redactUrlForLog(source);
    if (!vectorCacheDir.empty()) {
        const std::filesystem::path dir = std::filesystem::path(vectorCacheDir) / "url";
        result.cachePath = (dir / stableCacheKey(source)).string();
    }
    return result;
}

} // @Jin namespace globecore
