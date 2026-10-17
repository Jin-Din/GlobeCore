#include "vector/MvtDecoder.h"

#include "util/Log.h"

#include <cmath>
#include <cstring>

namespace globecore {

// ==================== 轻量 Protobuf wire-format 解码（无外部依赖） ====================
// MVT 使用 protobuf3 编码，只需处理两种 wire type：
//   0 = varint（int32/int64/uint32/uint64/bool/enum/sint32/sint64）
//   2 = length-delimited（string/bytes/embedded message/packed repeated）
// field_number = tag >> 3, wire_type = tag & 7

namespace pb {

/// 读取 varint（LEB128 编码）。返回推进后的游标；越界返回 nullptr
static const uint8_t *readVarint(const uint8_t *p, const uint8_t *end, uint64_t &out) {
    out = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return p;
        shift += 7;
        if (shift >= 64) return nullptr; // 溢出
    }
    return nullptr; // 截断
}

/// zigzag 解码 (sint32)：(n >> 1) ^ -(n & 1)
static int32_t zigzag32(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ -static_cast<int32_t>(n & 1));
}

/// 读取 length-delimited 字段（先读长度 varint，再读 bytes）。返回推进后游标 + 数据指针 + 长度
static const uint8_t *readBytes(const uint8_t *p, const uint8_t *end,
                                const uint8_t *&data, size_t &len) {
    uint64_t l = 0;
    p = readVarint(p, end, l);
    if (!p || p + l > end) return nullptr;
    data = p;
    len = static_cast<size_t>(l);
    return p + l;
}

/// 读取 tag（field_number + wire_type）
static const uint8_t *readTag(const uint8_t *p, const uint8_t *end,
                              uint32_t &fieldNumber, uint32_t &wireType) {
    uint64_t tag = 0;
    p = readVarint(p, end, tag);
    if (!p) return nullptr;
    fieldNumber = static_cast<uint32_t>(tag >> 3);
    wireType = static_cast<uint32_t>(tag & 7);
    return p;
}

/// 跳过未知字段（按 wire type 推进游标）
static const uint8_t *skipField(const uint8_t *p, const uint8_t *end, uint32_t wireType) {
    switch (wireType) {
        case 0: { uint64_t v; return readVarint(p, end, v); }
        case 2: { const uint8_t *d; size_t l; return readBytes(p, end, d, l); }
        case 1: { // fixed64
            if (p + 8 > end) return nullptr;
            return p + 8;
        }
        case 5: { // fixed32
            if (p + 4 > end) return nullptr;
            return p + 4;
        }
        default: return nullptr; // 未知 wire type（group 等已废弃）
    }
}
} // namespace pb

// ==================== MVT Value 解码 ====================

/// MVT Value 消息：oneof string/float/double/int/uint/sint/bool
struct MvtValue {
    enum class Type { None, String, Float, Double, Int, UInt, SInt, Bool };
    Type type = Type::None;
    std::string s;
    double d = 0.0;
    bool b = false;
};

