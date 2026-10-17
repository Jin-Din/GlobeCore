#include "util/JsonParser.h"

#include <cctype>
#include <cstring>

namespace globecore {

namespace {

class JsonParser {
public:
    JsonParser(const char *p, const char *end) : p_(p), end_(end) {}

    JsonValue parse() {
        skipWs();
        return parseValue();
    }

private:
    const char *p_;
    const char *end_;

    void skipWs() {
        while (p_ < end_ && (static_cast<unsigned char>(*p_) <= ' ')) ++p_;
    }

    bool match(char c) {
        skipWs();
        if (p_ < end_ && *p_ == c) { ++p_; return true; }
        return false;
    }

    bool peek(char c) {
        skipWs();
        return (p_ < end_ && *p_ == c);
    }

    JsonValue parseValue() {
        skipWs();
        if (p_ >= end_) return {};
        char c = *p_;
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        return {};
    }

    JsonValue parseObject() {
        JsonValue v; v.type = JsonValue::Type::Object;
        ++p_; // skip '{'
        skipWs();
        if (peek('}')) { ++p_; return v; } // empty
        while (p_ < end_) {
            skipWs();
            if (*p_ != '"') return v; // 错误
            JsonValue key = parseString();
            skipWs();
            if (!match(':')) return v;
            JsonValue val = parseValue();
            v.obj[key.strVal] = std::move(val);
            skipWs();
            if (match(',')) continue;
            if (match('}')) break;
            return v; // 错误
        }
        return v;
    }

    JsonValue parseArray() {
        JsonValue v; v.type = JsonValue::Type::Array;
        ++p_; // skip '['
        skipWs();
        if (peek(']')) { ++p_; return v; } // empty
        while (p_ < end_) {
            JsonValue val = parseValue();
            v.arr.push_back(std::move(val));
            skipWs();
            if (match(',')) continue;
            if (match(']')) break;
            return v; // 错误
        }
        return v;
    }

    JsonValue parseString() {
        JsonValue v; v.type = JsonValue::Type::String;
        ++p_; // skip opening '"'
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return v; // 闭合
            if (c == '\\' && p_ < end_) {
                char esc = *p_++;
                switch (esc) {
                    case '"': v.strVal += '"'; break;
                    case '\\': v.strVal += '\\'; break;
                    case '/': v.strVal += '/'; break;
                    case 'b': v.strVal += '\b'; break;
                    case 'f': v.strVal += '\f'; break;
                    case 'n': v.strVal += '\n'; break;
                    case 'r': v.strVal += '\r'; break;
                    case 't': v.strVal += '\t'; break;
                    case 'u': {
                        // \uXXXX → UTF-8（简单实现：只处理 BMP）
                        if (p_ + 4 > end_) return v;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p_++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return v;
                        }
                        if (cp < 0x80) {
                            v.strVal += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            v.strVal += static_cast<char>(0xC0 | (cp >> 6));
                            v.strVal += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            v.strVal += static_cast<char>(0xE0 | (cp >> 12));
                            v.strVal += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            v.strVal += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: v.strVal += esc; break;
                }
            } else {
                v.strVal += c;
            }
        }
        return v; // 截断（无闭合引号）
    }

    JsonValue parseNumber() {
        JsonValue v; v.type = JsonValue::Type::Number;
        const char *start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        while (p_ < end_) {
            char c = *p_;
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++p_;
            } else break;
        }
        std::string num(start, p_ - start);
        v.numVal = std::stod(num);
        return v;
    }

    JsonValue parseBool() {
        JsonValue v; v.type = JsonValue::Type::Bool;
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) {
            v.boolVal = true; p_ += 4;
        } else if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) {
            v.boolVal = false; p_ += 5;
        }
        return v;
    }

    JsonValue parseNull() {
        if (end_ - p_ >= 4 && std::strncmp(p_, "null", 4) == 0) {
            p_ += 4;
        }
        return {}; // type=Null
    }
};

} // namespace

JsonValue parseJson(const std::string &text) {
    if (text.empty()) return {};
    JsonParser parser(text.data(), text.data() + text.size());
    return parser.parse();
}

JsonValue parseJson(const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) return {};
    JsonParser parser(reinterpret_cast<const char *>(bytes.data()),
                      reinterpret_cast<const char *>(bytes.data() + bytes.size()));
    return parser.parse();
}

} // namespace globecore
