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

    // #11 帧间一致性：相机微动时直接复用上帧结果（零递归开销）。
    // 阈值取眼点位移 < 1% 眼高（手势拖拽/惯性滑行的典型微动量级）。
    // 显著移动 / 参数变更 / 首帧 → 全量重建。
    if (hasPrev_ && !cameraChangedSignificantly(cam.eye, tanHalfFov, viewportHeightPx,
                                                 targetLevel, maxLevel, detailFactor)) {
        out = prevVisible_; // 复用上帧可见叶集（含极冠外推后的经纬度边界）
        return;
    }

    out.clear();
    SubdivCtx c;
    c.eye = cam.eye;
    c.planes = cam.frustumPlanes;
    double eyeAlt = 0.0;
    globe.cartesianToGeographic(cam.eye, c.eyeLonDeg, c.eyeLatDeg, eyeAlt);
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
    prevTanHalfFov_ = tanHalfFov;
    prevViewportH_ = viewportHeightPx;
    prevTargetLevel_ = targetLevel;
    prevMaxLevel_ = maxLevel;
    prevDetailFactor_ = detailFactor;
    hasPrev_ = true;
}

bool GeographicQuadtree::cameraChangedSignificantly(const Vec3 &eye, double tanHalfFov, int viewportH,
                                                    int targetLevel, int maxLevel, double detailFactor) const {
    // 参数变更 → 必须全量重建
    if (viewportH != prevViewportH_ || targetLevel != prevTargetLevel_ ||
        maxLevel != prevMaxLevel_ || detailFactor != prevDetailFactor_) return true;
    // FOV 变更（缩放手势会改 fov）
    if (std::fabs(tanHalfFov - prevTanHalfFov_) > 1e-6) return true;
    // 眼点位移：取眼高 1% 或至少 100m 作阈值（近地手势微动 < 此值 → 复用上帧叶集）
    const double eyeLen = eye.length();
    const double prevLen = prevEye_.length();
    const double threshold = std::max(eyeLen * 0.01, 100.0);
    const Vec3 d = eye - prevEye_;
    return d.length() > threshold;
}

void GeographicQuadtree::clear() {
    hasPrev_ = false;
    prevVisible_.clear();
    prevEye_ = {};
}

} // namespace globecore
