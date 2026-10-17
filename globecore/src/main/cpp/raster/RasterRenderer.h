#ifndef GLOBECORE_RASTER_RASTER_RENDERER_H
#define GLOBECORE_RASTER_RASTER_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * 栅格瓦片渲染器（native 内建 GDAL 重投影），对应原主界面 app `jni_raster.cpp`（RasterEx）的能力，
 * 下沉进 globecore 使栅格渲染成为跨平台渲染内核自带能力（不再依赖 app 侧 GDAL JNI）。
 *
 * 职责：把任意经纬度四至内的栅格（GeoTIFF/img 等 GDAL 可打开格式）重采样为带透明通道的 PNG 字节，
 * 供 [TileLoader] 在磁盘缓存未命中时按全球墨卡托瓦片 (z,x,y) 的四至即时生成瓦片。
 *
 * 实现要点（复刻已在原主界面验证的 jni_raster.cpp）：
 *  - 按路径 LRU 缓存「已打开数据集 + WGS84 重投影 VRT + 四至 + 波段属性（调色板/alpha）」，避免每瓦片
 *    重复 GDALOpenEx / GDALAutoCreateWarpedVRT（重投影开销最大）；
 *  - GDAL 数据集非线程安全，全局互斥锁串行化访问（瓦片生成本为 CPU 密集，竞争低）；
 *  - 仅渲染瓦片与栅格四至的交集，交集外像素 alpha=0（全透明），范围外不出现不透明黑块；
 *  - 调色板栅格按色表展开 RGB（含 alpha）；nodata 命中像素置透明；
 *  - PNG 编码经 GDAL MEM→PNG CreateCopy 写入 `/vsimem/` 临时文件再读回字节（复用已链接的 GDAL PNG 驱动）。
 */
class RasterRenderer {
public:
    /// 渲染指定经纬度四至 [minLon,minLat,maxLon,maxLat] 的栅格为 size×size 的 PNG 字节。
    /// 无交集 / 打不开 / 编码失败返回 false（[outPng] 不填）。线程安全（内部全局串行化）。
    static bool renderTile(const std::string &path, double minLon, double minLat,
                           double maxLon, double maxLat, int size,
                           std::vector<uint8_t> &outPng);

    /// 读栅格 WGS84 四至，并据重投影分辨率推算推荐的最大瓦片级别 [outMaxLevel]（LOD 上限）。
    /// 打不开或四至无效返回 false。供 GlobeEngine::addRasterLayer 建层时确定 maxLevel。
    static bool info(const std::string &path, double &outMinLon, double &outMinLat,
                     double &outMaxLon, double &outMaxLat, int &outMaxLevel);

    /// 同上，失败时写出可展示的中文原因 [outError]（文件不存在/打不开/无地理参考/无法重投影/四至无效）。
    static bool info(const std::string &path, double &outMinLon, double &outMinLat,
                     double &outMaxLon, double &outMaxLat, int &outMaxLevel,
                     std::string &outError);

    /// 整幅读取栅格为 RGBA 位图（全幅加载通路，供 [RasterImageLoader]）：按原生分辨率直读，
    /// 仅当宽/高超过 [maxSide]（一般传 GL_MAX_TEXTURE_SIZE）或总像素超过 [maxPixels] 时等比缩小——
    /// 缩小经 GDAL RasterIO 自动沿内部 overviews 降采样，不做主动重采样放大。写出四至与像素尺寸，
    /// [outRgba] 为 w×h×4 RGBA（顶行对应 maxLat）。需数据集成功建立 WGS84 重投影 VRT。
    /// 失败返回 false 并写出 [outError]。线程安全（内部全局串行化）。
    ///
    /// [sidecarDir] 非空时启用全幅 sidecar 磁盘缓存（未压缩 raw RGBA，磁盘字节与 [outRgba] 1:1）：
    /// 命中 = 目录内匹配 `full_<w>x<h>.rgba` + 同名 `.meta`（源文件 mtime+size 与四至），校验字节数
    /// 恰为 w×h×4 后直读——完全绕开 GDAL 与任何图像解码，且命中尺寸亦受双帽约束（跨设备帽不同不误命中）；
    /// 未命中则照常经 GDAL 生成，成功后回写 sidecar（源文件被改动即失效重生成）。空串禁用缓存。
    static bool renderFullImage(const std::string &path, int maxSide, long long maxPixels,
                                int &outW, int &outH, std::vector<uint8_t> &outRgba,
                                double &outMinLon, double &outMinLat,
                                double &outMaxLon, double &outMaxLat,
                                std::string &outError,
                                const std::string &sidecarDir = std::string());
};

} // namespace globecore

#endif // GLOBECORE_RASTER_RASTER_RENDERER_H
