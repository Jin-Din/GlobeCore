#include "tileset3d/B3dmDecoder.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include <cstring>

namespace globecore {
namespace {

// @Jin 3D Tiles 头字段均为小端 uint32；当前 Android/Harmony 目标均为小端，仍用 memcpy 避免对齐问题。
uint32_t readU32LE(const uint8_t *p) {
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// @Jin Feature Table JSON 可能带空格或 NUL 填充，解析 RTC_CENTER 前先收敛成普通字符串。
std::string trimJsonPadding(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                   text[begin] == '\r' || text[begin] == '\n' ||
                                   text[begin] == '\0')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n' ||
                           text[end - 1] == '\0')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

B3dmContent decodeB3dm(const std::vector<uint8_t> &bytes) {
    B3dmContent result;

    // @Jin b3dm header 固定 28 字节：magic/version/byteLength + 四个 table 长度。
    constexpr size_t kHeaderSize = 28;
    if (bytes.size() < kHeaderSize) {
        result.error = "b3dm 数据不足 28 字节";
        return result;
    }
    if (std::memcmp(bytes.data(), "b3dm", 4) != 0) {
        result.error = "非 b3dm 格式（magic 不匹配）";
        return result;
    }

    const uint32_t version = readU32LE(bytes.data() + 4);
    const uint32_t declaredLength = readU32LE(bytes.data() + 8);
    const uint32_t ftJsonLength = readU32LE(bytes.data() + 12);
    const uint32_t ftBinLength = readU32LE(bytes.data() + 16);
    const uint32_t btJsonLength = readU32LE(bytes.data() + 20);
    const uint32_t btBinLength = readU32LE(bytes.data() + 24);
    if (version != 1U) {
        result.error = "仅支持 b3dm version=1";
        return result;
    }
    if (declaredLength > bytes.size()) {
        result.error = "b3dm 声明长度超过实际字节数";
        return result;
    }

    const size_t glbOffset = static_cast<size_t>(kHeaderSize) + ftJsonLength + ftBinLength +
                             btJsonLength + btBinLength;
    if (glbOffset >= bytes.size() || bytes.size() - glbOffset < 12U) {
        result.error = "b3dm 内嵌 glb 区域长度非法";
        return result;
    }

    // @Jin 按规范四个 table 均按 4 字节对齐；这里直接使用 header 给出的精确长度切分。
    if (ftJsonLength > 0) {
        result.featureTableJson.assign(reinterpret_cast<const char *>(bytes.data() + kHeaderSize),
                                       ftJsonLength);
        result.featureTableJson = trimJsonPadding(result.featureTableJson);
        const JsonValue ft = parseJson(result.featureTableJson);
        if (ft.isObject()) {
            const JsonValue &rtc = ft.at("RTC_CENTER");
            if (rtc.isArray() && rtc.arr.size() >= 3 &&
                rtc.arr[0].isNumber() && rtc.arr[1].isNumber() && rtc.arr[2].isNumber()) {
                result.hasRtcCenter = true;
                result.rtcCenter[0] = rtc.arr[0].numVal;
                result.rtcCenter[1] = rtc.arr[1].numVal;
                result.rtcCenter[2] = rtc.arr[2].numVal;
            }
        }
    }
    if (btJsonLength > 0) {
        const size_t btJsonOffset = static_cast<size_t>(kHeaderSize) + ftJsonLength + ftBinLength;
        result.batchTableJson.assign(reinterpret_cast<const char *>(bytes.data() + btJsonOffset),
                                     btJsonLength);
        result.batchTableJson = trimJsonPadding(result.batchTableJson);
    }

    // @Jin 只拷贝 GLB 视图，P0 保持解码职责单一；严格按 b3dm 声明长度截断，避免响应附带字节污染 GLB。
    result.glb.assign(bytes.begin() + static_cast<std::ptrdiff_t>(glbOffset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(declaredLength));
    if (std::memcmp(result.glb.data(), "glTF", 4) != 0) {
        result.error = "b3dm 内嵌内容不是 glTF/glb";
        result.glb.clear();
        return result;
    }

    result.ok = true;
    LOGI("b3dm decoded: glbBytes=%zu rtc=%d ftJson=%u btJson=%u",
         result.glb.size(), result.hasRtcCenter ? 1 : 0, ftJsonLength, btJsonLength);
    return result;
}

} // namespace globecore

