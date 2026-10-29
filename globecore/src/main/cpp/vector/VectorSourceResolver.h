#ifndef GLOBECORE_VECTOR_SOURCE_RESOLVER_H
#define GLOBECORE_VECTOR_SOURCE_RESOLVER_H

#include <string>

namespace globecore {

/**
 * @Jin 在线矢量源解析结果：source 仍保留用户原始输入，effectivePath 只在远程下载成功后指向缓存文件。
 */
struct ResolvedVectorSource {
    bool remote = false;
    std::string originalSource;
    std::string effectivePath;
    std::string cachePath;
    std::string safeLogName;
};

/**
 * @Jin P0 在线 GeoJSON Source 的最小 URL/缓存路径解析器，不依赖 GDAL /vsicurl/。
 */
class VectorSourceResolver {
public:
    static bool isHttpUrl(const std::string &source);
    static ResolvedVectorSource resolve(const std::string &source, const std::string &vectorCacheDir);
    static std::string stableCacheKey(const std::string &url);
    static std::string redactUrlForLog(const std::string &url);
};

} // @Jin namespace globecore

#endif
