#include "terrain/QuantizedMesh.h"

#include "util/Log.h"

#include <cstring>
#include <cmath>
#include <limits>

namespace globecore {

namespace {
/// 标准 quantized-mesh v1.0 header 字节数（顶点数据自此之后开始）。
/// 实测：多个生产服务的 vertexCount 位于 offset 88（而非规范文本里的 84），
/// 即 horizonOcclusionPoint 之后还有 4 字节保留/补齐。此处以实测为准。
constexpr size_t kQuantizedMeshHeaderBytes = 88;
/// header 内 minimumHeight/maximumHeight 的偏移
constexpr size_t kOffsetMinHeight = 24;
/// 量化值域上限（u/v/h 均为 [0, 32767]）
constexpr double kQuantMax = 32767.0;

/// 从字节缓冲读取小端数值（quantized-mesh 规范为小端）
/// 返回推进后的游标指针；越界返回 nullptr
template <typename T>
const uint8_t *readLE(const uint8_t *p, const uint8_t *end, T &out) {
    if (p + sizeof(T) > end) return nullptr;
    std::memcpy(&out, p, sizeof(T));
    return p + sizeof(T);
}

/// zigzag 解码：(n >> 1) ^ -(n & 1)，把无符号交错编码还原为有符号增量
inline int32_t zigzagDecode(uint16_t n) {
    const uint32_t v = static_cast<uint32_t>(n);
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1u) + 1u));
}

/// 就地把「zigzag 编码的增量序列」还原为绝对值序列（见 decodeQuantizedMesh 中的规范说明）
inline void decodeZigzagDeltaInPlace(std::vector<uint16_t> &v) {
    int32_t acc = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        acc += zigzagDecode(v[i]);
        v[i] = static_cast<uint16_t>(acc);
    }
}

/// 尝试用 uint16 索引解码三角形列表（vertexCount <= 65536）
/// 成功返回推进后的游标；失败返回 nullptr
///
/// @Jin 【地形贴合度根因修复】quantized-mesh 规范的三角形索引码流不是「原始索引值」，
///       而是 high-water mark（高水位）增量编码：维护初值为 0 的 highest，按码流顺序
///       逐个读码 code，真实索引 = highest - code；每当 code == 0 时 highest 自增 1。
///       旧实现直接把码流 memcpy 当索引用，实测会出现 max==vertexCount 的越界值，
///       三角网顶点配对整体错乱，buildHeightGrid 基于错三角网栅格化 → 地形形状与实际不符。
///       参考：Cesium quantized-mesh v1.0 规范 / QGIS QgsQuantizedMeshDecoder；
///       已用 build/qmt/decode_test.py 对真实瓦片离线验证（解码后全部落在 [0,vertexCount)）。
///       注：规范还有奇偶三角形缠绕序交换（交换同一三角形的第 2、3 索引），只影响正反面
///       朝向、不改变三角形覆盖范围；本引擎只用三角网做高度栅格采样，故无需还原缠绕序。
const uint8_t *decodeIndices16(const uint8_t *p, const uint8_t *end,
                                uint32_t triCount, std::vector<uint32_t> &indices) {
    const size_t codeCount = static_cast<size_t>(triCount) * 3;
    indices.resize(codeCount);
    uint32_t highest = 0; // @Jin 高水位：已引入的最高索引 +1，每遇到 code==0 自增
    for (size_t i = 0; i < codeCount; ++i) {
        uint16_t code = 0;
        p = readLE<uint16_t>(p, end, code);
        if (!p) return nullptr;
        const uint32_t code32 = static_cast<uint32_t>(code);
        indices[i] = highest - code32; // @Jin 真实索引 = 高水位 - 码值
        if (code32 == 0) ++highest;   // @Jin 码值 0 表示此处引入一个新顶点，水位抬升
    }
    return p;
}

/// 尝试用 uint32 索引解码三角形列表（vertexCount > 65536）
///
/// @Jin 同 decodeIndices16：32 位码流同样是 high-water mark 编码（仅位宽不同），
///       必须按 真实索引 = highest - code、code==0 时 highest++ 还原，不能直接取原值。
const uint8_t *decodeIndices32(const uint8_t *p, const uint8_t *end,
                                uint32_t triCount, std::vector<uint32_t> &indices) {
    const size_t codeCount = static_cast<size_t>(triCount) * 3;
    indices.resize(codeCount);
    uint32_t highest = 0; // @Jin 高水位：已引入的最高索引 +1，每遇到 code==0 自增
    for (size_t i = 0; i < codeCount; ++i) {
        uint32_t code = 0;
        p = readLE<uint32_t>(p, end, code);
        if (!p) return nullptr;
        indices[i] = highest - code; // @Jin 真实索引 = 高水位 - 码值
        if (code == 0) ++highest;    // @Jin 码值 0 表示此处引入一个新顶点，水位抬升
    }
    return p;
}

