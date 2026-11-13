#include "globe/GeographicQuadtree.h"

#include <algorithm>
#include <cmath>

namespace globecore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kDeg2Rad = kPi / 180.0;
/// 墨卡托有效纬度上限（与 MercatorProjection::maxLatitude 同口径，约 85.0511°）
constexpr double kMaxLat = 85.05112877980659;

/// Web 墨卡托瓦片 (z,x,y) → 经纬度包围盒（度）。与 Tessellator.cpp 中 tileToBounds 同口径。
/// lat 用墨卡托反投影（atan·sinh）；输出 latMin/latMax 钳到 [-kMaxLat, kMaxLat]。
inline void tileToBounds(int z, int x, int y,
                         double &lonMin, double &lonMax, double &latMin, double &latMax) {
    const double n = std::pow(2.0, static_cast<double>(z));
    lonMin = x / n * 360.0 - 180.0;
    lonMax = (x + 1.0) / n * 360.0 - 180.0;
    auto rowToLat = [n](double ty) {
        const double s = kPi * (1.0 - 2.0 * ty / n);
        double lat = std::atan(std::sinh(s)) * kRad2Deg;
        if (lat > kMaxLat) lat = kMaxLat;
        if (lat < -kMaxLat) lat = -kMaxLat;
        return lat;
    };
    latMax = rowToLat(static_cast<double>(y));       // 上行 → 北
    latMin = rowToLat(static_cast<double>(y) + 1.0); // 下行 → 南
}

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// 经度差回绕到 [-180,180]（跨日期变更线时 178° 与 -178° 实际相差 4° 而非 356°）
inline double wrapLonDelta(double deg) {
    while (deg > 180.0) deg -= 360.0;
    while (deg < -180.0) deg += 360.0;
    return deg;
}

/// 瓦片矩形中「地理上最近眼点」的那点（经/纬度各自 clamp 到瓦片边界）→ ECEF + 距眼平方。
/// 与 Tessellator.cpp nearestPointOfTile 同口径（含 360° 回绕）。
inline void nearestPointOfTile(const Vec3 &eye, double eyeLonDeg, double eyeLatDeg,
                               const Globe &globe,
                               double lonMin, double lonMax, double latMin, double latMax,
                               Vec3 &outNearest, double &outDist2, double surfaceHeightM = 0.0) {
    const double nLat = clampd(eyeLatDeg, latMin, latMax);
    double nLon = eyeLonDeg;
    if (nLon < lonMin || nLon > lonMax) {
        const double dMin = std::fabs(wrapLonDelta(eyeLonDeg - lonMin));
        const double dMax = std::fabs(wrapLonDelta(eyeLonDeg - lonMax));
        nLon = (dMin <= dMax) ? lonMin : lonMax;
    }
    // @Jin 最近点参与地平线、视锥和 SSE；地形开启时需抬到保守高程，避免近景山脊在选择阶段被剪掉。
    globe.geographicToCartesian(nLon, nLat, surfaceHeightM, outNearest);
    const Vec3 d = outNearest - eye;
    outDist2 = d.dot(d);
}

/// 递归细分上下文（同 Tessellator::SubdivCtx 精简版，去 360° 回绕以外的冗余字段）
struct SubdivCtx {
    Vec3 eye;
    Vec3 eyeDir;
    double eyeLen = 0.0;
    double eyeLonDeg = 0.0, eyeLatDeg = 0.0;
    std::array<FrustumPlane, 6> planes{};
    int targetLevel = 0;
    int maxLevel = 0;
    double tanHalfFov = 0.0;
    double viewportHeight = 1.0;
    double detailFactor = 1.0;
    double R = 0.0;
    // @Jin 当前已加载地形的全局保守高程；宁多留瓦片交给深度测试，也不能误删近景地形。
    double surfaceMarginM = 0.0;
    const Globe *globe = nullptr;
    // @Jin T3g：逐瓦片真实最大高程查询；非空时替代统一 surfaceMarginM。
    const ITileTerrainQuery *terrainQuery = nullptr;
    // @Jin T3g：逐瓦片余量（米），只覆盖栅格化/邻接误差，不再承担区域峰值（峰值由查询得到的真实 aboveMax 承担）。
    static constexpr double kPerTileMarginM = 50.0;
};

