#ifndef GLOBECORE_TERRAIN_QUANTIZEDMESH_H
#define GLOBECORE_TERRAIN_QUANTIZEDMESH_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * QuantizedMesh：Cesium quantized-mesh v1.0 地形瓦片解码器。
 *
 * 格式规范（地形瓦片按 Web 墨卡托 z/x/y 分块，与瓦片纹理同一 quadtree）：
 *   Header(32B): centerX/Y/Z(double×3) 中心 ECEF, belowMin/aboveMax(float×2) 高度范围,
 *                lonMin/latMin/lonMax/latMax(float×4) bbox, z/x/y(int×3)
 *   VertexData: vertexCount(uint32), u/v/h[vertexCount](uint16×3) 量化坐标
 *     — u/v 为 tile 内归一化 [0,32767] → 经纬度; h 为 [0,32767] → [belowMin, aboveMax] 米
 *   IndexData: triangleCount(uint32), indices[3×count](uint16, 或 vertexCount>65536 时 uint32)
 *   Extension(ext): 可选 oct-encoded normals / water mask / edges 等（本期跳过）
 *
 * 解码后提供高度采样 [sampleHeight]：对给定经纬度，找到包含该点的三角形并用重心坐标插值高度。
 * 未命中（点不在瓦片范围 / 无三角形）返回 0.0（等同无地形）。
 *
 * 纯 CPU、无 GL 依赖，可在加载线程解码、GL 线程采样。供 Renderer::buildGlobeTileMesh 按需调用。
 */
struct QuantizedMeshData {
    /// 顶点：经度、纬度、高度（米，椭球面以上）
    std::vector<double> lons, lats, alts;
    /// 三角形索引（3×triangleCount，顶点索引指向 lons/lats/alts 同下标）
    std::vector<uint32_t> indices;
    /// 瓦片四至（度）
    double lonMin = 0.0, latMin = 0.0, lonMax = 0.0, latMax = 0.0;
    /// 高度范围（米）
    float belowMin = 0.0f, aboveMax = 0.0f;
    bool valid = false;

    /// 采样给定经纬度处的高度（米）。用点在三角形内判定 + 重心坐标插值。
    /// 未命中返回 0.0（等同无地形）。线性扫描 O(triangleCount)，瓦片三角形数通常 ~1-2k，可接受。
    double sampleHeight(double lon, double lat) const;
};

/**
 * 解码 quantized-mesh 字节流。失败（数据不足/格式不符）返回 valid=false。
 * @param bytes  quantized-mesh v1.0 原始字节
 * @return       解码结果（lons/lats/alts + indices + bbox + valid）
 */
QuantizedMeshData decodeQuantizedMesh(const std::vector<uint8_t> &bytes);

} // namespace globecore

#endif // GLOBECORE_TERRAIN_QUANTIZEDMESH_H
