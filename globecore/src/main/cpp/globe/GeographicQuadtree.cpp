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
                               Vec3 &outNearest, double &outDist2) {
    const double nLat = clampd(eyeLatDeg, latMin, latMax);
    double nLon = eyeLonDeg;
    if (nLon < lonMin || nLon > lonMax) {
        const double dMin = std::fabs(wrapLonDelta(eyeLonDeg - lonMin));
        const double dMax = std::fabs(wrapLonDelta(eyeLonDeg - lonMax));
        nLon = (dMin <= dMax) ? lonMin : lonMax;
    }
    globe.geographicToCartesian(nLon, nLat, 0.0, outNearest);
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
    const Globe *globe = nullptr;
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
    nearestPointOfTile(c.eye, c.eyeLonDeg, c.eyeLatDeg, *c.globe,
                      lonMin, lonMax, latMin, latMax, nearest, nearestDist2);

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
        testHorizon(nearest);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                c.globe->geographicToCartesian(lonArr[i], latArr[j], 0.0, p);
                if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
                if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
                testHorizon(p);
            }
        }
        if ((lonMax - lonMin) > 180.0) {
            const double latC = 0.5 * (latMin + latMax);
            const double lonC = 0.5 * (lonMin + lonMax);
            c.globe->geographicToCartesian(lonC + 90.0, latC, 0.0, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            testHorizon(p);
            c.globe->geographicToCartesian(lonC - 90.0, latC, 0.0, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            testHorizon(p);
        }
        // 极冠补点（外推后 latMax/latMin 可能已为 90/-90，采样点自然覆盖极冠）
        if (isNorthPolar) {
            c.globe->geographicToCartesian(0.0, 90.0, 0.0, p);
            testHorizon(p);
        }
        if (isSouthPolar) {
            c.globe->geographicToCartesian(0.0, -90.0, 0.0, p);
            testHorizon(p);
        }
    }

    if (c.eyeLen > 0.0 && !horizonVisible) return;
    for (int pi = 0; pi < 6; ++pi) {
        const FrustumPlane &pl = c.planes[pi];
        const double px = (pl.a >= 0.0) ? xmax : xmin;
        const double py = (pl.b >= 0.0) ? ymax : ymin;
        const double pz = (pl.c >= 0.0) ? zmax : zmin;
        const double v = pl.a * px + pl.b * py + pl.c * pz + pl.d;
        if (v < 0.0) return;
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
            nearestPointOfTile(c.eye, c.eyeLonDeg, c.eyeLatDeg, *c.globe,
                               clonMin, clonMax, clatMin, clatMax, np, nd2);
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
                                          const Globe &globe, std::vector<GlobeTile> &out) {
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
    if (hasPrev_ && !cameraChangedSignificantly(cam, eyeAlt, tanHalfFov, viewportHeightPx,
                                                 targetLevel, maxLevel, detailFactor)) {
        out = prevVisible_; // 复用上帧可见叶集（含极冠外推后的经纬度边界）
        return;
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
    if (eyeLen > 1e-12) c.eyeDir = cam.eye * (1.0 / eyeLen);
    c.eyeLen = eyeLen;
    c.targetLevel = targetLevel;
    c.maxLevel = maxLevel;
    c.tanHalfFov = tanHalfFov;
    c.viewportHeight = static_cast<double>(viewportHeightPx);
    c.detailFactor = detailFactor;
    c.globe = &globe;

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
    hasPrev_ = true;
}

bool GeographicQuadtree::cameraChangedSignificantly(const TessCamera &cam, double eyeAlt,
                                                    double tanHalfFov, int viewportH,
                                                    int targetLevel, int maxLevel,
                                                    double detailFactor) const {
    // 参数变更 → 必须全量重建
    if (viewportH != prevViewportH_ || targetLevel != prevTargetLevel_ ||
        maxLevel != prevMaxLevel_ || detailFactor != prevDetailFactor_) return true;
    // FOV 变更（缩放手势会改 fov）
    if (std::fabs(tanHalfFov - prevTanHalfFov_) > 1e-6) return true;

    // @Jin 相机基一致性：heading/tilt 变化时眼点可能仍在旧阈值内，但视锥覆盖的球面已经改变。
    // @Jin 0.1° 足以吸收浮点噪声，同时保证正常双指俯仰/旋转每一小步都能及时更新可见叶集。
    constexpr double kMaxOrientationDeltaRad = 0.1 * kDeg2Rad;
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
    const double altitude = std::max(eyeAlt, 1.0);
    const double threshold = std::max(altitude * 0.0025, 20.0);
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
