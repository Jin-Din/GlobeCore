#include "gltf/GltfDecoder.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include <cstring>

namespace globecore {

namespace {

/// 读取小端 uint32
static uint32_t readU32LE(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

/// glTF componentType → 每元素字节数
static int componentTypeSize(int ct) {
    switch (ct) {
        case 5120: return 1;  // BYTE
        case 5121: return 1;  // UNSIGNED_BYTE
        case 5122: return 2;  // SHORT
        case 5123: return 2;  // UNSIGNED_SHORT
        case 5125: return 4;  // UNSIGNED_INT
        case 5126: return 4;  // FLOAT
        default: return 0;
    }
}

/// glTF accessor type → 分量数
static int typeCount(const std::string &type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

/// 从 accessor 提取 float 数组（仅支持 FLOAT componentType + VEC3/SCALAR）
/// 返回提取的 float 数组（count × typeCount 个值）
static bool extractFloats(const JsonValue &accessor, const uint8_t *binData, size_t binSize,
                         std::vector<float> &out) {
    int ct = 5126; // 默认 FLOAT
    auto &ctVal = accessor.at("componentType");
    if (ctVal.isNumber()) ct = static_cast<int>(ctVal.numVal);
    if (ct != 5126) return false; // 本期只支持 FLOAT

    std::string type = accessor.at("type").strVal;
    int tc = typeCount(type);
    if (tc == 0) return false;

    int bvIdx = 0;
    auto &bvVal = accessor.at("bufferView");
    if (bvVal.isNumber()) bvIdx = static_cast<int>(bvVal.numVal);
    else return false;

    size_t byteOffset = 0;
    if (accessor.at("byteOffset").isNumber())
        byteOffset = static_cast<size_t>(accessor.at("byteOffset").numVal);

    int count = 0;
    if (accessor.at("count").isNumber())
        count = static_cast<int>(accessor.at("count").numVal);
    if (count <= 0) return false;

    // 需要 bufferView 信息
    // bufferView index 在 bvIdx，但本函数无权访问 bufferViews 数组
    // → 由调用方传入 byteOffset/byteLength/byteStride
    // 简化：这里不处理，由上层调用方直接传数据指针
    return false; // 占位，实际逻辑在 decodeGlb 中
}

/// 从 BIN chunk 提取 accessor 数据指针
/// 返回指向第一个 float 的指针 + stride（字节）
struct AccessorInfo {
    const uint8_t *data = nullptr;
    size_t count = 0;
    int componentType = 5126;
    int typeCount = 0;
    size_t byteOffset = 0;  // accessor 相对 bufferView
    size_t viewByteOffset = 0; // bufferView 相对 buffer
    size_t viewByteLength = 0;
    int viewByteStride = 0;  // 0=tight packed
    int bufferViewIdx = -1;
};

static AccessorInfo getAccessorInfo(const JsonValue &accessors, int accessorIdx) {
    AccessorInfo info;
    if (accessorIdx < 0 || static_cast<size_t>(accessorIdx) >= accessors.arr.size()) return info;
    const JsonValue &acc = accessors.arr[accessorIdx];

    info.count = acc.at("count").isNumber() ? static_cast<size_t>(acc.at("count").numVal) : 0;
    info.componentType = acc.at("componentType").isNumber() ? static_cast<int>(acc.at("componentType").numVal) : 5126;
    info.typeCount = typeCount(acc.at("type").strVal);
    info.byteOffset = acc.at("byteOffset").isNumber() ? static_cast<size_t>(acc.at("byteOffset").numVal) : 0;
    auto &bv = acc.at("bufferView");
    info.bufferViewIdx = bv.isNumber() ? static_cast<int>(bv.numVal) : -1;
    return info;
}

} // namespace

GltfModel decodeGlb(const std::vector<uint8_t> &bytes) {
    GltfModel model;
    if (bytes.size() < 20) { // 12 header + 8 chunk header 最小
        model.error = "glb 数据不足 20 字节";
        return model;
    }

    // ── Header ──
    const uint8_t *p = bytes.data();
    uint32_t magic = readU32LE(p);
    if (magic != 0x46546C67) { // 'glTF'
        model.error = "非 glb 格式（magic 不匹配）";
        return model;
    }
    uint32_t version = readU32LE(p + 4);
    if (version != 2) {
        model.error = "仅支持 glb v2";
        return model;
    }
    // uint32_t totalLength = readU32LE(p + 8);

    // ── JSON chunk ──
    p += 12;
    uint32_t jsonLen = readU32LE(p);
    uint32_t jsonType = readU32LE(p + 4);
    if (jsonType != 0x4E4F534A) { // 'JSON'
        model.error = "JSON chunk 类型错误";
        return model;
    }
    p += 8;
    if (p + jsonLen > bytes.data() + bytes.size()) {
        model.error = "JSON chunk 超出数据范围";
        return model;
    }
    std::string jsonText(reinterpret_cast<const char *>(p), jsonLen);
    JsonValue gltf = parseJson(jsonText);
    if (!gltf.isObject()) {
        model.error = "glTF JSON 解析失败";
        return model;
    }
    p += jsonLen;

    // ── BIN chunk ──
    const uint8_t *binData = nullptr;
    size_t binSize = 0;
    if (p + 8 <= bytes.data() + bytes.size()) {
        uint32_t binLen = readU32LE(p);
        uint32_t binType = readU32LE(p + 4);
        if (binType == 0x004E4942) { // 'BIN\0'
            binData = p + 8;
            binSize = (p + 8 + binLen <= bytes.data() + bytes.size()) ? binLen : (bytes.data() + bytes.size() - p - 8);
        }
    }
    if (!binData || binSize == 0) {
        model.error = "无 BIN chunk 或数据为空";
        return model;
    }

    // ── bufferViews ──
    const JsonValue &bufferViews = gltf.at("bufferViews");
    const JsonValue &accessors = gltf.at("accessors");
    const JsonValue &meshes = gltf.at("meshes");
    const JsonValue &materials = gltf.at("materials");

    // helper: 从 accessor 索引提取 float 数组
    auto extractFloatArray = [&](int accessorIdx, std::vector<float> &out) -> bool {
        AccessorInfo ai = getAccessorInfo(accessors, accessorIdx);
        if (ai.bufferViewIdx < 0 || !ai.typeCount || !ai.count) return false;
        if (static_cast<size_t>(ai.bufferViewIdx) >= bufferViews.arr.size()) return false;
        const JsonValue &bv = bufferViews.arr[ai.bufferViewIdx];
        size_t bvOffset = bv.at("byteOffset").isNumber() ? static_cast<size_t>(bv.at("byteOffset").numVal) : 0;
        size_t bvLength = bv.at("byteLength").isNumber() ? static_cast<size_t>(bv.at("byteLength").numVal) : 0;
        int bvStride = bv.at("byteStride").isNumber() ? static_cast<int>(bv.at("byteStride").numVal) : 0;
        if (bvOffset + bvLength > binSize) return false;

        const uint8_t *dataBase = binData + bvOffset + ai.byteOffset;
        int compSize = componentTypeSize(ai.componentType);
        if (compSize == 0) return false;
        int stride = (bvStride > 0) ? bvStride : (compSize * ai.typeCount);
        out.resize(ai.count * ai.typeCount);

        for (size_t i = 0; i < ai.count; ++i) {
            const uint8_t *elem = dataBase + i * stride;
            for (int c = 0; c < ai.typeCount; ++c) {
                if (ai.componentType == 5126) { // FLOAT
                    float f;
                    std::memcpy(&f, elem + c * 4, 4);
                    out[i * ai.typeCount + c] = f;
                }
                // 其他类型可扩展
            }
        }
        return true;
    };

    // helper: 从 accessor 索引提取 index 数组
    auto extractIndexArray = [&](int accessorIdx, GltfPrimitive &prim) -> bool {
        AccessorInfo ai = getAccessorInfo(accessors, accessorIdx);
        if (ai.bufferViewIdx < 0 || !ai.count) return false;
        if (static_cast<size_t>(ai.bufferViewIdx) >= bufferViews.arr.size()) return false;
        const JsonValue &bv = bufferViews.arr[ai.bufferViewIdx];
        size_t bvOffset = bv.at("byteOffset").isNumber() ? static_cast<size_t>(bv.at("byteOffset").numVal) : 0;
        size_t bvLength = bv.at("byteLength").isNumber() ? static_cast<size_t>(bv.at("byteLength").numVal) : 0;
        if (bvOffset + bvLength > binSize) return false;

        const uint8_t *dataBase = binData + bvOffset + ai.byteOffset;
        int compSize = componentTypeSize(ai.componentType);
        if (compSize == 0) return false;

        if (ai.componentType == 5123) { // UNSIGNED_SHORT
            prim.use32 = false;
            prim.indices16.resize(ai.count);
            for (size_t i = 0; i < ai.count; ++i) {
                uint16_t idx;
                std::memcpy(&idx, dataBase + i * 2, 2);
                prim.indices16[i] = idx;
            }
            return true;
        } else if (ai.componentType == 5125) { // UNSIGNED_INT
            prim.use32 = true;
            prim.indices32.resize(ai.count);
            for (size_t i = 0; i < ai.count; ++i) {
                uint32_t idx;
                std::memcpy(&idx, dataBase + i * 4, 4);
                prim.indices32[i] = idx;
            }
            return true;
        }
        return false;
    };

    // ── 逐 mesh → primitive ──
    if (!meshes.isArray()) {
        model.error = "无 meshes";
        return model;
    }
    for (const auto &mesh : meshes.arr) {
        const JsonValue &prims = mesh.at("primitives");
        if (!prims.isArray()) continue;
        for (const auto &primVal : prims.arr) {
            GltfPrimitive prim;
            const JsonValue &attrs = primVal.at("attributes");
            if (!attrs.isObject()) continue;

            // POSITION (required)
            auto &posIdx = attrs.at("POSITION");
            if (!posIdx.isNumber()) continue;
            if (!extractFloatArray(static_cast<int>(posIdx.numVal), prim.positions)) continue;

            // NORMAL (optional)
            auto &nrmIdx = attrs.at("NORMAL");
            if (nrmIdx.isNumber()) {
                extractFloatArray(static_cast<int>(nrmIdx.numVal), prim.normals);
            }

            // indices (optional)
            auto &idxIdx = primVal.at("indices");
            if (idxIdx.isNumber()) {
                extractIndexArray(static_cast<int>(idxIdx.numVal), prim);
            }

            // material (optional) → baseColorFactor
            auto &matIdx = primVal.at("material");
            if (matIdx.isNumber() && materials.isArray() &&
                static_cast<size_t>(matIdx.numVal) < materials.arr.size()) {
                const JsonValue &mat = materials.arr[static_cast<size_t>(matIdx.numVal)];
                const JsonValue &pbr = mat.at("pbrMetallicRoughness");
                if (pbr.isObject()) {
                    const JsonValue &bcf = pbr.at("baseColorFactor");
                    if (bcf.isArray() && bcf.arr.size() >= 4) {
                        for (int c = 0; c < 4; ++c) {
                            if (bcf.arr[c].isNumber())
                                prim.baseColor[c] = static_cast<float>(bcf.arr[c].numVal);
                        }
                    }
                }
            }

            // 若无 indices，生成默认 strip indices
            if (!prim.use32 && prim.indices16.empty() && prim.indices32.empty()) {
                size_t vc = prim.positions.size() / 3;
                if (vc <= 65535) {
                    prim.use32 = false;
                    prim.indices16.resize(vc);
                    for (size_t i = 0; i < vc; ++i) prim.indices16[i] = static_cast<uint16_t>(i);
                } else {
                    prim.use32 = true;
                    prim.indices32.resize(vc);
                    for (size_t i = 0; i < vc; ++i) prim.indices32[i] = static_cast<uint32_t>(i);
                }
            }

            model.primitives.push_back(std::move(prim));
        }
    }

    model.ok = !model.primitives.empty();
    if (!model.ok && model.error.empty()) {
        model.error = "无有效 primitive";
    }

    LOGI("glb decoded: prims=%zu verts=%s",
         model.primitives.size(),
         model.primitives.empty() ? "0" : std::to_string(model.primitives[0].positions.size() / 3).c_str());
    return model;
}

} // namespace globecore
