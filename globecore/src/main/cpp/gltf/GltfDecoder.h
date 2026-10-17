#ifndef GLOBECORE_GLTF_GLTFDECODER_H
#define GLOBECORE_GLTF_GLTFDECODER_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * GltfDecoder：binary glTF (.glb) v2 解码器。
 *
 * .glb 格式：12B header(magic+version+length) + JSON chunk(8B header+JSON) + BIN chunk(8B header+binary)。
 * 解码后提取 mesh primitives（POSITION/NORMAL 顶点 + indices + baseColorFactor 材质），
 * 供 Renderer 在 3D 通路用 colorProgram_ 绘制（或后续扩展 PBR 着色器）。
 *
 * 简化范围：
 *   - 只处理 .glb（binary glTF v2），不支持 .gltf（JSON+外部资源）
 *   - 只提取 POSITION/NORMAL/indices + baseColorFactor（PBR 材质简化为纯色）
 *   - 不处理动画、骨骼、纹理、Morph target
 *   - 坐标系：glTF 右手 Y-up，引擎 3D ECEF 左手 — 调用方负责变换
 *
 * 纯 CPU、无 GL 依赖，可在后台线程解码。
 */

struct GltfPrimitive {
    std::vector<float> positions;  // [x,y,z, x,y,z, ...]
    std::vector<float> normals;    // [x,y,z, ...]（可为空 = 无法线）
    std::vector<uint16_t> indices16; // uint16 索引（vertexCount<=65536 时）
    std::vector<uint32_t> indices32; // uint32 索引（vertexCount>65536 时）
    bool use32 = false;             // true=用 indices32，false=用 indices16
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f}; // RGBA [0,1]
};

struct GltfModel {
    bool ok = false;
    std::string error;
    std::vector<GltfPrimitive> primitives;
};

/**
 * 解码 .glb 字节流为 [GltfModel]。
 * @param bytes  .glb 原始字节
 * @return       解码结果（primitives + ok/error）
 */
GltfModel decodeGlb(const std::vector<uint8_t> &bytes);

} // namespace globecore

#endif // GLOBECORE_GLTF_GLTFDECODER_H
