// lite_gdal_stubs.cpp —— LITE 构建下 GDAL/PROJ/KML 依赖链的最小桩实现
//
// 为什么需要这个（feature/terrain-pre）：
// GDAL 3.7 + PROJ 9 在 OHOS arm64-v8a 上交叉编译成本极高（依赖链长、需 sqlite3/expat/uriparser/
// libkml 全套，且 GDAL 单目标常编 30~40 分钟），而「在线瓦片 + 三维地形」这条验证链路根本不碰 GDAL。
// 引擎的设计恰好让裁剪变得干净：**依赖 GDAL/PROJ 的只有 8 个 .cpp，头文件一个都不依赖**
// （RasterRenderer.h / VectorReader.h / GdalBootstrap.h 全是纯 std 声明）。
// 于是 LITE 构建只需：不编那 8 个 .cpp、不链 gdal/proj/kml/*.a，再用本文件补上被调用到的符号。
//
// 行为约定：桩一律返回「不可用」而非崩溃——
//   - 布尔型返回 false（调用方按失败分支处理，如 addRasterLayer 返回错误、矢量层不显示）
//   - 字符串型返回空串（NAPI 侧已有 null/空串处理）
//   - readVectorFile 返回 ok=false + 中文 error，ArkTS 门面会把 error 透出给用户
// 这样即便调用到裁剪掉的能力，也表现为「功能提示不可用」而不是 .so 加载失败（未定义符号）。
//
// 编译守卫：仅在 CMake 侧 -DGLOBECORE_LITE=ON 时把本文件加入目标；非 LITE 构建不参与，
// 与真实实现零冲突（真实 .cpp 在 LITE 下不编译）。

#include "bridge/bridge_api.h"
#include "raster/RasterRenderer.h"
#include "vector/GdalBootstrap.h"
#include "vector/VectorReader.h"
#include "ohos/lite_geojson_reader.h" // @Jin LITE 下无 GDAL 的 GeoJSON 最小读取通路。

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace globecore {
namespace {

// @Jin LITE 只把 JSON/GeoJSON 文件接管到自带 reader；shp/kml/tif 等仍明确提示需要非 LITE/GDAL 构建。
bool hasGeoJsonExtension(const std::string &path) {
    auto endsWith = [&](const std::string &suffix) {
        if (path.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); ++i) {
            const char actual = static_cast<char>(
                std::tolower(static_cast<unsigned char>(path[path.size() - suffix.size() + i])));
            if (actual != suffix[i]) return false;
        }
        return true;
    };
    return endsWith(".geojson") || endsWith(".json");
}

} // @Jin anonymous namespace

// ── RasterRenderer：本地 GeoTIFF/img 整幅或分瓦片重投影渲染（依赖 GDAL warp）──

bool RasterRenderer::renderTile(const std::string &path, double minLon, double minLat,
                                double maxLon, double maxLat, int size,
                                std::vector<uint8_t> &outPng) {
    (void)path; (void)minLon; (void)minLat; (void)maxLon; (void)maxLat; (void)size;
    outPng.clear();
    return false;
}

bool RasterRenderer::info(const std::string &path, double &outMinLon, double &outMinLat,
                          double &outMaxLon, double &outMaxLat, int &outMaxLevel) {
    (void)path; (void)outMinLon; (void)outMinLat; (void)outMaxLon; (void)outMaxLat; (void)outMaxLevel;
    return false;
}

bool RasterRenderer::info(const std::string &path, double &outMinLon, double &outMinLat,
                          double &outMaxLon, double &outMaxLat, int &outMaxLevel,
                          std::string &outError) {
    (void)path; (void)outMinLon; (void)outMinLat; (void)outMaxLon; (void)outMaxLat; (void)outMaxLevel;
    outError = "当前为 LITE 构建（未链接 GDAL），不支持本地栅格文件直读";
    return false;
}

