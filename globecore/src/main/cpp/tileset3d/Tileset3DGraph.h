#ifndef GLOBECORE_TILESET3D_TILESET3DGRAPH_H
#define GLOBECORE_TILESET3D_TILESET3DGRAPH_H

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "geom/Matrix4.h"
#include "geom/Vec3.h"
#include "layer/TilesetJson.h"
#include "tileset3d/Tileset3DTransform.h"

namespace globecore {

/**
 * @Jin 运行时保守包围球；box 会转成外接球，region 的精确处理留待后续阶段。
 */
struct Tileset3DBounds {
    bool valid = false;
    Vec3 center;
    double radius = 0.0;
};

/**
 * @Jin 展开后的 tile 运行时节点；源 TilesetNode 由 Tileset3DGraph::tileset（unique_ptr 堆对象）持有，
 * 本结构只保存索引、指向该堆对象内部节点的裸指针和世界变换；图移动不影响其有效性。
 */
struct Tileset3DRuntimeNode {
    int id = -1;
    int parentId = -1;
    const TilesetNode *source = nullptr;
    Tileset3DTransform::Matrix worldTransform = Tileset3DTransform::identity();
    Tileset3DBounds worldBounds;
    std::vector<int> childIds;
};

/**
 * @Jin 一份 tileset.json 对应的运行时图。
 */
struct Tileset3DGraph {
    std::string baseUrl;
    // @Jin 修复悬空指针（P3 真机 URL 乱码根因）：TilesetResult::root 是值类型内嵌成员，
    // 若直接按值持有，Tileset3DGraph 在 build 返回/Provider move 赋值后 root 随对象搬到新地址，
    // runtime 节点里指向 root 的 source 裸指针立即悬空（vector 内的子节点因堆缓冲随 move 转移而幸免）。
    // 改为 unique_ptr 堆托管后，图本身怎么 move，TilesetResult 及其节点树地址始终不变，source 在图生命周期内有效。
    std::unique_ptr<TilesetResult> tileset;
    int rootId = -1;
    std::vector<Tileset3DRuntimeNode> nodes;

    // @Jin runtime 节点保存指向 tileset 内部节点的裸指针；拷贝已删除，移动只转移 unique_ptr 不搬动节点树，不会悬空。
    Tileset3DGraph() = default;
    Tileset3DGraph(const Tileset3DGraph &) = delete;
    Tileset3DGraph &operator=(const Tileset3DGraph &) = delete;
    Tileset3DGraph(Tileset3DGraph &&) = default;
    Tileset3DGraph &operator=(Tileset3DGraph &&) = default;
};

/**
 * @Jin 选中的 tile 内容；P1 只输出决策，不触发下载和 GL 上传。
 */
struct Tileset3DSelection {
    Tileset3DTransform::Matrix worldTransform = Tileset3DTransform::identity();
    int nodeId = -1;
    std::string contentUrl;
    std::string refine;
    double geometricError = 0.0;
    Vec3 boundsCenter;
    double boundsRadius = 0.0;
    bool isB3dm = false;
    bool isExternalTileset = false;
};

/**
 * @Jin 3D Tiles 图构建与基础 SSE 选择器。
 */
class Tileset3DGraphBuilder {
public:
    // @Jin 递归累积 transform，并把 box/sphere 转成世界坐标保守包围球。
    static Tileset3DGraph build(const std::string &baseUrl, const TilesetResult &tileset,
                                const Tileset3DTransform::Matrix &baseTransform = Tileset3DTransform::identity());

    // @Jin 基于视锥、距离和屏幕空间误差选择内容；frustumPlanes 传空指针时跳过视锥裁剪，便于离线验证。
    static std::vector<Tileset3DSelection> select(
            const Tileset3DGraph &graph,
            const Vec3 &eyeEcef,
            int viewportHeightPx,
            double fovYRad,
            double maximumScreenSpaceError,
            const std::array<FrustumPlane, 6> *frustumPlanes = nullptr);

private:
    static int addNode(Tileset3DGraph &graph, const TilesetNode &node,
                       const Tileset3DTransform::Matrix &parentWorld, int parentId);
    static Tileset3DBounds worldBounds(const TilesetNode &node,
                                       const Tileset3DTransform::Matrix &world);
    static void traverse(const Tileset3DGraph &graph, int nodeId,
                         const Vec3 &eyeEcef, int viewportHeightPx,
                         double fovYRad, double maximumScreenSpaceError,
                         const std::array<FrustumPlane, 6> *frustumPlanes,
                         std::vector<Tileset3DSelection> &out);
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DGRAPH_H



