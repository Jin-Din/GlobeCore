#include "vector/MapboxStyle.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include <cstdlib>
#include <string>

namespace globecore {

namespace {

/// 解析颜色字符串到 RGBA [0,1] 浮点。返回 false 表示无法解析。
/// 支持 #RGB / #RRGGBB / #RRGGBBAA / rgb(r,g,b) / rgba(r,g,b,a)
bool parseColor(const std::string &s, float &r, float &g, float &b, float &a) {
    a = 1.0f;
    if (s.empty()) return false;

    if (s[0] == '#') {
        // hex
        const char *hex = s.c_str() + 1;
        size_t len = s.size() - 1;
        auto hexToByte = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (len == 3) {
            r = static_cast<float>((hexToByte(hex[0]) * 17) / 255.0);
            g = static_cast<float>((hexToByte(hex[1]) * 17) / 255.0);
            b = static_cast<float>((hexToByte(hex[2]) * 17) / 255.0);
            return true;
        } else if (len == 6 || len == 8) {
            auto byte2 = [&](size_t i) { return (hexToByte(hex[i]) * 16 + hexToByte(hex[i + 1])); };
            r = byte2(0) / 255.0f;
            g = byte2(2) / 255.0f;
            b = byte2(4) / 255.0f;
            if (len == 8) a = byte2(6) / 255.0f;
            return true;
        }
        return false;
    }

    if (s.size() >= 5 && (s.substr(0, 4) == "rgb(" || s.substr(0, 5) == "rgba(")) {
        bool isRgba = (s[3] == 'a');
        size_t valStart = isRgba ? 5 : 4;
        size_t end = s.find(')');
        if (end == std::string::npos) return false;
        std::string body = s.substr(valStart, end - valStart);
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        size_t pos = 0;
        while (pos < body.size() && n < 4) {
            size_t comma = body.find(',', pos);
            std::string part = (comma == std::string::npos) ? body.substr(pos) : body.substr(pos, comma - pos);
            // trim spaces
            while (!part.empty() && part.front() == ' ') part.erase(0, 1);
            while (!part.empty() && part.back() == ' ') part.pop_back();
            if (n < 3) {
                vals[n] = static_cast<float>(std::atof(part.c_str()) / 255.0);
            } else {
                vals[n] = static_cast<float>(std::atof(part.c_str())); // alpha 0-1
            }
            n++;
            pos = comma;
            if (comma == std::string::npos) break;
            ++pos;
        }
        r = vals[0]; g = vals[1]; b = vals[2];
        if (n >= 4) a = vals[3];
        return true;
    }
    return false;
}

/// 从 JsonValue 提取颜色表达式（支持常量字符串或 [interpolate, ...] 简化为取第一个 stops 值）
/// 简化实现：如果值是字符串则解析颜色；如果是数组（表达式）则取最后一个常量值。
bool extractColor(const JsonValue &v, float &r, float &g, float &b, float &a) {
    if (v.isString()) {
        return parseColor(v.strVal, r, g, b, a);
    }
    if (v.isArray()) {
        // 表达式数组：尝试找到最后一个 string 类型的元素作为颜色值
        for (int i = static_cast<int>(v.arr.size()) - 1; i >= 0; --i) {
            if (v.arr[i].isString() && v.arr[i].strVal[0] == '#') {
                return parseColor(v.arr[i].strVal, r, g, b, a);
            }
        }
    }
    return false;
}

/// 从 JsonValue 提取数值（支持常量或 [interpolate, ...] 表达式简化为取最后常量）
double extractNumber(const JsonValue &v) {
    if (v.isNumber()) return v.numVal;
    if (v.isArray()) {
        // 表达式：找最后一个 number 元素
        for (int i = static_cast<int>(v.arr.size()) - 1; i >= 0; --i) {
            if (v.arr[i].isNumber()) return v.arr[i].numVal;
        }
    }
    return 0.0;
}

/// 提取标注字段名（symbol text-field 形如 "{name}" 或 ["get", "name"] → "name"）
std::string extractLabelField(const JsonValue &v) {
    if (v.isString()) {
        std::string s = v.strVal;
        // 去除 { } 包裹
        if (!s.empty() && s.front() == '{' && s.back() == '}') {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }
    if (v.isArray() && v.arr.size() >= 2) {
        // ["get", "name"] 或 ["format", ...] 简化取第一个 string 参数
        if (v.arr[0].isString() && v.arr[0].strVal == "get" && v.arr[1].isString()) {
            return v.arr[1].strVal;
        }
    }
    return std::string();
}

/// 把 Mapbox paint/layout 属性映射到 [VectorStyle]
void applyStyleProps(const JsonValue &paint, const JsonValue &layout, const std::string &layerType,
                     VectorStyle &style, std::string &labelField) {
    float r = 0, g = 0, b = 0, a = 1;

    if (layerType == "fill") {
        if (extractColor(paint.at("fill-color"), r, g, b, a)) {
            style.fillR = r; style.fillG = g; style.fillB = b;
        }
        float opacity = 1.0f;
        auto &fo = paint.at("fill-opacity");
        if (fo.isNumber()) opacity = static_cast<float>(fo.numVal);
        else if (fo.isArray()) opacity = static_cast<float>(extractNumber(fo));
        style.fillA = a * opacity;

        if (extractColor(paint.at("fill-outline-color"), r, g, b, a)) {
            style.outlineR = r; style.outlineG = g; style.outlineB = b; style.outlineA = a;
        }
    } else if (layerType == "line") {
        if (extractColor(paint.at("line-color"), r, g, b, a)) {
            style.lineR = r; style.lineG = g; style.lineB = b; style.lineA = a;
        }
        style.lineWidth = static_cast<float>(extractNumber(paint.at("line-width")));
    } else if (layerType == "circle") {
        if (extractColor(paint.at("circle-color"), r, g, b, a)) {
            style.pointR = r; style.pointG = g; style.pointB = b; style.pointA = a;
        }
        style.pointRadiusDp = static_cast<float>(extractNumber(paint.at("circle-radius")));
    } else if (layerType == "symbol") {
        labelField = extractLabelField(layout.at("text-field"));
        if (!labelField.empty()) {
            style.labelField = labelField;
            if (extractColor(paint.at("text-color"), r, g, b, a)) {
                style.labelR = r; style.labelG = g; style.labelB = b;
            }
            style.labelSize = static_cast<float>(extractNumber(layout.at("text-size")));
            if (style.labelSize <= 0) style.labelSize = 1.0f;
        }
    }
}

} // namespace

MapboxStyleResult parseMapboxStyle(const std::string &jsonText) {
    MapboxStyleResult result;
    JsonValue root = parseJson(jsonText);
    if (!root.isObject()) {
        result.error = "JSON 解析失败或非对象";
        return result;
    }

    // sources → tileUrlTemplate + maxZoom（取第一个 vector source）
    const JsonValue &sources = root.at("sources");
    if (sources.isObject()) {
        for (const auto &src : sources.obj) {
            const JsonValue &srcVal = src.second;
            if (srcVal.at("type").isString() && srcVal.at("type").strVal == "vector") {
                const JsonValue &tiles = srcVal.at("tiles");
                if (tiles.isArray() && !tiles.arr.empty() && tiles.arr[0].isString()) {
                    result.tileUrlTemplate = tiles.arr[0].strVal;
                }
                if (srcVal.at("maxzoom").isNumber()) {
                    result.maxZoom = static_cast<int>(srcVal.at("maxzoom").numVal);
                }
                break; // 取第一个 vector source
            }
        }
    }

    // layers → StyleLayer 列表
    const JsonValue &layers = root.at("layers");
    if (!layers.isArray()) {
        result.error = "layers 非数组";
        return result;
    }

    for (const auto &layerVal : layers.arr) {
        if (!layerVal.isObject()) continue;
        StyleLayer sl;
        sl.id = layerVal.at("id").strVal;
        sl.source = layerVal.at("source").strVal;
        sl.sourceLayer = layerVal.at("source-layer").strVal;
        sl.type = layerVal.at("type").strVal;

        // visibility
        const JsonValue &layout = layerVal.at("layout");
        if (layout.isObject()) {
            const JsonValue &vis = layout.at("visibility");
            if (vis.isString() && vis.strVal == "none") {
                sl.visible = false;
            }
        }

        // paint → VectorStyle
        const JsonValue &paint = layerVal.at("paint");
        applyStyleProps(paint, layout, sl.type, sl.style, sl.labelField);

        result.layers.push_back(std::move(sl));
    }

    result.ok = !result.layers.empty();
    if (!result.ok && result.error.empty()) {
        result.error = "无有效样式层";
    }

    LOGI("MapboxStyle parsed: layers=%zu tileUrl=%s maxZoom=%d",
         result.layers.size(), result.tileUrlTemplate.c_str(), result.maxZoom);
    return result;
}

} // namespace globecore