/**
 * 递归细分（同 Tessellator::subdivide 四步口径，增加极冠外推）：
 *   ① 地平线剔除（最近点 + 3×3 网格 + 极冠补点：任一可见即保留）
 *   ② 视锥 AABB 剔除（3×3 网格 ± 半球修正点）
 *   ③ 屏幕空间误差 mustSubdivide（逐纹素口径）
 *   ④ 出叶 or 4 子瓦片按 nearestDist² 升序 DFS
 *
 * #11 改进：触极行瓦片（latMax≥kMaxLat 或 latMin≤-kMaxLat）的经纬度边界外推到 ±90°，
 * 使贴球网格覆盖完整极冠 85.05°~90°（纹理仍取末级行拉伸，视觉无空洞）。
 */
void subdivide(int z, int x, int y, const SubdivCtx &c, std::vector<GlobeTile> &out) {
    double lonMin, lonMax, latMin, latMax;
    tileToBounds(z, x, y, lonMin, lonMax, latMin, latMax);

    // #11 极冠外推：墨卡托截到 ±85.0511°，极冠无瓦片。触极行瓦片边界外推到 ±90°，
    // 使贴球网格覆盖完整极冠（纹理仍取末级行拉伸，视觉无空洞）。
    const bool isNorthPolar = (latMax >= kMaxLat - 1e-9);
    const bool isSouthPolar = (latMin <= -kMaxLat + 1e-9);
    if (isNorthPolar) latMax = 90.0;
    if (isSouthPolar) latMin = -90.0;

    // 最近点（供 SSE 判据、子块 DFS 排序、地平线判据之一）
    Vec3 nearest;
    double nearestDist2 = 0.0;
    // @Jin T3g（实验/可回退）：逐节点真实地形高程。有查询接口时按本瓦片 bbox 取相交 DEM 的最大 aboveMax，
    // 山谷瓦片不再被视口内远处山脊的峰值整体抬高，从根上消除低空近裁面误剔导致的周边黑块。
    const double nodeSurfaceMargin = c.terrainQuery
        ? (c.terrainQuery->maxTerrainHeightForBounds(lonMin, latMin, lonMax, latMax) + SubdivCtx::kPerTileMarginM)
        : c.surfaceMarginM;
    // @Jin T4：最近点仍按地形安全壳抬高，服务 SSE/DFS 与视锥；地平线判定另用高 0 的 nearestHorizon。
    nearestPointOfTile(c.eye, c.eyeLonDeg, c.eyeLatDeg, *c.globe,
                      lonMin, lonMax, latMin, latMax, nearest, nearestDist2, nodeSurfaceMargin);

    // ② 视锥 AABB + ① 地平线（同 Tessellator，9 网格采样 + 半球修正 + 极冠补点 + 最近点）
    double xmin = 1e300, ymin = 1e300, zmin = 1e300;
    double xmax = -1e300, ymax = -1e300, zmax = -1e300;
    bool horizonVisible = false;
    {
        const double lonArr[3] = {lonMin, 0.5 * (lonMin + lonMax), lonMax};
        const double latArr[3] = {latMin, 0.5 * (latMin + latMax), latMax};
        Vec3 p;
        auto testHorizon = [&c, &horizonVisible](const Vec3 &q) {
            if (horizonVisible || c.eyeLen <= 0.0) return;
            if (q.dot(c.eyeDir) * c.eyeLen >= q.dot(q)) horizonVisible = true;
        };
        // @Jin T4：地理最近点的地平线判定取椭球面（高 0），地形壳只服务视锥 AABB。
        Vec3 nearestHorizon; double nearestHorizonD2 = 0.0;
        nearestPointOfTile(c.eye, c.eyeLonDeg, c.eyeLatDeg, *c.globe,
                           lonMin, lonMax, latMin, latMax, nearestHorizon, nearestHorizonD2, 0.0);
        testHorizon(nearestHorizon);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                // @Jin T3g：视锥 AABB 用逐瓦片地形安全壳，覆盖 DEM 抬升后的真实顶点范围。
                c.globe->geographicToCartesian(lonArr[i], latArr[j], nodeSurfaceMargin, p);
                if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
                if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
                // @Jin T4（实验/可回退）：地平线测试只回答是否在地球背面，采样取椭球面（高 0），与 Tessellator 同口径。
                // @Jin 旧写法把地形壳点送入地平线判据，低空眼点半径小于邻近地形壳时根节点被误判到地平线下（实测 leaves=0 整屏黑）。
                Vec3 ph;
                c.globe->geographicToCartesian(lonArr[i], latArr[j], 0.0, ph);
                testHorizon(ph);
            }
        }
        if ((lonMax - lonMin) > 180.0) {
            const double latC = 0.5 * (latMin + latMax);
            const double lonC = 0.5 * (lonMin + lonMax);
            c.globe->geographicToCartesian(lonC + 90.0, latC, nodeSurfaceMargin, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            Vec3 phW1; c.globe->geographicToCartesian(lonC + 90.0, latC, 0.0, phW1); testHorizon(phW1); // @Jin T4
            c.globe->geographicToCartesian(lonC - 90.0, latC, nodeSurfaceMargin, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            Vec3 phW2; c.globe->geographicToCartesian(lonC - 90.0, latC, 0.0, phW2); testHorizon(phW2); // @Jin T4
        }
        // 极冠补点（外推后 latMax/latMin 可能已为 90/-90，采样点自然覆盖极冠）
        if (isNorthPolar) {
            c.globe->geographicToCartesian(0.0, 90.0, nodeSurfaceMargin, p);
            Vec3 phN; c.globe->geographicToCartesian(0.0, 90.0, 0.0, phN); testHorizon(phN); // @Jin T4
        }
        if (isSouthPolar) {
            c.globe->geographicToCartesian(0.0, -90.0, nodeSurfaceMargin, p);
            Vec3 phS; c.globe->geographicToCartesian(0.0, -90.0, 0.0, phS); testHorizon(phS); // @Jin T4
        }
    }

    if (c.eyeLen > 0.0 && !horizonVisible) {
        return;
    }
    for (int pi = 0; pi < 6; ++pi) {
        const FrustumPlane &pl = c.planes[pi];
        const double px = (pl.a >= 0.0) ? xmax : xmin;
        const double py = (pl.b >= 0.0) ? ymax : ymin;
        const double pz = (pl.c >= 0.0) ? zmax : zmin;
        const double v = pl.a * px + pl.b * py + pl.c * pz + pl.d;
        // @Jin T8（实验/可回退）：侧视锥平面（0~3）有界松弛，修复低空垂直/大倾角视角周边黑块。
        // 根因：眼高 ~150m、fovY45 时视锥地面覆盖仅约 200m×124m，而单个 z18 瓦片跨度约 126~153m，
        // 保守 p-vertex AABB 测试把角点稍出视锥的整瓦剔除；SSE 又强制细化到 z18，父级不留兜底，
        // 被剔区域彻底无几何（实测垂直 z18 仅 8 叶、屏幕 50%~87% 空洞）。松弛量取约 2 个 z18 瓦片
        // 跨度（300m），等价于多保留一环邻接瓦片；高空视锥跨公里级，300m 相对可忽略，实测 88km 眼高
        // leaves 不膨胀（42~48）。near/far（4/5）保持严格，不削弱近平面穿山保护。
        // 真机数据：垂直 z18 leaves 8→85、地图区空洞 50%~87%→0%~13%（残余为 UI 色干扰）；
        // 倾角55 leaves 120→205，远低于 kMaxTiles=640 预算。回退：本块改回 if (v < 0.0)。
        constexpr double kSideFrustumCullSlackM = 300.0;
        const double cullThreshold = (pi < 4) ? -kSideFrustumCullSlackM : 0.0;
        if (v < cullThreshold) {
            return;
        }
    }

    // ③ SSE mustSubdivide（逐纹素口径，同 Tessellator）
    const double latC = 0.5 * (latMin + latMax);
    double cosLat = std::cos(latC * kDeg2Rad);
    if (cosLat < 0.05) cosLat = 0.05;
    constexpr double kTileTexels = 256.0;
    const double texelSize = 2.0 * kPi * c.R * cosLat / (std::pow(2.0, static_cast<double>(z)) * kTileTexels);
    const double nearestDist = std::sqrt(nearestDist2);
    const double pixelSize = 2.0 * nearestDist * c.tanHalfFov / c.viewportHeight;
    const bool mustSubdiv = texelSize > pixelSize * c.detailFactor;

    const bool outOfBudget = static_cast<int>(out.size()) >= Tessellator::kMaxTiles;
    const bool leaf = outOfBudget || (!mustSubdiv) || (z >= c.targetLevel) || (z >= c.maxLevel) || (z >= 30);
    if (leaf) {
        GlobeTile t;
        t.z = z;
        t.x = x;
        t.y = y;
        t.lonMin = lonMin;
        t.lonMax = lonMax;
        t.latMin = latMin;
        t.latMax = latMax; // 已含极冠外推
        out.push_back(t);
        return;
    }

    // ④ 4 子瓦片按 nearestDist² 升序 DFS（近侧优先占配额）
    struct Child { int cx, cy; double d2; };
    Child ch[4];
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            Child &cc = ch[dy * 2 + dx];
            cc.cx = 2 * x + dx;
            cc.cy = 2 * y + dy;
            double clonMin, clonMax, clatMin, clatMax;
            tileToBounds(z + 1, cc.cx, cc.cy, clonMin, clonMax, clatMin, clatMax);
            Vec3 np; double nd2 = 0.0;
            // @Jin T3g：子块按各自 bbox 查真实高程（递归入口会再算 nodeSurfaceMargin，这里保持同口径用父级查询函数）
            const double childMargin = c.terrainQuery
                ? (c.terrainQuery->maxTerrainHeightForBounds(clonMin, clatMin, clonMax, clatMax) + SubdivCtx::kPerTileMarginM)
                : c.surfaceMarginM;
            nearestPointOfTile(c.eye, c.eyeLonDeg, c.eyeLatDeg, *c.globe,
                               clonMin, clonMax, clatMin, clatMax, np, nd2,
                               childMargin);
            cc.d2 = nd2;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            if (ch[j].d2 < ch[i].d2) std::swap(ch[i], ch[j]);
        }
    }
    for (int i = 0; i < 4; ++i) {
        subdivide(z + 1, ch[i].cx, ch[i].cy, c, out);
    }
}

} // namespace

