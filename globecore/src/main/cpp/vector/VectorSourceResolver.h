#ifndef GLOBECORE_VECTOR_SOURCE_RESOLVER_H
#define GLOBECORE_VECTOR_SOURCE_RESOLVER_H

#include <string>

namespace globecore {

/**
 * @Jin 在线矢量源解析结果：source 仍保留用户原始输入，effectivePath 只在远程下载成功后指向缓存文件。
 */
// @Jin GeoJSON 专用数据源种类：File=本地文件，Url=http(s) 在线地址，Data=GeoJSON 字符串（落 inline 缓存）。
enum class SourceKind { File, Url, Data };

struct ResolvedVectorSource {
    SourceKind kind = SourceKind::File;
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
    // @Jin 显式来源解析：addGeoJSONLayer 经 url/data 互斥校验后传入确定的 kind，不再对单一字符串做内容嗅探。
    static ResolvedVectorSource resolve(SourceKind kind, const std::string &value,
                                        const std::string &vectorCacheDir);
    // @Jin 旧 addVectorLayer 通路保留：按 http 前缀隐式映射 File/Url（弃用期内零回归）。
    static ResolvedVectorSource resolve(const std::string &source, const std::string &vectorCacheDir);
    static std::string stableCacheKey(const std::string &url);
    static std::string redactUrlForLog(const std::string &url);
};

} // @Jin namespace globecore

#endif