/// 跳过一段「uint32 count + count 个索引」的边索引（west/south/east/north）。
/// @param wide 索引是否为 uint32（vertexCount > 65536 时）
/// @return 推进后的游标；越界返回 nullptr
const uint8_t *skipEdgeIndices(const uint8_t *p, const uint8_t *end, bool wide, uint32_t &outCount) {
    uint32_t count = 0;
    p = readLE<uint32_t>(p, end, count);
    if (!p) return nullptr;
    outCount = count;
    const size_t bytes = static_cast<size_t>(count) * (wide ? 4u : 2u);
    if (p + bytes > end) return nullptr;
    return p + bytes;
}

/// 跳过扩展段循环：uint8 id + uint32 length + length 字节。
/// 扩展段位于瓦片最末（oct 法线 / 水掩码 / 元数据），本解码器不需要其内容。
/// @param outExtCount 输出：扩展段数量
/// @return 推进后的游标；结构异常返回 nullptr
const uint8_t *skipExtensions(const uint8_t *p, const uint8_t *end, uint32_t &outExtCount) {
    uint32_t n = 0;
    while (p < end) {
        // 剩余不足一个扩展头
        if (p + 5 > end) return nullptr;
        const uint8_t extId = *p;
        p += 1;
        uint32_t len = 0;
        p = readLE<uint32_t>(p, end, len);
        if (!p) return nullptr;
        if (p + len > end) return nullptr;
        p += len;
        ++n;
        (void)extId; // 扩展 id 仅用于将来按需解析（1=法线 2=水掩码 4=元数据）
    }
    outExtCount = n;
    return p;
}

/// 判定点是否在三角形内（含边界），返回重心坐标；不在返回 false。
///   u → 顶点 b 的权重，v → 顶点 c 的权重，w → 顶点 a 的权重（u+v+w ≡ 1）
///
/// 【feature/terrain-pre 修正】原实现的重心坐标是错的，导致【任何点都判为不在三角形内】：
///   ① area 取成了 cross(B-A,C-A) 的相反数 → 三个权重整体变号；
///   ② d1/d2/d3 与顶点的对应关系错位：d1 是子三角形 (B,A,P) 的面积比 → 对顶点 C，
///      d2 对应顶点 A，d3 对应顶点 B（原实现按 d1→b、d2→c 映射）。
/// 推导（以 A(0,0) B(1,0) C(0,1) P(0.5,0.25) 校验：正确重心为 a=.25 b=.5 c=.25）：
///   d1 = cross(P-B, A-B) = .25 → w_c ✓ ；d2 = cross(P-C, B-C) = .25 → w_a ✓
///   d3 = cross(P-A, C-A) = .50 → w_b ✓ ；分母 = cross(B-A, C-A) = 1
/// 修正后：u = d3/area, v = d1/area, w = d2/area。
bool pointInTriangle(double px, double py,
                     double ax, double ay, double bx, double by, double cx, double cy,
                     double &u, double &v, double &w) {
    const double d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by); // → 顶点 c 的权重
    const double d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy); // → 顶点 a 的权重
    const double d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay); // → 顶点 b 的权重
    const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    if (hasNeg && hasPos) return false;
    // 2×三角形有向面积：cross(B-A, C-A)。CCW 三角形为正，与 d1/d2/d3 同号。
    const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::abs(area) < 1e-20) return false; // 退化三角形
    const double invArea = 1.0 / area;
    u = d3 * invArea;
    v = d1 * invArea;
    w = d2 * invArea;
    return (u >= -1e-10 && v >= -1e-10 && w >= -1e-10);
}
} // namespace

