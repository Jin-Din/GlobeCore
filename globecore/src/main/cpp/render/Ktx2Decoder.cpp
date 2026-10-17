#include "render/Ktx2Decoder.h"

#include "util/Log.h"

#include <cstring>

namespace globecore {

namespace {

// KTX2 magic bytes: 0xAB 0x4B 0x54 0x58 ("KTX")
constexpr uint32_t kKtx2Magic = 0x4B5458AB; // 小端读为 0xAB4B5458 → 我们按字节比较

// KTX2 supercompression schemes
constexpr uint32_t kSupercompNone = 0;
// constexpr uint32_t kSupercompBasisLZ = 1;
// constexpr uint32_t kSupercompZstd = 2;

struct Ktx2Header {
    uint8_t magic[4];
    uint32_t version = 0;
    uint32_t type = 0;        // 0=UNSIGNED_BYTE
    uint32_t typeSize = 1;
    uint32_t format = 0;
    uint32_t internalFormat = 0;
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;
    uint32_t pixelDepth = 0;
    uint32_t layerCount = 0;
    uint32_t faceCount = 0;
    uint32_t levelCount = 0;
    uint32_t supercompScheme = 0;
    uint32_t dfdByteOffset = 0;
    uint32_t dfdByteLength = 0;
    uint32_t kvdByteOffset = 0;
    uint32_t kvdByteLength = 0;
    uint64_t sgdByteOffset = 0;
    uint64_t sgdByteLength = 0;
};

static uint32_t readU32LE(const uint8_t *p) {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

static uint64_t readU64LE(const uint8_t *p) {
    uint64_t v; std::memcpy(&v, p, 8); return v;
}

static Ktx2Header parseHeader(const uint8_t *p, size_t size) {
    Ktx2Header h;
    if (size < 80) return h;
    std::memcpy(h.magic, p, 4);
    h.version = readU32LE(p + 4);
    h.type = readU32LE(p + 8);
    h.typeSize = readU32LE(p + 12);
    h.format = readU32LE(p + 16);
    h.internalFormat = readU32LE(p + 20);
    h.pixelWidth = readU32LE(p + 24);
    h.pixelHeight = readU32LE(p + 28);
    h.pixelDepth = readU32LE(p + 32);
    h.layerCount = readU32LE(p + 36);
    h.faceCount = readU32LE(p + 40);
    h.levelCount = readU32LE(p + 44);
    h.supercompScheme = readU32LE(p + 48);
    h.dfdByteOffset = readU32LE(p + 52);
    h.dfdByteLength = readU32LE(p + 56);
    h.kvdByteOffset = readU32LE(p + 60);
    h.kvdByteLength = readU32LE(p + 64);
    h.sgdByteOffset = readU64LE(p + 68);
    h.sgdByteLength = readU64LE(p + 76);
    return h;
}

/// KTX2 level offset entry (8B offset + 8B uncompressed + 8B compressed)
struct LevelEntry {
    uint64_t byteOffset;
    uint64_t byteLength;     // uncompressed size
    uint64_t compressedLength; // compressed size (0 if no supercomp)
};

static LevelEntry readLevelEntry(const uint8_t *p) {
    LevelEntry e;
    e.byteOffset = readU64LE(p);
    e.byteLength = readU64LE(p + 8);
    e.compressedLength = readU64LE(p + 16);
    return e;
}

/// vkFormat → GL 压缩格式映射（KTX2 header.format 字段在 GLES 2.0 常为 0，
/// 需从 DFD 的 vendor id 映射。但 KTX2 spec 也在 header.vkFormat... 不，KTX2
/// header 无 vkFormat，改为 DFD 的 sample.model+qualifiers。
/// 简化：支持常见 ETC2/ASTC 的 vkFormat 值（从 DFD BDFD sample.model 映射）
/// 更简化：支持 header.internalFormat 字段（当 KTX2 由 GL 生成时含此字段）
GLenum vkFormatToGl(uint32_t internalFormat) {
    switch (internalFormat) {
        // ETC2 (GLES 3.0 core / GLES 2.0 extension)
        case 0x9274: return 0x9274; // GL_COMPRESSED_RGB8_ETC2
        case 0x9276: return 0x9276; // GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2
        case 0x9278: return 0x9278; // GL_COMPRESSED_RGBA8_ETC2_EAC
        // ASTC (GL_OES_texture_compression_astc)
        case 0x93B0: return 0x93B0; // GL_COMPRESSED_RGBA_ASTC_4x4
        case 0x93B1: return 0x93B1; // GL_COMPRESSED_RGBA_ASTC_5x4
        case 0x93B2: return 0x93B2; // GL_COMPRESSED_RGBA_ASTC_5x5
        case 0x93B3: return 0x93B3; // GL_COMPRESSED_RGBA_ASTC_6x5
        case 0x93B4: return 0x93B4; // GL_COMPRESSED_RGBA_ASTC_6x6
        case 0x93B5: return 0x93B5; // GL_COMPRESSED_RGBA_ASTC_8x5
        case 0x93B6: return 0x93B6; // GL_COMPRESSED_RGBA_ASTC_8x6
        case 0x93B7: return 0x93B7; // GL_COMPRESSED_RGBA_ASTC_8x8
        case 0x93B8: return 0x93B8; // GL_COMPRESSED_RGBA_ASTC_10x5
        case 0x93B9: return 0x93B9; // GL_COMPRESSED_RGBA_ASTC_10x6
        case 0x93BA: return 0x93BA; // GL_COMPRESSED_RGBA_ASTC_10x8
        case 0x93BB: return 0x93BB; // GL_COMPRESSED_RGBA_ASTC_10x10
        case 0x93BC: return 0x93BC; // GL_COMPRESSED_RGBA_ASTC_12x10
        case 0x93BD: return 0x93BD; // GL_COMPRESSED_RGBA_ASTC_12x12
        // EAC (R/RG 通道)
        case 0x9270: return 0x9270; // GL_COMPRESSED_R11_EAC
        case 0x9272: return 0x9272; // GL_COMPRESSED_RG11_EAC
        case 0x9271: return 0x9271; // GL_COMPRESSED_SIGNED_R11_EAC
        case 0x9273: return 0x9273; // GL_COMPRESSED_SIGNED_RG11_EAC
        default: return 0; // 未知/不支持
    }
}

} // namespace

Ktx2Data decodeKtx2(const std::vector<uint8_t> &bytes) {
    Ktx2Data result;
    if (bytes.size() < 80) {
        LOGW("KTX2: 数据不足 80B (%zu)", bytes.size());
        return result;
    }

    const uint8_t *base = bytes.data();
    size_t size = bytes.size();

    Ktx2Header h = parseHeader(base, size);

    // 验证 magic
    if (h.magic[0] != 0xAB || h.magic[1] != 0x4B || h.magic[2] != 0x54 || h.magic[3] != 0x58) {
        LOGW("KTX2: magic 不匹配");
        return result;
    }
    if (h.version != 2) {
        LOGW("KTX2: version=%u 非 2", h.version);
        return result;
    }

    result.width = static_cast<int>(h.pixelWidth);
    result.height = static_cast<int>(h.pixelHeight);
    if (result.width <= 0 || result.height <= 0) {
        LOGW("KTX2: 尺寸无效 w=%u h=%u", h.pixelWidth, h.pixelHeight);
        return result;
    }

    // GL 格式映射
    result.glFormat = vkFormatToGl(h.internalFormat);
    if (result.glFormat == 0) {
        LOGW("KTX2: internalFormat=0x%x 不支持", h.internalFormat);
        return result;
    }

    // 超压缩不支持（Basis/zstd 需额外解压库）
    if (h.supercompScheme != kSupercompNone) {
        LOGW("KTX2: supercomp=%u 不支持（仅支持无超压缩）", h.supercompScheme);
        return result;
    }

    // Level offset 表：levelCount × 24B entries
    if (h.levelCount == 0) {
        LOGW("KTX2: levelCount=0");
        return result;
    }

    // Level offset 表紧跟 header（80B 之后）
    const uint8_t *levelTable = base + 80;
    size_t levelTableSize = static_cast<size_t>(h.levelCount) * 24;
    if (80 + levelTableSize > size) {
        LOGW("KTX2: level 表越界");
        return result;
    }

    // levelCount 在 KTX2 中按「粗→细」逆序排列（level 0 = 最粗）
    // 但有时 level 0 = 最细。我们取最大尺寸的 level（= 基础级别）
    // KTX2 spec: level 0 是最粗级别，levelCount-1 是最细
    // 但实际数据中 level 0 的 byteOffset 常指向基础级别（最大尺寸）
    // 简化：取 levelCount-1 的最后一个条目（按 spec 是最细 = 基础）
    // 但为安全，找 byteLength 最大的条目作为基础级别
    LevelEntry best = {};
    bool found = false;
    for (uint32_t li = 0; li < h.levelCount; ++li) {
        LevelEntry e = readLevelEntry(levelTable + li * 24);
        if (e.byteOffset == 0 || (e.compressedLength == 0 && e.byteLength == 0)) continue;
        uint64_t dataSize = (e.compressedLength > 0) ? e.compressedLength : e.byteLength;
        if (e.byteOffset + dataSize > size) continue;
        if (!found || dataSize > ((best.compressedLength > 0 ? best.compressedLength : best.byteLength))) {
            best = e;
            found = true;
        }
    }

    if (!found) {
        LOGW("KTX2: 无有效 level 条目");
        return result;
    }

    uint64_t dataSize = (best.compressedLength > 0) ? best.compressedLength : best.byteLength;
    if (best.byteOffset + dataSize > size) {
        LOGW("KTX2: level 数据越界 off=%llu size=%llu", 
             static_cast<unsigned long long>(best.byteOffset),
             static_cast<unsigned long long>(dataSize));
        return result;
    }

    result.data.resize(static_cast<size_t>(dataSize));
    std::memcpy(result.data.data(), base + best.byteOffset, static_cast<size_t>(dataSize));
    result.ok = true;

    LOGI("KTX2 decoded: w=%d h=%d fmt=0x%x dataSize=%zu",
         result.width, result.height, result.glFormat, result.data.size());
    return result;
}

} // namespace globecore