/// 解码 Value 消息
static MvtValue decodeValue(const uint8_t *data, const uint8_t *end) {
    MvtValue v;
    const uint8_t *p = data;
    while (p && p < end) {
        uint32_t fn = 0, wt = 0;
        p = pb::readTag(p, end, fn, wt);
        if (!p) break;
        uint64_t varint = 0;
        switch (fn) {
            case 1: // string_value
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d) { v.type = MvtValue::Type::String; v.s.assign(reinterpret_cast<const char *>(d), l); }
                } else p = pb::skipField(p, end, wt);
                break;
            case 2: // float_value
                if (wt == 5 && p + 4 <= end) {
                    float f; std::memcpy(&f, p, 4);
                    v.type = MvtValue::Type::Float; v.d = static_cast<double>(f);
                    p += 4;
                } else p = pb::skipField(p, end, wt);
                break;
            case 3: // double_value
                if (wt == 1 && p + 8 <= end) {
                    double dd; std::memcpy(&dd, p, 8);
                    v.type = MvtValue::Type::Double; v.d = dd;
                    p += 8;
                } else p = pb::skipField(p, end, wt);
                break;
            case 4: // int_value
                if (wt == 0) { p = pb::readVarint(p, end, varint); v.type = MvtValue::Type::Int; v.d = static_cast<double>(static_cast<int64_t>(varint)); }
                else p = pb::skipField(p, end, wt);
                break;
            case 5: // uint_value
                if (wt == 0) { p = pb::readVarint(p, end, varint); v.type = MvtValue::Type::UInt; v.d = static_cast<double>(varint); }
                else p = pb::skipField(p, end, wt);
                break;
            case 6: // sint_value (zigzag)
                if (wt == 0) { p = pb::readVarint(p, end, varint); v.type = MvtValue::Type::SInt; v.d = static_cast<double>(pb::zigzag32(static_cast<uint32_t>(varint))); }
                else p = pb::skipField(p, end, wt);
                break;
            case 7: // bool_value
                if (wt == 0) { p = pb::readVarint(p, end, varint); v.type = MvtValue::Type::Bool; v.b = (varint != 0); v.d = v.b ? 1.0 : 0.0; }
                else p = pb::skipField(p, end, wt);
                break;
            default:
                p = pb::skipField(p, end, wt);
                break;
        }
    }
    return v;
}

// ==================== MVT Feature 解码 ====================

/// MVT 几何类型
enum class MvtGeomType { Unknown = 0, Point = 1, LineString = 2, Polygon = 3 };

/// 解码 Feature 的 geometry commands 为坐标序列
/// 返回 rings：外环 + 内环（POLYGON），或 parts（LINE），或 单点列表（POINT）
/// 坐标为 tile extent [0, extent) 内的整数值
static bool decodeGeometry(const std::vector<uint32_t> &cmds, uint32_t extent,
                           std::vector<std::vector<std::pair<int32_t, int32_t>>> &rings) {
    // geometry 为 packed varint，已解码为 uint32 数组
    // 每个 command 编码为 (id << 3) | count，后跟参数（zigzag int32）
    size_t i = 0;
    int32_t x = 0, y = 0; // 累积坐标
    std::vector<std::pair<int32_t, int32_t>> currentRing;

    while (i < cmds.size()) {
        uint32_t cmd = cmds[i++];
        uint32_t cmdId = cmd >> 3;
        uint32_t count = cmd & 0x7;

        if (cmdId == 1) { // MoveTo: count 应为 1，2 参数 (dx, dy)
            if (count != 1 || i + 2 > cmds.size()) return false;
            // 若有当前环且非空，保存并开始新环
            if (!currentRing.empty()) {
                rings.push_back(std::move(currentRing));
                currentRing.clear();
            }
            for (uint32_t c = 0; c < count; ++c) {
                int32_t dx = pb::zigzag32(cmds[i++]);
                int32_t dy = pb::zigzag32(cmds[i++]);
                x += dx; y += dy;
                currentRing.emplace_back(x, y);
            }
        } else if (cmdId == 2) { // LineTo: count 个点，每个 2 参数
            if (count == 0 || i + count * 2 > cmds.size()) return false;
            for (uint32_t c = 0; c < count; ++c) {
                int32_t dx = pb::zigzag32(cmds[i++]);
                int32_t dy = pb::zigzag32(cmds[i++]);
                x += dx; y += dy;
                currentRing.emplace_back(x, y);
            }
        } else if (cmdId == 7) { // ClosePath: 无参数，闭合当前环
            if (!currentRing.empty()) {
                // 闭合：首尾重合
                currentRing.push_back(currentRing.front());
                rings.push_back(std::move(currentRing));
                currentRing.clear();
            }
        } else {
            // 未知 command，跳过参数（无法确定 count，直接终止）
            break;
        }
    }
    // 尾部未闭合的环（LINE 无 ClosePath）
    if (!currentRing.empty()) {
        rings.push_back(std::move(currentRing));
    }
    return true;
}