bool RasterRenderer::renderFullImage(const std::string &path, int maxSide, long long maxPixels,
                                     int &outW, int &outH, std::vector<uint8_t> &outRgba,
                                     double &outMinLon, double &outMinLat,
                                     double &outMaxLon, double &outMaxLat,
                                     std::string &outError,
                                     const std::string &sidecarDir) {
    (void)path; (void)maxSide; (void)maxPixels; (void)sidecarDir;
    outW = 0; outH = 0;
    outRgba.clear();
    outMinLon = 0.0; outMinLat = 0.0; outMaxLon = 0.0; outMaxLat = 0.0;
    outError = "当前为 LITE 构建（未链接 GDAL），不支持本地栅格文件直读";
    return false;
}

// ── GdalBootstrap：GDALAllRegister 一次性引导（GlobeEngine 构造时调用）──

void ensureGdalRegistered() {
    // LITE：无 GDAL 可注册，保持空实现
}

// ── VectorReader：OGR 矢量文件读取 ──

VectorReadResult readVectorFile(const std::string &path, const std::string &labelField,
                                bool hasExtent,
                                double minLon, double minLat, double maxLon, double maxLat,
                                int maxFeatures,
                                const std::unordered_map<long long, std::string> *labelFieldOverrides) {
    // @Jin 远程 URL 已由共享 VectorLoader 下载为 .geojson 私有缓存；LITE 在这里直接解析缓存，不链接 OGR。
    if (hasGeoJsonExtension(path)) {
        return readLiteGeoJsonFile(path, labelField, hasExtent, minLon, minLat, maxLon, maxLat,
                                   maxFeatures, labelFieldOverrides);
    }
    VectorReadResult r;
    r.ok = false;
    r.error = "当前为 LITE 构建（未链接 GDAL/OGR），仅支持 GeoJSON 矢量文件";
    return r;
}

} // namespace globecore

namespace gcbridge {

// ── srs.cpp：PROJ 坐标系变换 ──

int projVersion() {
    return 0;
}

void initProjDataPath(const std::string &path) {
    (void)path;
}

bool convert(double x, double y, const std::string &srcCrs, const std::string &tgtCrs,
             double &outX, double &outY) {
    (void)x; (void)y; (void)srcCrs; (void)tgtCrs; (void)outX; (void)outY;
    return false;
}

// ── gdal_info.cpp ──

std::string gdalVersion() {
    return "0"; // 0 = 不可用；ArkTS 侧 nativeGetGdalVersion 直接透出
}

std::string vectorDrivers() {
    return "";
}

// ── layer_info.cpp ──

bool layerExtent(const std::string &path, double extent[4]) {
    (void)path; (void)extent;
    return false;
}

std::vector<std::string> vectorFieldNames(const std::string &path) {
    (void)path;
    return {};
}

std::string layerSrs(const std::string &path) {
    (void)path;
    return "";
}

// ── vector_io.cpp ──

std::string readVectorFeatures(const std::string &path,
                               double minLon, double minLat, double maxLon, double maxLat,
                               bool hasFilter, bool includeAll, const std::string &labelField,
                               bool simplify, double simplifyTolerance) {
    (void)path; (void)minLon; (void)minLat; (void)maxLon; (void)maxLat;
    (void)hasFilter; (void)includeAll; (void)labelField;
    (void)simplify; (void)simplifyTolerance;
    return ""; // 空串 = 数据源打不开（与 Android JNI 返回 null 同义）
}

std::string queryVectorFeatures(const std::string &path, const std::string &sql) {
    (void)path; (void)sql;
    return "";
}

long long countVectorFeatures(const std::string &path) {
    (void)path;
    return -1;
}

bool updateFeatureAttributes(const std::string &path, long long featureId,
                             const std::vector<std::string> &keys,
                             const std::vector<std::string> &values) {
    (void)path; (void)featureId; (void)keys; (void)values;
    return false;
}

std::string getFeatureAttributes(const std::string &path, long long featureId) {
    (void)path; (void)featureId;
    return "";
}

} // namespace gcbridge
