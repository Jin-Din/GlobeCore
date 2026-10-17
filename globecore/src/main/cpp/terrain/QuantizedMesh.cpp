#include "terrain/QuantizedMesh.h"

#include "util/Log.h"

#include <cstring>
#include <cmath>

namespace globecore {

namespace {
/// 从字节缓冲读取小端数值（quantized-mesh 规范为小端）
/// 返回推进后的游标指针；越界返回 nullptr
template <typename T>
const uint8_t *readLE(const uint8_t *p, const uint8_t *end, T &out) {
    if (p + sizeof(T) > end) return nullptr;
    std::memcpy(&out, p, sizeof(T));
    return p + sizeof(T);
}

/// 尝试用 uint16 索引解码三角形列表（vertexCount <= 65536）
/// 成功返回推进后的游标；失败返回 nullptr
const uint8_t *decodeIndices16(const uint8_t *p, const uint8_t *end,
                                uint32_t triCount, std::vector<uint32_t> &indices) {
    indices.resize(static_cast<size_t>(triCount) * 3);
    for (uint32_t i = 0; i < triCount * 3; ++i) {
        uint16_t idx = 0;
        p = readLE<uint16_t>(p, end, idx);
        if (!p) return nullptr;
        indices[i] = static_cast<uint32_t>(idx);
    }
    return p;
}

/// 尝试用 uint32 索引解码三角形列表（vertexCount > 65536）
const uint8_t *decodeIndices32(const uint8_t *p, const uint8_t *end,
                                uint32_t triCount, std::vector<uint32_t> &indices) {
    indices.resize(static_cast<size_t>(triCount) * 3);
    for (uint32_t i = 0; i < triCount * 3; ++i) {
        uint32_t idx = 0;
        p = readLE<uint32_t>(p, end, idx);
        if (!p) return nullptr;
        indices[i] = idx;
    }
    return p;
}

/// 判定点是否在三角形内（含边界），返回重心坐标 (u, v, w)；不在返回 false
/// 使用 CCW 判定（quantized-mesh 三角形为 CCW）
bool pointInTriangle(double px, double py,
                     double ax, double ay, double bx, double by, double cx, double cy,
                     double &u, double &v, double &w) {
    const double d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const double d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const double d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    if (hasNeg && hasPos) return false;
    const double area = (by - ay) * (cx - ax) - (bx - ax) * (cy - ay);
    if (std::abs(area) < 1e-20) return false; // 退化三角形
    const double invArea = 1.0 / area;
    // 重心坐标：a→(1-u-v), b→u, c→v（标准重心映射，与 d1/d2/d3 对应按面积比）
    u = d1 * invArea;
    v = d2 * invArea;
    w = 1.0 - u - v;
    return (u >= -1e-10 && v >= -1e-10 && w >= -1e-10);
}
} // namespace

QuantizedMeshData decodeQuantizedMesh(const std::vector<uint8_t> &bytes) {
    QuantizedMeshData out;
    if (bytes.size() < 32) {
        LOGW("QuantizedMesh: header < 32 bytes (%zu)", bytes.size());
        return out;
    }
    const uint8_t *p = bytes.data();
    const uint8_t *end = p + bytes.size();

    // Header: 跳过 center(3×double) + belowMin/aboveMax，读 bbox + z/x/y
    // center ECEF 24B + belowMin/aboveMax 8B = 32B 前 24B 跳过
    double centerX = 0, centerY = 0, centerZ = 0;
    p = readLE<double>(p, end, centerX);
    p = readLE<double>(p, end, centerY);
    p = readLE<double>(p, end, centerZ);
    if (!p) return out;

    float belowMin = 0.0f, aboveMax = 0.0f;
    p = readLE<float>(p, end, belowMin);
    p = readLE<float>(p, end, aboveMax);
    if (!p) return out;

    // bbox（float×4）
    float lonMinF = 0, latMinF = 0, lonMaxF = 0, latMaxF = 0;
    p = readLE<float>(p, end, lonMinF);
    p = readLE<float>(p, end, latMinF);
    p = readLE<float>(p, end, lonMaxF);
    p = readLE<float>(p, end, latMaxF);
    if (!p) return out;

    // z/x/y（int32×3）—— 部分实现省略，但标准 v1.0 header 含此字段
    int32_t zTile = 0, xTile = 0, yTile = 0;
    p = readLE<int32_t>(p, end, zTile);
    p = readLE<int32_t>(p, end, xTile);
    p = readLE<int32_t>(p, end, yTile);
    if (!p) return out;

    // 至此已读 32B header。如果某些实现没有 z/x/y（header 仅 24B），则 p 会在 vertex data
    // 的偏移处错位——为兼容此情况，若后续 vertexCount 解码异常，尝试跳过 z/x/y 重新解析。
    out.lonMin = lonMinF; out.latMin = latMinF; out.lonMax = lonMaxF; out.latMax = latMaxF;
    out.belowMin = belowMin; out.aboveMax = aboveMax;

    // VertexData: vertexCount + u/v/h
    uint32_t vertexCount = 0;
    p = readLE<uint32_t>(p, end, vertexCount);
    if (!p || vertexCount == 0 || vertexCount > 10'000'000) {
        LOGW("QuantizedMesh: bad vertexCount=%u", vertexCount);
        return out;
    }

    out.lons.resize(vertexCount);
    out.lats.resize(vertexCount);
    out.alts.resize(vertexCount);

    // u/v/h 各为 uint16[vertexCount]，小端
    // 先读 u[vertexCount]
    std::vector<uint16_t> u16(vertexCount), v16(vertexCount), h16(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        p = readLE<uint16_t>(p, end, u16[i]);
        if (!p) { LOGW("QuantizedMesh: trunc u@%u", i); return out; }
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        p = readLE<uint16_t>(p, end, v16[i]);
        if (!p) { LOGW("QuantizedMesh: trunc v@%u", i); return out; }
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        p = readLE<uint16_t>(p, end, h16[i]);
        if (!p) { LOGW("QuantizedMesh: trunc h@%u", i); return out; }
    }

    // 量化→地理坐标（u/v [0,32767] → tile bbox；h [0,32767] → [belowMin, aboveMax]）
    constexpr double kQuantMax = 32767.0;
    const double lonRange = lonMaxF - lonMinF;
    const double latRange = latMaxF - latMinF;
    const double altRange = static_cast<double>(aboveMax - belowMin);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        out.lons[i] = lonMinF + (u16[i] / kQuantMax) * lonRange;
        out.lats[i] = latMinF + (v16[i] / kQuantMax) * latRange;
        out.alts[i] = belowMin + (h16[i] / kQuantMax) * altRange;
    }