/// tile extent 坐标 → WGS84 经纬度
static inline void tileToLonLat(int32_t x, int32_t y, uint32_t extent,
                                double minLon, double minLat, double maxLon, double maxLat,
                                double &outLon, double &outLat) {
    outLon = minLon + (static_cast<double>(x) / extent) * (maxLon - minLon);
    outLat = maxLat - (static_cast<double>(y) / extent) * (maxLat - minLat); // y 自上而下
}

/// 解码单个 Feature 消息为 [VectorFeatureData]
static bool decodeFeature(const uint8_t *data, const uint8_t *end,
                          const std::vector<std::string> &keys,
                          const std::vector<MvtValue> &values,
                          uint32_t extent, MvtGeomType geomType,
                          double minLon, double minLat, double maxLon, double maxLat,
                          const std::string &labelField,
                          VectorFeatureData &outFeat) {
    uint64_t id = 0;
    bool hasId = false;
    std::vector<uint32_t> tags;
    std::vector<uint32_t> geometry;
    MvtGeomType featType = geomType; // 可被 feature-level type 覆盖

    const uint8_t *p = data;
    while (p && p < end) {
        uint32_t fn = 0, wt = 0;
        p = pb::readTag(p, end, fn, wt);
        if (!p) break;
        switch (fn) {
            case 1: { // id (varint 或 fixed64)
                if (wt == 0) { p = pb::readVarint(p, end, id); hasId = true; }
                else if (wt == 1 && p + 8 <= end) { std::memcpy(&id, p, 8); hasId = true; p += 8; }
                else p = pb::skipField(p, end, wt);
                break;
            }
            case 2: { // tags (packed varint)
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d) {
                        const uint8_t *tp = d; const uint8_t *te = d + l;
                        while (tp && tp < te) {
                            uint64_t v;
                            tp = pb::readVarint(tp, te, v);
                            if (tp) tags.push_back(static_cast<uint32_t>(v));
                        }
                    }
                } else if (wt == 0) { // 单个 varint（非 packed）
                    uint64_t v; p = pb::readVarint(p, end, v);
                    if (p) tags.push_back(static_cast<uint32_t>(v));
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 3: { // type (varint)
                if (wt == 0) {
                    uint64_t v; p = pb::readVarint(p, end, v);
                    featType = static_cast<MvtGeomType>(v);
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 4: { // geometry (packed varint)
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d) {
                        const uint8_t *gp = d; const uint8_t *ge = d + l;
                        while (gp && gp < ge) {
                            uint64_t v;
                            gp = pb::readVarint(gp, ge, v);
                            if (gp) geometry.push_back(static_cast<uint32_t>(v));
                        }
                    }
                } else p = pb::skipField(p, end, wt);
                break;
            }
            default:
                p = pb::skipField(p, end, wt);
                break;
        }
    }

    if (!hasId) return false;
    outFeat.fid = static_cast<long long>(id);

    // 解析属性 tags → label
    if (!labelField.empty()) {
        for (size_t ti = 0; ti + 1 < tags.size(); ti += 2) {
            uint32_t ki = tags[ti];
            uint32_t vi = tags[ti + 1];
            if (ki < keys.size() && keys[ki] == labelField && vi < values.size()) {
                if (values[vi].type == MvtValue::Type::String) {
                    outFeat.label = values[vi].s;
                } else if (values[vi].type == MvtValue::Type::Double ||
                           values[vi].type == MvtValue::Type::Float ||
                           values[vi].type == MvtValue::Type::Int ||
                           values[vi].type == MvtValue::Type::UInt ||
                           values[vi].type == MvtValue::Type::SInt) {
                    outFeat.label = std::to_string(values[vi].d);
                }
                break;
            }
        }
    }

    // 解码几何 commands
    if (geometry.empty() || extent == 0) return false;
    std::vector<std::vector<std::pair<int32_t, int32_t>>> rings;
    if (!decodeGeometry(geometry, extent, rings) || rings.empty()) return false;

    // 按 geomType 映射到 VectorFeatureData
    if (featType == MvtGeomType::Point) {
        // 每个环的第一个点为一个点要素（MVT 点可以多点）
        outFeat.type = VectorGeomType::Point;
        // 取第一个点
        if (!rings[0].empty()) {
            tileToLonLat(rings[0][0].first, rings[0][0].second, extent,
                         minLon, minLat, maxLon, maxLat, outFeat.lon, outFeat.lat);
        }
    } else if (featType == MvtGeomType::LineString) {
        outFeat.type = VectorGeomType::Line;
        outFeat.parts.resize(rings.size());
        for (size_t r = 0; r < rings.size(); ++r) {
            auto &part = outFeat.parts[r];
            part.resize(rings[r].size() * 2);
            for (size_t k = 0; k < rings[r].size(); ++k) {
                tileToLonLat(rings[r][k].first, rings[r][k].second, extent,
                             minLon, minLat, maxLon, maxLat,
                             part[k * 2], part[k * 2 + 1]);
            }
        }
    } else if (featType == MvtGeomType::Polygon) {
        outFeat.type = VectorGeomType::Polygon;
        // MVT POLYGON：第一个环为外环，后续为内环（洞）
        // 但 ClosePath 后的环顺序不保证外环在前，需按面积/方向判断
        // 简化：第一个环作外环，其余作洞（MVT 规范约定外环 CCW，内环 CW，但实际实现常不遵守）
        if (rings.size() == 1) {
            const auto &ring = rings[0];
            outFeat.outer.resize(ring.size() * 2);
            for (size_t k = 0; k < ring.size(); ++k) {
                tileToLonLat(ring[k].first, ring[k].second, extent,
                             minLon, minLat, maxLon, maxLat,
                             outFeat.outer[k * 2], outFeat.outer[k * 2 + 1]);
            }
        } else {
            // 多环：用面积判外/内环（最大面积为外环，其余为洞的近似）
            // 简化方案：第一个环作外环，其余作洞
            const auto &ring0 = rings[0];
            outFeat.outer.resize(ring0.size() * 2);
            for (size_t k = 0; k < ring0.size(); ++k) {
                tileToLonLat(ring0[k].first, ring0[k].second, extent,
                             minLon, minLat, maxLon, maxLat,
                             outFeat.outer[k * 2], outFeat.outer[k * 2 + 1]);
            }
            outFeat.holes.resize(rings.size() - 1);
            for (size_t r = 1; r < rings.size(); ++r) {
                auto &hole = outFeat.holes[r - 1];
                hole.resize(rings[r].size() * 2);
                for (size_t k = 0; k < rings[r].size(); ++k) {
                    tileToLonLat(rings[r][k].first, rings[r][k].second, extent,
                                 minLon, minLat, maxLon, maxLat,
                                 hole[k * 2], hole[k * 2 + 1]);
                }
            }
        }
    }
    return true;
}

