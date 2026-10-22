#ifndef GLOBECORE_TERRAIN_QUANTIZEDMESH_H
#define GLOBECORE_TERRAIN_QUANTIZEDMESH_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * QuantizedMesh：Cesium quantized-mesh v1.0 地形瓦片解码器。
 *
 * 【重要】本文件在 feature/terrain-pre 分支上按「真实服务的二进制实测」重写过。
 * 旧实现的 header 布局是错的（它假定 header 里带 bbox 与 z/x/y，并把 boundingSphere
 * 的 center+radius 误读为 bbox），且完全没处理 EdgeIndices 段 —— 解析标准瓦片必然错位。
 *
 * 标准 quantized-mesh v1.0 实际布局（小端，本分支按此实现）：
 *   [0..23]   center            double ×3   瓦片包围盒中心（ECEF 米）
 *   [24..31]  minimumHeight     float
 *             maximumHeight     float       该瓦片高度范围（米），h 量化的值域
 *   [32..55]  boundingSphere    double ×3   包围球中心（通常与 center 相同）
 *   [56..59]  boundingSphere    float       包围球半径
 *   [60..83]  horizonOcclusion  double ×3
 *   [84..87]  <保留 4 字节>      实测：部分生产服务（含 ctb-tile 系）在此多写 4 字节
 *   [88..91]  vertexCount       uint32      ← 顶点数据从 92 开始（不是 84！）
 *   顶点数据  u[vc] uint16, v[vc] uint16, height[vc] uint16（平面存放，非交错）
 *            ⚠️ 三段都是【zigzag 编码的增量】：真值[i] = 真值[i-1] + zigzagDecode(raw[i])。
 *               原始 uint16 可以 > 32767（实测 65534），直接当绝对值用会整体错位。
 *            u: 0=瓦片西边, 32767=东边；v: 0=瓦片南边, 32767=北边
 *            h: 0 → minimumHeight, 32767 → maximumHeight
 *   索引数据  triangleCount uint32 + indices[3*count]
 *            vertexCount > 65536 时索引为 uint32，否则 uint16
 *   边索引    west/south/east/north 四段：各 uint32 count + count 个索引
 *            （旧实现完全跳过此段 → 会把边索引当成扩展段，导致尾部解析错位）
 *   扩展段    循环：uint8 extensionId + uint32 length + length 字节数据
 *            id=1 oct-encoded 顶点法线（2 字节/顶点）；id=2 水掩码；id=4 元数据
 *
 * ⚠️ 标准 header 里【没有 bbox】——瓦片四至必须由 z/x/y 结合该服务的分块方案推算，
 *    故本解码器要求调用方显式传入 lonMin/latMin/lonMax/latMax。
 *
 * 解码后提供高度采样 sampleHeight()：对给定经纬度，找到包含该点的三角形并用重心坐标插值。
 * 未命中（不在 bbox 内 / 不在任何三角形内）返回 0.0（等同无地形）。
 */
struct QuantizedMeshData {
    /// 顶点：经度、纬度、高度（米，椭球面以上）
    std::vector<double> lons, lats, alts;
    /// 三角形索引（3×triangleCount，顶点索引指向 lons/lats/alts 同下标）
    std::vector<uint32_t> indices;
    /// 瓦片四至（度）。由调用方传入（标准 header 不含 bbox）
    double lonMin = 0.0, latMin = 0.0, lonMax = 0.0, latMax = 0.0;
    /// 高度范围（米），取自 header
    float belowMin = 0.0f, aboveMax = 0.0f;
    bool valid = false;

    // ── 高度栅格（feature/terrain-pre）──
    // 为什么需要：sampleHeight 若直接做「逐三角形重心判定」，一次调用是 O(triangleCount)
    // （实测单瓦片 400+ 三角形）。而 Renderer::buildGlobeTileMesh 每帧为每块可见瓦片重建
    // 球面网格，逐顶点采样 → 每帧几十万次三角形扫描，帧率会直接塌掉。
    // 故解码时把三角网一次性栅格化成 (gridN+1)² 的高度节点，采样退化为 O(1) 双线性插值。
    // 栅格化只在注入瓦片时做一次（每块 ~8 千次判定），与每帧开销无关。
    int gridN = 0;                 ///< 每边格子数；节点数为 (gridN+1)²
    std::vector<float> grid;       ///< 行优先，行 0 = 南边（latMin），列 0 = 西边（lonMin）
    bool gridReady = false;

    /// 把三角网栅格化成高度节点表。解码后由调用方（或 decodeQuantizedMesh 内部）调用一次。
    /// n 为每边格子数（默认 64：单瓦片 ~4 千节点，内存 ~17 KB/块，精度优于原始三角网在
    /// 球面网格上的可见差异）。
    void buildHeightGrid(int n = 64);

    /// 采样给定经纬度处的高度（米）：命中栅格时 O(1) 双线性插值；
    /// 落在四至外先钳制到边界（等价沿边界常数外推，保证跨瓦片高度连续）；
    /// 栅格不可用时回退逐三角形扫描；最终未命中返回 0.0（等同无地形）。
    double sampleHeight(double lon, double lat) const;
};

/**
 * 解码 quantized-mesh 字节流（标准 v1.0 布局）。
 *
 * @param bytes    quantized-mesh v1.0 原始字节（未 gzip；若服务返回 gzip 需先解压）
 * @param lonMin   瓦片西边经度（度）
 * @param latMin   瓦片南边纬度（度）—— 对应 v=0
 * @param lonMax   瓦片东边经度（度）
 * @param latMax   瓦片北边纬度（度）—— 对应 v=32767
 * @return         解码结果（lons/lats/alts + indices + bbox + valid）
 */
QuantizedMeshData decodeQuantizedMesh(const std::vector<uint8_t> &bytes,
                                      double lonMin, double latMin,
                                      double lonMax, double latMax);

} // namespace globecore

#endif // GLOBECORE_TERRAIN_QUANTIZEDMESH_H
