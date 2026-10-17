#ifndef GLOBECORE_LAYER_TILESETJSON_H
#define GLOBECORE_LAYER_TILESETJSON_H

#include <string>
#include <vector>

namespace globecore {

/**
 * TilesetJson：3D Tiles 1.0 tileset.json 解析器。
 *
 * 输出 tile 树结构（boundingVolume + geometricError + refine + content.uri + children），
 * 供宿主按 LOD 选择加载哪个 tile 的 content（.b3dm/.glb）。
 *
 * boundingVolume 支持 box（OBB 12 float）/ region（6 float 经纬度+高度）/ sphere（4 float）。
 * geometricError 用于屏幕误差 LOD：误差大于此值时细分子节点。
 * refine：ADD（叠加子节点）/ REPLACE（用子节点替换自身）。
 *
 * 纯 CPU、无 GL 依赖，与 [parseJson] 配合使用。
 */

struct TilesetContent {
    std::string uri;  // content 资源 URI（.b3dm/.glb/.i3dm 等）
};

struct TilesetNode {
    // boundingVolume（三种之一，有效位标识）
    bool hasBox = false;
    std::vector<double> box;  // OBB：center(3) + 3 half-axes(9) = 12 float
    bool hasRegion = false;
    double region[6] = {0}; // lonMin,latMin,lonMax,latMax,minHeight,maxHeight
    bool hasSphere = false;
    double sphere[4] = {0}; // center(3) + radius

    double geometricError = 0.0;
    std::string refine = "ADD"; // ADD / REPLACE
    TilesetContent content;
    std::vector<TilesetNode> children;
};

struct TilesetResult {
    bool ok = false;
    std::string error;
    double geometricError = 0.0; // asset-level geometricError
    TilesetNode root;
};

/**
 * 解析 tileset.json 文本。
 * @param jsonText  tileset.json 完整文本
 * @return          解析结果（root tile 树 + ok/error）
 */
TilesetResult parseTileset(const std::string &jsonText);

} // namespace globecore

#endif // GLOBECORE_LAYER_TILESETJSON_H