void GeographicQuadtree::buildVisibleTiles(const TessCamera &cam, int targetLevel, int maxLevel,
                                          double tanHalfFov, int viewportHeightPx, double detailFactor,
                                          const Globe &globe, std::vector<GlobeTile> &out,
                                          double terrainCullMarginM,
                                          const ITileTerrainQuery *terrainQuery) {
    if (viewportHeightPx <= 0) { out.clear(); return; }
    if (targetLevel < 0) targetLevel = 0;
    if (maxLevel < 0) maxLevel = 0;

    // @Jin 先求离地高度，供帧间缓存按“当前尺度”判断位移；不能再用地心距 1% 的绝对阈值。
    double eyeLonDeg = 0.0;
    double eyeLatDeg = 0.0;
    double eyeAlt = 0.0;
    globe.cartesianToGeographic(cam.eye, eyeLonDeg, eyeLatDeg, eyeAlt);

    // #11 帧间一致性：相机微动时直接复用上帧结果（零递归开销）。
    // @Jin 朝向变化、离地高度缩放及明显平移都必须重建；旧逻辑在 50 km 高度允许约 64 km 位移，
    // @Jin 倾斜/缩放后仍复用旧叶集，真机表现为中心影像带之外出现大片黑区。
    if (hasPrev_) {
        if (!cameraChangedSignificantly(cam, eyeAlt, tanHalfFov, viewportHeightPx,
                                        targetLevel, maxLevel, detailFactor,
                                        terrainCullMarginM)) {
        out = prevVisible_; // 复用上帧可见叶集（含极冠外推后的经纬度边界）
            return;
        }
        // @Jin T3h（实验/可回退）：视角/参数都没变、仅地形修订号变化（DEM 注入波峰）时节流叶集重建。
        // 逐瓦片 AABB 高度变化交给网格签名驱动的 VBO 重建处理（acquireTileGlobeMesh 不受此开关影响），
        // 180ms 内复用旧叶集，把每帧全量四叉树递归削成约 5.5 次/秒，给纹理上传让出帧时间。
        // 回退：删除本 if 块即恢复“修订号一变就重建”的旧行为。
        const bool viewParamsSame = (viewportHeightPx == prevViewportH_ &&
                                     targetLevel == prevTargetLevel_ &&
                                     maxLevel == prevMaxLevel_ &&
                                     std::fabs(detailFactor - prevDetailFactor_) < 1e-9 &&
                                     std::fabs(tanHalfFov - prevTanHalfFov_) <= 1e-6);
        const bool onlyRevisionChanged = viewParamsSame &&
            std::fabs((terrainCullMarginM > 0.0 ? terrainCullMarginM : 0.0) - prevTerrainCullMarginM_) > 0.5;
        if (onlyRevisionChanged) {
            const auto nowTs = std::chrono::steady_clock::now();
            if (hasLastRevisionRebuildTime_) {
                const double sinceMs = std::chrono::duration<double, std::milli>(nowTs - lastRevisionRebuildTime_).count();
                if (sinceMs < 180.0) {
                    out = prevVisible_;
                    return;
                }
            }
            lastRevisionRebuildTime_ = nowTs;
            hasLastRevisionRebuildTime_ = true;
        } else {
            hasLastRevisionRebuildTime_ = false; // 视角变化后重新计时
        }
    }

    out.clear();
    SubdivCtx c;
    c.eye = cam.eye;
    c.planes = cam.frustumPlanes;
    // @Jin 复用上面已求得的地理坐标，避免同帧重复反算 ECEF。
    c.eyeLonDeg = eyeLonDeg;
    c.eyeLatDeg = eyeLatDeg;
    const double eyeLen = cam.eye.length();
    c.R = globe.equatorialRadius();
    // @Jin 只接受非负余量；首批 DEM 注入后，下一帧四叉树会因余量变化自动重建。
    c.surfaceMarginM = terrainCullMarginM > 0.0 ? terrainCullMarginM : 0.0;
    if (eyeLen > 1e-12) c.eyeDir = cam.eye * (1.0 / eyeLen);
    c.eyeLen = eyeLen;
    c.targetLevel = targetLevel;
    c.maxLevel = maxLevel;
    c.tanHalfFov = tanHalfFov;
    c.viewportHeight = static_cast<double>(viewportHeightPx);
    c.detailFactor = detailFactor;
    c.globe = &globe;
    c.terrainQuery = terrainQuery; // @Jin T3g：可能为空，空则走旧统一余量路径

    subdivide(0, 0, 0, c, out);

    // 缓存本帧结果供下帧复用
    prevVisible_ = out;
    prevEye_ = cam.eye;
    // @Jin 同步保存相机基，下一帧才能识别纯旋转（眼点不变但可见半球改变）。
    prevForward_ = cam.forward;
    prevUp_ = cam.up;
    prevTanHalfFov_ = tanHalfFov;
    prevViewportH_ = viewportHeightPx;
    prevTargetLevel_ = targetLevel;
    prevMaxLevel_ = maxLevel;
    prevDetailFactor_ = detailFactor;
    prevTerrainCullMarginM_ = c.surfaceMarginM;
    hasPrev_ = true;
}

