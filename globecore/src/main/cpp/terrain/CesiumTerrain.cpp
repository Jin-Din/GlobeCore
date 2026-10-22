#include "terrain/CesiumTerrain.h"

#include "util/JsonParser.h"

#include <algorithm>
#include <cmath>

namespace globecore {

namespace {

// @Jin C++17 不保证 kPi 在所有工具链中可见，这里显式定义统一常量。
constexpr double kPi = 3.14159265358979323846;

// @Jin 读取可选字符串字段；类型不符时保留默认值。
std::string jsonStringOr(const JsonValue &obj, const char *key, const std::string &fallback) {
    const JsonValue &value = obj.at(key);
    return value.isString() ? value.strVal : fallback;
}

// @Jin 读取可选整数字段；类型不符时保留默认值。
int jsonIntOr(const JsonValue &obj, const char *key, int fallback) {
    const JsonValue &value = obj.at(key);
    return value.isNumber() ? static_cast<int>(std::trunc(value.numVal)) : fallback;
}

// @Jin 把经度归一化到 [-180, 180]，避免极小浮点越界导致瓦片号落在域外。
double wrapLongitude(double lon) {
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    return lon;
}

// @Jin 计算给定 Web Mercator 归一化纵坐标对应的纬度。
double mercatorLatitudeAtNorm(double yNorm) {
    const double clamped = std::max(0.0, std::min(1.0, yNorm));
    return std::atan(std::sinh(kPi * (1.0 - 2.0 * clamped))) * 180.0 / kPi;
}

// @Jin 返回服务地理网格与墨卡托 bbox 真正有面积重叠的瓦片范围 [x0,y0,x1,y1]。
std::vector<int> intersectingGeoTileRange(int z, const TerrainGeoBounds &bounds) {
    const double cols = std::ldexp(1.0, z + 1);
    const double rows = std::ldexp(1.0, z);
    const double lonSpan = 360.0 / cols;
    const double latSpan = 180.0 / rows;
    // @Jin 共边不算相交：向内收缩一个极小 epsilon，避免多请求相邻无效 DEM。
    constexpr double epsilon = 1e-10;
    int x0 = static_cast<int>(std::floor((bounds.lonMin + 180.0 + epsilon) / lonSpan));
    int x1 = static_cast<int>(std::floor((bounds.lonMax + 180.0 - epsilon) / lonSpan));
    int y0 = static_cast<int>(std::floor((bounds.latMin + 90.0 + epsilon) / latSpan));
    int y1 = static_cast<int>(std::floor((bounds.latMax + 90.0 - epsilon) / latSpan));
    x0 = std::max(0, std::min(static_cast<int>(cols) - 1, x0));
    x1 = std::max(0, std::min(static_cast<int>(cols) - 1, x1));
    y0 = std::max(0, std::min(static_cast<int>(rows) - 1, y0));
    y1 = std::max(0, std::min(static_cast<int>(rows) - 1, y1));
    return {x0, y0, x1, y1};
}

} // namespace

CesiumTerrainMeta parseCesiumTerrainLayerJson(const std::string &jsonText, int fallbackMaxZoom) {
    CesiumTerrainMeta meta;
    meta.maxZoom = std::max(0, fallbackMaxZoom);
    JsonValue root = parseJson(jsonText);
    if (!root.isObject()) {
        meta.error = "layer.json 解析失败或根节点不是对象";
        return meta;
    }

    meta.format = jsonStringOr(root, "format", "");
    meta.scheme = jsonStringOr(root, "scheme", "");
    meta.projection = jsonStringOr(root, "projection", "");
    meta.minZoom = std::max(0, jsonIntOr(root, "minzoom", 0));
    meta.maxZoom = std::max(meta.minZoom, jsonIntOr(root, "maxzoom", meta.maxZoom));

    const JsonValue &tiles = root.at("tiles");
    if (tiles.isArray() && tiles.arr.size() > 0 && tiles.arr[0].isString() &&
        !tiles.arr[0].strVal.empty()) {
        // @Jin 使用服务声明的模板，兼容 .terrain、查询参数和版本参数。
        meta.tileTemplate = tiles.arr[0].strVal;
    }

    const JsonValue &available = root.at("available");
    if (available.isArray()) {
        meta.available.resize(available.arr.size());
        for (size_t level = 0; level < available.arr.size(); ++level) {
            const JsonValue &ranges = available.arr[level];
            if (!ranges.isArray()) continue;
            meta.available[level].reserve(ranges.arr.size());
            for (const JsonValue &rangeValue : ranges.arr) {
                if (!rangeValue.isObject()) continue;
                TerrainAvailableRange range;
                range.startX = jsonIntOr(rangeValue, "startX", 0);
                range.startY = jsonIntOr(rangeValue, "startY", 0);
                range.endX = jsonIntOr(rangeValue, "endX", -1);
                range.endY = jsonIntOr(rangeValue, "endY", -1);
                if (range.endX >= range.startX && range.endY >= range.startY) {
                    meta.available[level].push_back(range);
                }
            }
        }
    }

    meta.ok = true;
    return meta;
}

bool isCesiumTerrainTileAvailable(const CesiumTerrainMeta &meta, int z, int x, int y) {
    if (z < 0 || z > meta.maxZoom) return false;
    if (z >= static_cast<int>(meta.available.size())) return true;
    const auto &ranges = meta.available[static_cast<size_t>(z)];
    for (const TerrainAvailableRange &range : ranges) {
        if (x >= range.startX && x <= range.endX && y >= range.startY && y <= range.endY) {
            return true;
        }
    }
    return false;
}

TerrainGeoBounds cesiumGeoTileBounds(int z, int x, int y) {
    const double cols = std::ldexp(1.0, z + 1);
    const double rows = std::ldexp(1.0, z);
    TerrainGeoBounds bounds;
    bounds.lonMin = -180.0 + 360.0 * static_cast<double>(x) / cols;
    bounds.latMin = -90.0 + 180.0 * static_cast<double>(y) / rows;
    bounds.lonMax = -180.0 + 360.0 * static_cast<double>(x + 1) / cols;
    bounds.latMax = -90.0 + 180.0 * static_cast<double>(y + 1) / rows;
    return bounds;
}

TerrainGeoBounds webMercatorTileBounds(int z, int x, int y) {
    const double n = std::ldexp(1.0, z);
    TerrainGeoBounds bounds;
    bounds.lonMin = -180.0 + 360.0 * static_cast<double>(x) / n;
    bounds.lonMax = -180.0 + 360.0 * static_cast<double>(x + 1) / n;
    bounds.latMin = mercatorLatitudeAtNorm(static_cast<double>(y + 1) / n);
    bounds.latMax = mercatorLatitudeAtNorm(static_cast<double>(y) / n);
    return bounds;
}

std::vector<TerrainServiceTile> cesiumServiceTilesForEngineTile(
    const CesiumTerrainMeta &meta, int z, int x, int y) {
    std::vector<TerrainServiceTile> out;
    const int serviceZ = std::max(0, std::min(meta.maxZoom, z));
    const TerrainGeoBounds engineBounds = webMercatorTileBounds(z, x, y);
    const std::vector<int> range = intersectingGeoTileRange(serviceZ, engineBounds);
    for (int sx = range[0]; sx <= range[2]; ++sx) {
        for (int sy = range[1]; sy <= range[3]; ++sy) {
            if (isCesiumTerrainTileAvailable(meta, serviceZ, sx, sy)) {
                out.push_back({serviceZ, sx, sy});
            }
        }
    }
    return out;
}

void lonLatToWebMercatorTile(int z, double lon, double lat, int &outX, int &outY) {
    const double n = std::ldexp(1.0, z);
    const double safeLon = wrapLongitude(lon);
    const double latRad = std::max(-1.4844, std::min(1.4844, lat * kPi / 180.0));
    const double yNorm = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / kPi) / 2.0;
    outX = static_cast<int>(std::floor((safeLon + 180.0) / 360.0 * n));
    outY = static_cast<int>(std::floor(yNorm * n));
    const int maxIndex = static_cast<int>(n) - 1;
    outX = std::max(0, std::min(maxIndex, outX));
    outY = std::max(0, std::min(maxIndex, outY));
}

} // namespace globecore
