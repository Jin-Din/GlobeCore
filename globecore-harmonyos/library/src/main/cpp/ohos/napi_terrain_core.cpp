// @Jin napi_terrain_core.cpp —— Cesium 地形通用核心的 HarmonyOS NAPI 薄桥接。
// @Jin HTTP、磁盘缓存和 UI 仍由宿主/HAR ArkTS 负责；本文件只暴露 layer.json、网格数学与 available 判断。
#include <napi/native_api.h>

#include <cstdint>
#include <vector>

#include "terrain/CesiumTerrain.h"
#include "util/Log.h"

#include "napi_util.h"

namespace gcohos {

namespace {

// @Jin NAPI 句柄直接存放 CesiumTerrainMeta 指针；创建/销毁由 Provider 生命周期管理。
inline globecore::CesiumTerrainMeta *toTerrainMeta(int64_t handle) {
    return reinterpret_cast<globecore::CesiumTerrainMeta *>(handle);
}

// @Jin 解析 layer.json 并创建核心句柄；失败返回 0。
napi_value NativeCesiumTerrainCreate(napi_env env, napi_callback_info info) {
    napi_value args[2];
    NapiGetArgs(env, info, 2, args);
    const std::string jsonText = NapiGetString(env, args[0]);
    int32_t fallbackMaxZoom = 15;
    NapiGetInt32(env, args[1], fallbackMaxZoom);
    auto *meta = new globecore::CesiumTerrainMeta(
        globecore::parseCesiumTerrainLayerJson(jsonText, fallbackMaxZoom));
    if (!meta->ok) {
        LOGW("CesiumTerrainCore: layer.json parse failed: %s", meta->error.c_str());
        delete meta;
        return NapiMakeInt64(env, 0);
    }
    LOGI("CesiumTerrainCore: create projection=%s scheme=%s maxzoom=%d availableLevels=%zu",
         meta->projection.c_str(), meta->scheme.c_str(), meta->maxZoom, meta->available.size());
    return NapiMakeInt64(env, reinterpret_cast<int64_t>(meta));
}

// @Jin 释放核心句柄。
napi_value NativeCesiumTerrainDestroy(napi_env env, napi_callback_info info) {
    napi_value args[1];
    NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    NapiGetInt64(env, args[0], handle);
    delete toTerrainMeta(handle);
    return NapiMakeNull(env);
}

// @Jin 返回 layer.json 的有效 maxzoom。
napi_value NativeCesiumTerrainMaxZoom(napi_env env, napi_callback_info info) {
    napi_value args[1];
    NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    NapiGetInt64(env, args[0], handle);
    const auto *meta = toTerrainMeta(handle);
    return NapiMakeInt32(env, meta == nullptr ? -1 : meta->maxZoom);
}

// @Jin 返回 layer.json 的瓦片 URL 模板。
napi_value NativeCesiumTerrainTileTemplate(napi_env env, napi_callback_info info) {
    napi_value args[1];
    NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    NapiGetInt64(env, args[0], handle);
    const auto *meta = toTerrainMeta(handle);
    return NapiMakeString(env, meta == nullptr ? "" : meta->tileTemplate);
}

// @Jin 返回引擎瓦片相交的全部服务 DEM 编号，摊平为 [z,x,y,z,x,y,...]。
napi_value NativeCesiumTerrainEngineToServiceTiles(napi_env env, napi_callback_info info) {
    napi_value args[4];
    NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    int32_t z = 0;
    int32_t x = 0;
    int32_t y = 0;
    NapiGetInt64(env, args[0], handle);
    NapiGetInt32(env, args[1], z);
    NapiGetInt32(env, args[2], x);
    NapiGetInt32(env, args[3], y);
    const auto *meta = toTerrainMeta(handle);
    if (meta == nullptr) return NapiMakeNull(env);
    const std::vector<globecore::TerrainServiceTile> tiles =
        globecore::cesiumServiceTilesForEngineTile(*meta, z, x, y);
    std::vector<double> flat;
    flat.reserve(tiles.size() * 3);
    for (const auto &tile : tiles) {
        flat.push_back(tile.z);
        flat.push_back(tile.x);
        flat.push_back(tile.y);
    }
    return NapiMakeFloat64Array(env, flat);
}

// @Jin 返回服务瓦片的经纬度四至 [lonMin,latMin,lonMax,latMax]。
napi_value NativeCesiumTerrainTileBounds(napi_env env, napi_callback_info info) {
    napi_value args[3];
    NapiGetArgs(env, info, 3, args);
    int32_t z = 0;
    int32_t x = 0;
    int32_t y = 0;
    NapiGetInt32(env, args[0], z);
    NapiGetInt32(env, args[1], x);
    NapiGetInt32(env, args[2], y);
    const globecore::TerrainGeoBounds bounds = globecore::cesiumGeoTileBounds(z, x, y);
    const double values[4] = {bounds.lonMin, bounds.latMin, bounds.lonMax, bounds.latMax};
    return NapiMakeFloat64Array(env, values, 4);
}

// @Jin 判断服务瓦片是否在 layer.json available 范围内。
napi_value NativeCesiumTerrainIsAvailable(napi_env env, napi_callback_info info) {
    napi_value args[4];
    NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    int32_t z = 0;
    int32_t x = 0;
    int32_t y = 0;
    NapiGetInt64(env, args[0], handle);
    NapiGetInt32(env, args[1], z);
    NapiGetInt32(env, args[2], x);
    NapiGetInt32(env, args[3], y);
    const auto *meta = toTerrainMeta(handle);
    return NapiMakeBool(env, meta != nullptr && globecore::isCesiumTerrainTileAvailable(*meta, z, x, y));
}

} // namespace

// @Jin 注册地形核心 API。
napi_value RegisterTerrainCore(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeCesiumTerrainCreate", nullptr, NativeCesiumTerrainCreate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainDestroy", nullptr, NativeCesiumTerrainDestroy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainMaxZoom", nullptr, NativeCesiumTerrainMaxZoom, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainTileTemplate", nullptr, NativeCesiumTerrainTileTemplate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainEngineToServiceTiles", nullptr, NativeCesiumTerrainEngineToServiceTiles, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainTileBounds", nullptr, NativeCesiumTerrainTileBounds, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCesiumTerrainIsAvailable", nullptr, NativeCesiumTerrainIsAvailable, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace gcohos