// ==================== Layer 解码 ====================

struct MvtLayer {
    std::string name;
    std::vector<std::string> keys;
    std::vector<MvtValue> values;
    uint32_t extent = 4096;
    std::vector<const uint8_t *> featurePtrs; // 各 feature 消息的起始指针
    std::vector<const uint8_t *> featureEnds;
};

/// 解码 Layer 消息（只提取元数据 + feature 指针，不解码 feature 内部）
static bool decodeLayer(const uint8_t *data, const uint8_t *end, MvtLayer &out) {
    const uint8_t *p = data;
    while (p && p < end) {
        uint32_t fn = 0, wt = 0;
        p = pb::readTag(p, end, fn, wt);
        if (!p) break;
        switch (fn) {
            case 1: { // name
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d) out.name.assign(reinterpret_cast<const char *>(d), l);
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 2: { // features (repeated, length-delimited)
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d && l > 0) {
                        out.featurePtrs.push_back(d);
                        out.featureEnds.push_back(d + l);
                    }
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 3: { // keys (repeated string)
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d) out.keys.emplace_back(reinterpret_cast<const char *>(d), l);
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 4: { // values (repeated Value message)
                if (wt == 2) {
                    const uint8_t *d = nullptr; size_t l = 0;
                    p = pb::readBytes(p, end, d, l);
                    if (p && d && l > 0) {
                        out.values.push_back(decodeValue(d, d + l));
                    }
                } else p = pb::skipField(p, end, wt);
                break;
            }
            case 5: { // extent (varint)
                if (wt == 0) { uint64_t v; p = pb::readVarint(p, end, v); out.extent = static_cast<uint32_t>(v); }
                else p = pb::skipField(p, end, wt);
                break;
            }
            case 15: { // version
                if (wt == 0) { uint64_t v; p = pb::readVarint(p, end, v); }
                else p = pb::skipField(p, end, wt);
                break;
            }
            default:
                p = pb::skipField(p, end, wt);
                break;
        }
    }
    return true;
}

