#ifndef GLOBECORE_RENDER_KTX2DECODER_H
#define GLOBECORE_RENDER_KTX2DECODER_H

#include <cstdint>
#include <vector>

#include <GLES2/gl2.h>

namespace globecore {

/**
 * Ktx2Decoder：KTX2 容器格式解析器（Khronos KTX v2）。
 *
 * KTX2 是 GPU 压缩纹理的标准容器（ETC2/ASTC/BC 等），支持硬件直传免运行时解码，
 * 内存占用减少 4-8 倍（RGBA8 256²=256KB → ETC2≈64KB / ASTC 4×4≈32KB）。
 *
 * 格式结构：
 *   Header(80B): magic(0xAB4B5458) + version + type + typeSize + format + internalFormat +
 *                pixelWidth/pixelHeight/pixelDepth + layerCount/faceCount/levelCount +
 *                supercompressionScheme + DFD/KV/SGD offsets+lengths
 *   DFD (Data Format Descriptor)
 *   KV (Key-Value metadata)
 *   Level data (offsets+lengths + compressed/uncompressed bytes)
 *
 * 本解析器提取 mip level 0（基础级别）的压缩数据 + GL 内部格式，
 * 供 [Texture::uploadCompressed] 直传上传。多 mip 级别暂不支持（瓦片纹理常单级）。
 *
 * vkFormat → GL 压缩格式映射：
 *   ETC2 RGB8 (37808=GL_COMPRESSED_RGB8_ETC2)
 *   ETC2 RGB8A1 (37809=GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2)
 *   ETC2 RGBA8 (37810=GL_COMPRESSED_RGBA8_ETC2_EAC)
 *   ASTC 4×4 RGBA (37819=GL_COMPRESSED_RGBA_ASTC_4x4)
 *   ...（见 vkFormatToGl）
 *
 * 纯 CPU、无 GL 依赖（输出 GL 常量但不调用 GL）。
 */

struct Ktx2Data {
    bool ok = false;
    GLenum glFormat = 0;       // GL 压缩内部格式（ETC2/ASTC 等）
    int width = 0;            // 基础级别宽
    int height = 0;           // 基础级别高
    std::vector<uint8_t> data; // 基础级别压缩数据
};

/**
 * 解析 KTX2 字节流，提取基础级别的压缩纹理数据 + GL 格式。
 * 失败返回 ok=false。
 */
Ktx2Data decodeKtx2(const std::vector<uint8_t> &bytes);

} // namespace globecore

#endif // GLOBECORE_RENDER_KTX2DECODER_H
