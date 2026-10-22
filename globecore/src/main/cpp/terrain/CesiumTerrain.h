#ifndef GLOBECORE_TERRAIN_CESIUMTERRAIN_H
#define GLOBECORE_TERRAIN_CESIUMTERRAIN_H

#include <string>
#include <vector>

namespace globecore {

/**
 * @Jin CesiumTerrain：Cesium quantized-mesh 地形服务的平台无关核心工具。
 * @Jin 本模块负责 layer.json 解析、可用范围判断，以及 EPSG:4326/TMS 服务网格
 * @Jin 与引擎 Web Mercator 瓦片网格之间的 bbox 求交；不包含 HTTP 与平台 UI。
 */
struct TerrainGeoBounds {
    double lonMin = 0.0;
    double latMin = 0.0;
    double lonMax = 0.0;
    double latMax = 0.0;
};

/** @Jin layer.json available 数组中的一块闭区间瓦片范围。 */
struct TerrainAvailableRange {
    int startX = 0;
    int startY = 0;
    int endX = -1;
    int endY = -1;
};

/** @Jin 一个服务 DEM 瓦片的 z/x/y 编号。 */
struct TerrainServiceTile {
    int z = 0;
    int x = 0;
    int y = 0;
};

/** @Jin layer.json 的通用地形元数据。 */
struct CesiumTerrainMeta {
    bool ok = false;
    std::string error;
    std::string format;
    std::string scheme;
    std::string projection;
    std::string tileTemplate = "{z}/{x}/{y}.terrain";
    int minZoom = 0;
    int maxZoom = 15;
    std::vector<std::vector<TerrainAvailableRange>> available;
};

/**
 * @Jin 解析 Cesium 地形 layer.json。
 * @Jin fallbackMaxZoom 用于服务未声明 maxzoom 时保留宿主配置。
 */
CesiumTerrainMeta parseCesiumTerrainLayerJson(const std::string &jsonText, int fallbackMaxZoom);

/** @Jin 判断服务瓦片是否位于 layer.json 声明的 available 覆盖范围内。 */
bool isCesiumTerrainTileAvailable(const CesiumTerrainMeta &meta, int z, int x, int y);

/** @Jin 计算 Cesium GeographicTilingScheme 瓦片的经纬度四至。 */
TerrainGeoBounds cesiumGeoTileBounds(int z, int x, int y);

/** @Jin 计算引擎 Web Mercator 瓦片的经纬度四至。 */
TerrainGeoBounds webMercatorTileBounds(int z, int x, int y);

/**
 * @Jin 找出一个引擎墨卡托瓦片真正相交、且服务声明可用的所有 Cesium 服务瓦片。
 * @Jin 输出按 x 再 y 升序排列，保持现有 ArkTS Provider 的请求顺序。
 */
std::vector<TerrainServiceTile> cesiumServiceTilesForEngineTile(
    const CesiumTerrainMeta &meta, int z, int x, int y);

/**
 * @Jin 把经纬度转换为 Web Mercator 引擎瓦片号。
 * @Jin 返回 [tx, ty]；纬度按 Web Mercator 上限钳制。
 */
void lonLatToWebMercatorTile(int z, double lon, double lat, int &outX, int &outY);

} // namespace globecore

#endif // GLOBECORE_TERRAIN_CESIUMTERRAIN_H