QuantizedMeshData decodeQuantizedMesh(const std::vector<uint8_t> &bytes,
                                      double lonMin, double latMin,
                                      double lonMax, double latMax) {
    QuantizedMeshData out;
    out.lonMin = lonMin;
    out.latMin = latMin;
    out.lonMax = lonMax;
    out.latMax = latMax;

    if (bytes.size() < kQuantizedMeshHeaderBytes + 4) {
        LOGW("QuantizedMesh: too small (%zu bytes)", bytes.size());
        return out;
    }

    const uint8_t *base = bytes.data();
    const uint8_t *end = base + bytes.size();

    // ---- header：高度值域（offset 24..31）----
    float belowMin = 0.0f, aboveMax = 0.0f;
    const uint8_t *h = base + kOffsetMinHeight;
    h = readLE<float>(h, end, belowMin);
    h = readLE<float>(h, end, aboveMax);
    if (!h) {
        LOGW("QuantizedMesh: truncated height range");
        return out;
    }
    out.belowMin = belowMin;
    out.aboveMax = aboveMax;

    // ---- 顶点数据：vertexCount + u/v/h 三段平面数组 ----
    const uint8_t *p = base + kQuantizedMeshHeaderBytes;
    uint32_t vertexCount = 0;
    p = readLE<uint32_t>(p, end, vertexCount);
    if (!p || vertexCount == 0 || vertexCount > 10'000'000u) {
        LOGW("QuantizedMesh: bad vertexCount=%u", vertexCount);
        return out;
    }

    const bool wideIndex = (vertexCount > 65536u);
    const size_t planeBytes = static_cast<size_t>(vertexCount) * 2u;
    if (static_cast<size_t>(end - p) < planeBytes * 3u) {
        LOGW("QuantizedMesh: vertex data truncated (need %zu, left %td)",
             planeBytes * 3u, end - p);
        return out;
    }

    // ⚠️ u/v/h 三段是「zigzag 编码的增量」而非绝对值（quantized-mesh v1.0 规范，feature/terrain-pre 修正）：
    //    实际值[i] = 实际值[i-1] + zigzagDecode(raw[i])，首元素即以 0 为前值的累加。
    //    判据：原始 uint16 会出现 > 32767 的值（实测本服务 u 段出现 65534），
    //    而量化值域恒为 [0, 32767]；按增量累加后 min=0 / max=32767 精确成立。
    //    漏掉这一步的后果是顶点坐标整体错乱 → 三角网不覆盖瓦片 → 采样恒落到兜底值。
    std::vector<uint16_t> u16(vertexCount), v16(vertexCount), h16(vertexCount);
    std::memcpy(u16.data(), p, planeBytes); p += planeBytes;
    std::memcpy(v16.data(), p, planeBytes); p += planeBytes;
    std::memcpy(h16.data(), p, planeBytes); p += planeBytes;
    decodeZigzagDeltaInPlace(u16);
    decodeZigzagDeltaInPlace(v16);
    decodeZigzagDeltaInPlace(h16);

    // ---- 索引数据 ----
    uint32_t triangleCount = 0;
    p = readLE<uint32_t>(p, end, triangleCount);
    if (!p || triangleCount > 20'000'000u) {
        LOGW("QuantizedMesh: bad triangleCount=%u", triangleCount);
        return out;
    }
    if (wideIndex) {
        p = decodeIndices32(p, end, triangleCount, out.indices);
    } else {
        p = decodeIndices16(p, end, triangleCount, out.indices);
    }
    if (!p) {
        LOGW("QuantizedMesh: index decode failed");
        return out;
    }

    // ---- 边索引（west/south/east/north）：标准段，旧实现缺失导致尾部错位 ----
    uint32_t edgeCounts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        p = skipEdgeIndices(p, end, wideIndex, edgeCounts[i]);
        if (!p) {
            LOGW("QuantizedMesh: edge indices #%d truncated", i);
            return out;
        }
    }

    // ---- 扩展段（法线/水掩码/元数据）：本解码器只做跳过 ----
    uint32_t extCount = 0;
    if (!skipExtensions(p, end, extCount)) {
        LOGW("QuantizedMesh: extensions truncated");
        return out;
    }

    // ---- 量化 → 地理坐标 ----
    const double lonRange = lonMax - lonMin;
    const double latRange = latMax - latMin;
    const double altRange = static_cast<double>(aboveMax - belowMin);
    out.lons.resize(vertexCount);
    out.lats.resize(vertexCount);
    out.alts.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        // 少数生产瓦片存在裙边哨兵值（如 u=0xFFFE），钳到值域上限避免顶点飞到瓦片之外
        const double uu = (u16[i] > 32767 ? kQuantMax : static_cast<double>(u16[i]));
        const double vv = (v16[i] > 32767 ? kQuantMax : static_cast<double>(v16[i]));
        const double hh = (h16[i] > 32767 ? kQuantMax : static_cast<double>(h16[i]));
        out.lons[i] = lonMin + (uu / kQuantMax) * lonRange;
        out.lats[i] = latMin + (vv / kQuantMax) * latRange;   // v=0 → 南边
        out.alts[i] = belowMin + (hh / kQuantMax) * altRange;
    }

    out.valid = true;
    // 解码即栅格化：采样退化为 O(1)，避免每帧逐顶点扫三角形（见 QuantizedMesh.h 注释）
    out.buildHeightGrid();
    LOGI("QuantizedMesh: verts=%u tris=%u edges=%u/%u/%u/%u ext=%u bbox=[%.4f,%.4f,%.4f,%.4f] alt=[%.1f,%.1f] grid=%d",
         vertexCount, triangleCount, edgeCounts[0], edgeCounts[1], edgeCounts[2], edgeCounts[3],
         extCount, out.lonMin, out.latMin, out.lonMax, out.latMax, out.belowMin, out.aboveMax,
         out.gridReady ? out.gridN : 0);
    return out;
}

