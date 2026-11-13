#ifndef GLOBECORE_GLOBE_GEOGRAPHIC_QUADTREE_H
#define GLOBECORE_GLOBE_GEOGRAPHIC_QUADTREE_H

#include <vector>
#include <chrono>

#include "geom/Vec3.h"
#include "globe/Globe.h"
#include "globe/Tessellator.h"

namespace globecore {

/// @Jin T3g（实验/可回退）：逐瓦片地形高程查询接口。旧实现把视口/全局最高山脊高程统一抬升到
/// 每一个瓦片的 AABB，山谷瓦片被过度抬高后在低空被近裁面裁掉（真机：山区 z16+ 倾角接近时周边黑块）。
/// 本接口让四叉树按每个瓦片 bbox 查询其真实覆盖的最大高程。回退：buildVisibleTiles 传 nullptr 即恢复统一余量路径。
class ITileTerrainQuery {
public:
    virtual ~ITileTerrainQuery() = default;
    /// 返回与瓦片 bbox 相交的 DEM 中的最大地表高程（米，椭球面以上）；无任何 DEM 覆盖返回 0。
    virtual double maxTerrainHeightForBounds(double lonMin, double latMin,
                                             double lonMax, double latMax) const = 0;
};


/**
 * GeographicQuadtree：3D 球体模式下带帧间一致性的可见瓦片选取器（#11 优化）。
 *
 * 与 [Tessellator] 的关键差异：
 *
 *  1. **帧间一致性（frame coherence）**：[Tessellator] 每帧从根瓦片 DFS 全量重建可见叶集；
 *     本类缓存上帧可见叶集与相机快照，相机微动（< 阈值）时直接复用上帧结果（零递归开销），
 *     仅在相机显著移动/参数变更时触发全量重建；@Jin 朝向变化不视为微动，避免倾斜/旋转后复用旧叶集。
 *
 *  2. **极冠覆盖（polar cap）**：Web 墨卡托在 ±85.0511° 截断，极冠 85.05°~90° 无瓦片纹理。
 *     本类对触极行瓦片（latMax≥85.0511° 或 latMin≤-85.0511°）输出时把经纬度边界外推到 ±90°，
 *     使贴球网格覆盖完整极冠（纹理仍取末级行拉伸，视觉无空洞）。
 *
 *  3. **几何地平线剔除**：同 Tessellator 的 dot(q,ê)·|E| ≥ |q|² 判据，但增加瓦片中心点的
 *     精确地平线测试（减少粗级瓦片的保守保留）。
 *
 * 纹理键仍用 Web 墨卡托 (z,x,y)——与 TileCache/TileLoader/URL 模板全链路兼容，不改变瓦片加载管线。
 * 输出的 [GlobeTile] 与 Tessellator 完全同构，Renderer 无感知切换。
 */
class GeographicQuadtree {
public:
    GeographicQuadtree() = default;

    /// 构建可见叶瓦片集合。相机微动时复用上帧结果（增量模式）；显著移动/参数变更时全量重建。
    /// 接口与 Tessellator::buildVisibleTiles 对齐，可直接替换。
    // @Jin T3g：terrainQuery 非空时逐瓦片查真实最大高程；terrainCullMarginM 仅作无接口回退与帧间变化检测代理。
    void buildVisibleTiles(const TessCamera &cam, int targetLevel, int maxLevel,
                           double tanHalfFov, int viewportHeightPx, double detailFactor,
                           const Globe &globe, std::vector<GlobeTile> &out,
                           double terrainCullMarginM = 0.0,
                           const ITileTerrainQuery *terrainQuery = nullptr);

    /// 清除帧间缓存（上下文重建/换图源时调用）

    void clear();

private:
    /// 帧间缓存：上帧相机状态 + 可见瓦片集
    bool hasPrev_ = false;
    Vec3 prevEye_{};
    // @Jin 上帧相机基：forward 相同但 up 不同（俯视时旋转）也必须触发重建。
    Vec3 prevForward_{};
    Vec3 prevUp_{};
    double prevTanHalfFov_ = -1.0;
    int prevViewportH_ = -1;
    int prevTargetLevel_ = -1;
    int prevMaxLevel_ = -1;
    double prevDetailFactor_ = -1.0;
    // @Jin 地形剔除余量变化（首批 DEM 到达或清空地形）也必须重建可见叶集。
    double prevTerrainCullMarginM_ = -1.0;
    // @Jin T3h（实验/可回退）：仅地形修订变化导致的叶集重建节流，缓解 DEM 批量注入期的帧尖峰（实测 220~477ms）与占位块滞留。
    std::chrono::steady_clock::time_point lastRevisionRebuildTime_{};
    bool hasLastRevisionRebuildTime_ = false;
    std::vector<GlobeTile> prevVisible_;

    /// @Jin 判断相机是否发生会影响可见叶集的变化；朝向变化必须立即重建，眼点阈值按离地高度缩放。
    bool cameraChangedSignificantly(const TessCamera &cam, double eyeAlt, double tanHalfFov,
                                    int viewportH, int targetLevel, int maxLevel,
                                    double detailFactor, double terrainCullMarginM) const;
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_GEOGRAPHIC_QUADTREE_H
