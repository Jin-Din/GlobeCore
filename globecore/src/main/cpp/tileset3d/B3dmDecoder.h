#ifndef GLOBECORE_TILESET3D_B3DMDECODER_H
#define GLOBECORE_TILESET3D_B3DMDECODER_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * @Jin 3D Tiles 1.0 b3dm 内容解包结果。
 * @Jin P0 阶段只负责剥离 b3dm 容器并提取内嵌 binary glTF，不在这里创建 GL 资源。
 */
struct B3dmContent {
    bool ok = false;
    std::string error;

    // @Jin Feature Table / Batch Table 原文保留，后续要素拾取和属性查询阶段继续使用。
    std::string featureTableJson;
    std::string batchTableJson;

    // @Jin b3dm 内嵌的完整 .glb 字节，可直接交给 GltfDecoder::decodeGlb()。
    std::vector<uint8_t> glb;

    // @Jin RTC_CENTER 来自 Feature Table，用于把模型局部坐标平移到 ECEF 世界坐标。
    bool hasRtcCenter = false;
    double rtcCenter[3] = {0.0, 0.0, 0.0};
};

/**
 * @Jin 解析 b3dm 二进制容器。
 * @Jin 输入通常由 HttpClient 下载得到；函数纯 CPU、无 GL 依赖，可在后台 worker 线程执行。
 */
B3dmContent decodeB3dm(const std::vector<uint8_t> &bytes);

} // namespace globecore

#endif // GLOBECORE_TILESET3D_B3DMDECODER_H
