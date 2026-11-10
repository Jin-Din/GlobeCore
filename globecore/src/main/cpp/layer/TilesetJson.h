#ifndef GLOBECORE_LAYER_TILESETJSON_H
#define GLOBECORE_LAYER_TILESETJSON_H

#include <array>
#include <string>
#include <vector>

namespace globecore {

/**
 * TilesetJson：3D Tiles 1.0 tileset.json 解析器。
 *
 * 输出 tile 树结构（transform + boundingVolume + geometricError + refine + content.uri + children），
 * 供宿主按 LOD 选择加载哪个 tile 的 content（.b3dm/.glb/.json）。
 *
 * boundingVolume 支持 box（OBB 12 float）/ region（6 float 经纬度+高度）/ sphere（4 float）。
 * geometricError 用于屏幕误差 LOD：误差大于此值时细分子节点。
 * refine：ADD（叠加子节点）/ REPLACE（用子节点替换自身）。
 *
 * 纯 CPU、无 GL 依赖，与 parseJson 配合使用。
 */
struct TilesetContent {
    // @Jin 3D Tiles 1.0 推荐字段为 uri，旧数据可能仍使用 url；解析时两者兼容。
    std::string uri;
};

struct TilesetNode {
    // @Jin 列主序 4x4 变换矩阵，默认单位矩阵；语义为 tile 局部坐标到父节点/ECEF 坐标的变换。
    std::array<double, 16> transform = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };

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
    // @Jin 记录资产声明的上轴；真实 mars3d/osgb 数据常见 gltfUpAxis=Z，渲染阶段需做轴向转换。
    std::string gltfUpAxis = "Y";
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
