#ifndef GLOBECORE_VECTOR_MVTDECODER_H
#define GLOBECORE_VECTOR_MVTDECODER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "vector/VectorReader.h"

namespace globecore {

/**
 * MvtDecoder：Mapbox Vector Tile (MVT) / PBF 解码器。
 *
 * 手写轻量 protobuf wire-format 解码（不依赖 libprotobuf），解析 MVT v2 spec：
 *   Tile → Layer(name/features/keys/values/extent) → Feature(id/tags/type/geometry)
 * 几何 commands（MoveTo=1/LineTo=2/ClosePath=7，zigzag int32 增量坐标）解码为
 * WGS84 经纬度坐标，按要素类型映射到 [VectorFeatureData]（复用现有 VectorBuilder 管线）。
 *
 * 瓦片坐标 [0, extent)（默认 4096）线性映射到瓦片经纬度包围盒：
 *   lon = lonMin + (x / extent) * (lonMax - lonMin)
 *   lat = latMax - (y / extent) * (latMax - latMin)  // y 自上而下（XYZ）
 *
 * 属性 tags 为 [keyIdx, valIdx, ...] 交替序列，keys/values 在 Layer 级别定义；
 * labelField 非空时从属性中提取该字段值作为 [VectorFeatureData.label]。
 *
 * 纯 CPU、无 GL 依赖，可在后台线程解码，输出直接喂入 [VectorBuilder::buildGeometry]。
 */
struct MvtDecodeResult {
    bool ok = false;
    std::string error;
    std::vector<VectorFeatureData> features;
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    bool hasBBox = false;
};

/**
 * 解码 MVT/PBF 字节流为 [VectorFeatureData] 数组。
 *
 * @param bytes       MVT/PBF 原始字节（protobuf 编码）
 * @param layerName   要解码的 source-layer 名（Mapbox Style 的 layer.source-layer）；
 *                    空串则取第一个 layer（兼容单层瓦片）
 * @param labelField  非空时从属性提取该字段值作为要素标注文本
 * @param minLon/minLat/maxLon/maxLat  瓦片 WGS84 经纬度包围盒（用于 tile 坐标→地理坐标映射）
 * @return            解码结果（features + bbox + ok/error）
 */
MvtDecodeResult decodeMvt(const std::vector<uint8_t> &bytes,
                          const std::string &layerName = std::string(),
                          const std::string &labelField = std::string(),
                          double minLon = 0.0, double minLat = 0.0,
                          double maxLon = 0.0, double maxLat = 0.0);

} // namespace globecore

#endif // GLOBECORE_VECTOR_MVTDECODER_H