bool GeographicQuadtree::cameraChangedSignificantly(const TessCamera &cam, double eyeAlt,
                                                    double tanHalfFov, int viewportH,
                                                    int targetLevel, int maxLevel,
                                                    double detailFactor,
                                                    double terrainCullMarginM) const {
    // 参数变更 → 必须全量重建
    if (viewportH != prevViewportH_ || targetLevel != prevTargetLevel_ ||
        maxLevel != prevMaxLevel_ || detailFactor != prevDetailFactor_) return true;
    // @Jin 地形从无到有或最大高程明显变化时，旧的海拔 0 可见叶集不能继续复用。
    // @Jin T3g：terrainCullMarginM 入参在新通路下承载 terrainCacheRevision_（每次 DEM 注入 +1），阈值放宽到 0.5 确保修订即可触发重建。
    if (std::fabs((terrainCullMarginM > 0.0 ? terrainCullMarginM : 0.0) -
                  prevTerrainCullMarginM_) > 0.5) {
        return true;
    }
    // FOV 变更（缩放手势会改 fov）
    if (std::fabs(tanHalfFov - prevTanHalfFov_) > 1e-6) return true;

    // @Jin 相机基一致性：heading/tilt 变化时眼点可能仍在旧阈值内，但视锥覆盖的球面已经改变。
    // @Jin 0.1° 足以吸收浮点噪声，同时保证正常双指俯仰/旋转每一小步都能及时更新可见叶集。
    // @Jin T3c（实验/可回退）：0.1°→0.35°，单指平移中的微小朝向变化不再每帧重建叶集，缓解高 zoom 拖动顿挫/阻力感。回退：改回 0.1。
    constexpr double kMaxOrientationDeltaRad = 0.35 * kDeg2Rad;
    const double minDirectionDot = std::cos(kMaxOrientationDeltaRad);
    const Vec3 forward = cam.forward.normalized();
    const Vec3 up = cam.up.normalized();
    const Vec3 prevForward = prevForward_.normalized();
    const Vec3 prevUp = prevUp_.normalized();
    if (forward.lengthSquared() < 0.5 || up.lengthSquared() < 0.5 ||
        prevForward.lengthSquared() < 0.5 || prevUp.lengthSquared() < 0.5) {
        return true;
    }
    if (forward.dot(prevForward) < minDirectionDot || up.dot(prevUp) < minDirectionDot) {
        return true;
    }

    // @Jin 位移阈值按离地高度缩放（0.25%，下限 20 m）：50 km 时约 125 m。
    // @Jin 这避免高空仍沿用数十公里前的叶集，同时静止/极小抖动继续命中缓存。
    // @Jin T3c（实验/可回退）：位移阈值 0.25%/20m → 0.5%/40m，交互中复用叶集的窗口翻倍。回退：改回 0.0025 与 20.0。
    const double altitude = std::max(eyeAlt, 1.0);
    const double threshold = std::max(altitude * 0.005, 40.0);
    const Vec3 d = cam.eye - prevEye_;
    return d.length() > threshold;
}

void GeographicQuadtree::clear() {
    hasPrev_ = false;
    prevVisible_.clear();
    prevEye_ = {};
    // @Jin 清空缓存时同步清相机基，避免上下文重建后的首帧误用旧朝向。
    prevForward_ = {};
    prevUp_ = {};
}

} // namespace globecore
