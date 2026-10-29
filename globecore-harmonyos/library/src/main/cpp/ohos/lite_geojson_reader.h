#ifndef GLOBECORE_OHOS_LITE_GEOJSON_READER_H
#define GLOBECORE_OHOS_LITE_GEOJSON_READER_H

#include <string>
#include <unordered_map>

#include "vector/VectorReader.h"

namespace globecore {

/**
 * @Jin HarmonyOS NEXT LITE 构建专用 GeoJSON 读取器。
 * @Jin 该路径不链接 GDAL/OGR，只支持 RFC 7946 中 WGS84 GeoJSON 的常见几何，
 * @Jin 供在线 GeoJSON 下载到缓存后复用 VectorBuilder/VBO/拾取/贴地管线。
 */
VectorReadResult readLiteGeoJsonFile(const std::string &path,
                                     const std::string &labelField,
                                     bool hasExtent,
                                     double minLon, double minLat,
                                     double maxLon, double maxLat,
                                     int maxFeatures,
                                     const std::unordered_map<long long, std::string> *labelFieldOverrides);

} // @Jin namespace globecore

#endif