namespace {
/// 在三角形集合中求 (lon,lat) 所在三角形的重心插值高度；未命中返回 false。
/// 【仅栅格不可用时的回退路径】——每帧逐顶点走这条路的代价是 O(triangleCount)，不可接受。
inline bool sampleTriangle(const std::vector<double> &lons, const std::vector<double> &lats,
                           const std::vector<double> &alts, const std::vector<uint32_t> &indices,
                           double lon, double lat, double &outAlt) {
    const uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);
    for (uint32_t t = 0; t < triCount; ++t) {
        const uint32_t ia = indices[t * 3 + 0];
        const uint32_t ib = indices[t * 3 + 1];
        const uint32_t ic = indices[t * 3 + 2];
        if (ia >= lons.size() || ib >= lons.size() || ic >= lons.size()) continue;
        double u = 0, v = 0, w = 0;
        if (pointInTriangle(lon, lat,
                            lons[ia], lats[ia], lons[ib], lats[ib], lons[ic], lats[ic],
                            u, v, w)) {
            outAlt = alts[ia] * w + alts[ib] * u + alts[ic] * v;
            return true;
        }
    }
    return false;
}
} // namespace

void QuantizedMeshData::buildHeightGrid(int n) {
    gridReady = false;
    gridN = n < 4 ? 4 : n;
    const int side = gridN + 1;
    grid.assign(static_cast<size_t>(side) * static_cast<size_t>(side),
                std::numeric_limits<float>::quiet_NaN());

    const double lonRange = lonMax - lonMin;
    const double latRange = latMax - latMin;
    if (!valid || indices.size() < 3 || !(lonRange > 0.0) || !(latRange > 0.0)) {
        grid.clear();
        return;
    }
    const uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);
    const size_t nVerts = lons.size();

    // 把每个三角形栅格化到节点表：只遍历三角形包围盒覆盖的节点，
    // 单块总判定量 ≈ 三角形数 × 单三角覆盖节点数（实测 ~8 千次），远低于「逐节点扫全部三角形」。
    for (uint32_t t = 0; t < triCount; ++t) {
        const uint32_t ia = indices[t * 3 + 0];
        const uint32_t ib = indices[t * 3 + 1];
        const uint32_t ic = indices[t * 3 + 2];
        if (ia >= nVerts || ib >= nVerts || ic >= nVerts) continue;
        // 归一化到 [0,1]²（u=0 西边，v=0 南边）
        const double ua = (lons[ia] - lonMin) / lonRange, va = (lats[ia] - latMin) / latRange;
        const double ub = (lons[ib] - lonMin) / lonRange, vb = (lats[ib] - latMin) / latRange;
        const double uc = (lons[ic] - lonMin) / lonRange, vc = (lats[ic] - latMin) / latRange;
        // 外扩一格避免边界漏格（三角形顶点恰在格子线上时四舍五入会漏）
        int i0 = static_cast<int>(std::floor(std::min(ua, std::min(ub, uc)) * gridN)) - 1;
        int i1 = static_cast<int>(std::ceil(std::max(ua, std::max(ub, uc)) * gridN)) + 1;
        int j0 = static_cast<int>(std::floor(std::min(va, std::min(vb, vc)) * gridN)) - 1;
        int j1 = static_cast<int>(std::ceil(std::max(va, std::max(vb, vc)) * gridN)) + 1;
        if (i0 < 0) i0 = 0; if (i1 > gridN) i1 = gridN;
        if (j0 < 0) j0 = 0; if (j1 > gridN) j1 = gridN;
        for (int j = j0; j <= j1; ++j) {
            const double pv = static_cast<double>(j) / gridN;
            for (int i = i0; i <= i1; ++i) {
                const size_t idx = static_cast<size_t>(j) * side + static_cast<size_t>(i);
                if (!std::isnan(grid[idx])) continue; // 先到先得：重叠处任取一个已足够
                const double pu = static_cast<double>(i) / gridN;
                double u = 0, v = 0, w = 0;
                if (pointInTriangle(pu, pv, ua, va, ub, vb, uc, vc, u, v, w)) {
                    grid[idx] = static_cast<float>(alts[ia] * w + alts[ib] * u + alts[ic] * v);
                }
            }
        }
    }

    // 未被任何三角形覆盖的节点（瓦片边缘/退化三角）：从已填邻居迭代扩散补齐。
    // 两向扫描各一遍即可覆盖连续空洞；极端情况再兜底为 header 的 minimumHeight。
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 2 * side) {
        changed = false;
        for (int j = 0; j <= gridN; ++j) {
            for (int i = 0; i <= gridN; ++i) {
                const size_t idx = static_cast<size_t>(j) * side + static_cast<size_t>(i);
                if (!std::isnan(grid[idx])) continue;
                float acc = 0.0f; int cnt = 0;
                if (i > 0 && !std::isnan(grid[idx - 1])) { acc += grid[idx - 1]; ++cnt; }
                if (i < gridN && !std::isnan(grid[idx + 1])) { acc += grid[idx + 1]; ++cnt; }
                if (j > 0 && !std::isnan(grid[idx - side])) { acc += grid[idx - side]; ++cnt; }
                if (j < gridN && !std::isnan(grid[idx + side])) { acc += grid[idx + side]; ++cnt; }
                if (cnt > 0) { grid[idx] = acc / cnt; changed = true; }
            }
        }
    }
    for (size_t k = 0; k < grid.size(); ++k) {
        if (std::isnan(grid[k])) grid[k] = belowMin;
    }
    gridReady = true;
}