// ==================== Tile 解码入口 ====================

MvtDecodeResult decodeMvt(const std::vector<uint8_t> &bytes,
                          const std::string &layerName,
                          const std::string &labelField,
                          double minLon, double minLat, double maxLon, double maxLat) {
    MvtDecodeResult result;
    if (bytes.empty()) {
        result.error = "MVT 字节为空";
        return result;
    }

    const uint8_t *p = bytes.data();
    const uint8_t *end = p + bytes.size();

    // Tile 消息：repeated Layer (field 3, length-delimited)
    std::vector<MvtLayer> layers;
    while (p && p < end) {
        uint32_t fn = 0, wt = 0;
        p = pb::readTag(p, end, fn, wt);
        if (!p) break;
        if (fn == 3 && wt == 2) {
            // Layer 消息
            const uint8_t *d = nullptr; size_t l = 0;
            p = pb::readBytes(p, end, d, l);
            if (p && d && l > 0) {
                MvtLayer layer;
                if (decodeLayer(d, d + l, layer)) {
                    layers.push_back(std::move(layer));
                }
            }
        } else {
            p = pb::skipField(p, end, wt);
        }
    }

    if (layers.empty()) {
        result.error = "MVT 无 layer";
        return result;
    }

    // 按 layerName 选择目标 layer（空则取第一个）
    const MvtLayer *target = nullptr;
    if (!layerName.empty()) {
        for (const auto &l : layers) {
            if (l.name == layerName) { target = &l; break; }
        }
    }
    if (!target) target = &layers[0];

    // 逐 feature 解码
    result.hasBBox = true;
    result.minLon = minLon; result.minLat = minLat;
    result.maxLon = maxLon; result.maxLat = maxLat;

    for (size_t fi = 0; fi < target->featurePtrs.size(); ++fi) {
        VectorFeatureData feat;
        // 默认 geomType 从 feature 内部 type 字段读取（decodeFeature 会处理）
        if (decodeFeature(target->featurePtrs[fi], target->featureEnds[fi],
                         target->keys, target->values, target->extent,
                         MvtGeomType::Unknown, // 让 feature-level type 决定
                         minLon, minLat, maxLon, maxLat, labelField, feat)) {
            result.features.push_back(std::move(feat));
        }
    }

    result.ok = !result.features.empty();
    if (!result.ok && result.error.empty()) {
        result.error = "MVT 解码后无有效要素";
    }

    LOGI("MVT decoded: layer=%s features=%zu/%zu extent=%u bbox=[%.4f,%.4f,%.4f,%.4f]",
         target->name.c_str(), result.features.size(), target->featurePtrs.size(),
         target->extent, minLon, minLat, maxLon, maxLat);
    return result;
}

} // namespace globecore
