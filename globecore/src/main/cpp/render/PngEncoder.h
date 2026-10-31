#ifndef GLOBECORE_RENDER_PNG_ENCODER_H
#define GLOBECORE_RENDER_PNG_ENCODER_H

#include <cstdint>
#include <vector>

namespace globecore {

/**
 * @Jin 轻量 PNG 编码器（feature/epsg-4490，实验、可回退）。
 * @Jin 仅依赖 zlib（全平台/LITE 构建都链接 zstatic），不依赖 GDAL——供 EPSG:4326/4490 地理在线
 * @Jin 瓦片在 TileLoader 内重采样后编码为 PNG，复用既有「瓦片字节→磁盘缓存→ImageDecoder→GL」链路。
 */
class PngEncoder {
public:
    /// 把交错 RGBA8888（w*h*4，顶行=北）编码为 PNG 字节。失败返回 false。线程安全（纯函数）。
    static bool encodeRGBA(int width, int height, const uint8_t *rgba, std::vector<uint8_t> &outPng);
};

} // namespace globecore

#endif // GLOBECORE_RENDER_PNG_ENCODER_H
