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

// @Jin GeoJSON 专用接口的显式来源解析：File 直通本地路径；Url 落 url/ 缓存目录；Data 按内容哈希落 inline/ 目录。
ResolvedVectorSource VectorSourceResolver::resolve(SourceKind kind, const std::string &value,
                                                   const std::string &vectorCacheDir) {
    ResolvedVectorSource result;
    result.kind = kind;
    result.originalSource = value;
    result.effectivePath = value;
    result.safeLogName = value;

    if (kind == SourceKind::File) {
        return result;
    }

    result.remote = (kind == SourceKind::Url);
    if (kind == SourceKind::Url) {
        result.safeLogName = redactUrlForLog(value);
    } else {
        // @Jin 字符串内容不进日志/路径，只暴露 inline: + 哈希键，避免大段 GeoJSON 刷屏或敏感属性泄漏。
        result.safeLogName = "inline:" + stableCacheKey(value);
    }
    result.effectivePath.clear();
    if (!vectorCacheDir.empty()) {
        const std::string subDir = kind == SourceKind::Url ? "url" : "inline";
        const std::filesystem::path dir = std::filesystem::path(vectorCacheDir) / subDir;
        result.cachePath = (dir / stableCacheKey(value)).string();
    }
    return result;
}

ResolvedVectorSource VectorSourceResolver::resolve(const std::string &source, const std::string &vectorCacheDir) {
    // @Jin 旧通路：维持 P0 的 http 前缀隐式判定，委托新实现避免两套逻辑漂移。
    return resolve(isHttpUrl(source) ? SourceKind::Url : SourceKind::File, source, vectorCacheDir);
}

} // @Jin namespace globecore
