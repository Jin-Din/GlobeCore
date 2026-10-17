#ifndef GLOBECORE_UTIL_JSONPARSER_H
#define GLOBECORE_UTIL_JSONPARSER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace globecore {

/**
 * 轻量递归下降 JSON 解析器（无外部依赖），用于解析 Mapbox Style JSON。
 * 支持：object / array / string / number / true / false / null。
 * 不支持：注释、尾随逗号（标准 JSON）。
 * 解析结果为 [JsonValue] 树，可通过键/索引访问。
 */
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;

    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj; // 键按字典序（Mapbox Style 字段查找不依赖顺序）

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBool() const { return type == Type::Bool; }

    /// 取 object 字段（不存在返回 Null）
    const JsonValue &at(const std::string &key) const {
        static JsonValue nullVal;
        auto it = obj.find(key);
        return (it != obj.end()) ? it->second : nullVal;
    }

    /// 取 array 元素（越界返回 Null）
    const JsonValue &at(size_t i) const {
        static JsonValue nullVal;
        return (i < arr.size()) ? arr[i] : nullVal;
    }
};

/// 解析 JSON 字符串。失败返回 type=Null。
JsonValue parseJson(const std::string &text);

/// 解析 JSON 字节流。失败返回 type=Null。
JsonValue parseJson(const std::vector<uint8_t> &bytes);

} // namespace globecore

#endif // GLOBECORE_UTIL_JSONPARSER_H