double QuantizedMeshData::sampleHeight(double lon, double lat) const {
    if (!valid || indices.empty()) return 0.0;

    // ── 主路径：栅格双线性插值，O(1) ──
    if (gridReady) {
        const double lonRange = lonMax - lonMin;
        const double latRange = latMax - latMin;
        if (lonRange <= 0.0 || latRange <= 0.0) return 0.0;
        // 四至外先钳制到边界：等价于沿边界做常数外推。
        // 必要性：① Renderer::buildGlobeTileMesh 把网格外扩 0.75% 盖邻瓦片缝隙，采样必越界；
        // ② 服务网格与引擎瓦片不重合时，引擎瓦片只有中心区域落在本 DEM 内。
        // 若不钳制直接回 0，这些区域会整片塌陷成海平面，视觉上是明显「台阶／空洞」。
        double u = (lon - lonMin) / lonRange;
        double v = (lat - latMin) / latRange;
        if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
        if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
        const double fx = u * gridN, fy = v * gridN;
        int i0 = static_cast<int>(std::floor(fx));
        int j0 = static_cast<int>(std::floor(fy));
        if (i0 < 0) i0 = 0; if (j0 < 0) j0 = 0;
        if (i0 > gridN - 1) i0 = gridN - 1;
        if (j0 > gridN - 1) j0 = gridN - 1;
        const int side = gridN + 1;
        const double tx = fx - i0, ty = fy - j0;
        const size_t base = static_cast<size_t>(j0) * side + static_cast<size_t>(i0);
        const double h00 = grid[base];
        const double h10 = grid[base + 1];
        const double h01 = grid[base + side];
        const double h11 = grid[base + side + 1];
        const double h0 = h00 + (h10 - h00) * tx;
        const double h1 = h01 + (h11 - h01) * tx;
        return h0 + (h1 - h0) * ty;
    }

    // ── 回退：逐三角形扫描（栅格未建成，仅在异常数据下发生）──
    double alt = 0.0;
    if (sampleTriangle(lons, lats, alts, indices, lon, lat, alt)) return alt;
    double cl = lon, ct = lat;
    const double eLon = (lonMax - lonMin) * 1e-6;
    const double eLat = (latMax - latMin) * 1e-6;
    if (cl < lonMin + eLon) cl = lonMin + eLon;
    if (cl > lonMax - eLon) cl = lonMax - eLon;
    if (ct < latMin + eLat) ct = latMin + eLat;
    if (ct > latMax - eLat) ct = latMax - eLat;
    if (sampleTriangle(lons, lats, alts, indices, cl, ct, alt)) return alt;
    return 0.0;
}

} // namespace globecore