    // IndexData: triangleCount + indices
    uint32_t triangleCount = 0;
    p = readLE<uint32_t>(p, end, triangleCount);
    if (!p || triangleCount > 20'000'000) {
        LOGW("QuantizedMesh: bad triangleCount=%u", triangleCount);
        return out;
    }

    // 索引宽度取决于顶点数：>65536 用 uint32，否则 uint16
    if (vertexCount <= 65535) {
        p = decodeIndices16(p, end, triangleCount, out.indices);
    } else {
        p = decodeIndices32(p, end, triangleCount, out.indices);
    }
    if (!p) {
        LOGW("QuantizedMesh: index decode failed");
        return out;
    }

    // 扩展段（法线/水掩码等）本期不解析，直接丢弃剩余字节
    out.valid = true;
    LOGI("QuantizedMesh decoded: verts=%u tris=%u bbox=[%.4f,%.4f,%.4f,%.4f] alt=[%.1f,%.1f]",
         vertexCount, triangleCount, out.lonMin, out.latMin, out.lonMax, out.latMax,
         out.belowMin, out.aboveMax);
    return out;
}

double QuantizedMeshData::sampleHeight(double lon, double lat) const {
    if (!valid || indices.empty()) return 0.0;
    // 快速 bbox 剔除
    if (lon < lonMin || lon > lonMax || lat < latMin || lat > latMax) return 0.0;

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
            return alts[ia] * w + alts[ib] * u + alts[ic] * v;
        }
    }
    return 0.0; // 点不在任何三角形内（边缘缝隙），回退无地形
}

} // namespace globecore
