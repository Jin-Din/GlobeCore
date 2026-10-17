#ifndef GLOBECORE_VECTOR_MAPBOXSTYLE_H
#define GLOBECORE_VECTOR_MAPBOXSTYLE_H

#include <string>
#include <vector>

#include "vector/VectorGeometry.h"

namespace globecore {

/**
 * MapboxStyle：解析 Mapbox Style JSON（v8）→ [StyleLayer] 列表。
 *
 * 每个 [StyleLayer] 携带 source-layer 名（用于匹配 MVT layer）+ [VectorStyle]（paint/layout
 * 属性映射到引擎的填充/描边/线/点颜色与线宽）。宿主据此为每条 MVT 图层创建矢量层并应用样式。
 *
 * paint 属性映射：
 *   fill-fill-color → fillR/G/B/A（alpha 取 fill-opacity）
 *   fill-fill-outline-color → outlineR/G/B/A
 *   line-line-color → lineR/G/B/A
 *   line-line-width → lineWidth（像素，常量值或 stops 取目标 zoom 的插值）
 *   circle-circle-color → pointR/G/B/A
 *   circle-circle-radius → pointRadiusDp
 *   symbol-text-field → labelField（形如 "{name}" → name）
 *   symbol-text-color → labelR/G/B/A
 *   symbol-text-size → labelSize
 *
 * 颜色格式支持 #RGB / #RRGGBB / #RRGGBBAA / rgb()/rgba()。
 * 线宽支持常量值或 stops（interpolate/step 简化为基础 stops 取最近 zoom）。
 *
 * 纯 CPU、无 GL 依赖，与 [parseJson] 配合使用。
 */

struct StyleLayer {
    std::string id;           // layer id（唯一标识）
    std::string source;       // source 名（对应 sources 中的 key）
    std::string sourceLayer;  // source-layer 名（匹配 MVT layer）
    std::string type;         // fill | line | circle | symbol
    bool visible = true;      // layout.visibility != "none"
    VectorStyle style;        // 映射后的引擎样式
    std::string labelField;   // 标注字段名（已去 {} 包裹）
};

struct MapboxStyleResult {
    bool ok = false;
    std::string error;
    std::vector<StyleLayer> layers;
    // source 的 tile URL 模板（取第一个 vector source）
    std::string tileUrlTemplate;
    int maxZoom = 14;
};

/**
 * 解析 Mapbox Style JSON 文本。
 * @param jsonText  完整的 Mapbox Style JSON 文本
 * @return          解析结果（layers + tileUrlTemplate + maxZoom）
 */
MapboxStyleResult parseMapboxStyle(const std::string &jsonText);

} // namespace globecore

#endif // GLOBECORE_VECTOR_MAPBOXSTYLE_H
