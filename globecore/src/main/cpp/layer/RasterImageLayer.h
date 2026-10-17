#ifndef GLOBECORE_LAYER_RASTER_IMAGE_LAYER_H
#define GLOBECORE_LAYER_RASTER_IMAGE_LAYER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "layer/RasterImageLoader.h"
#include "render/Texture.h"

namespace globecore {

/**
 * 全幅栅格图层：一个 tif/img 整幅直读为一张 RGBA 位图，贴在其 WGS84 四至范围上
 * （2D 单位四边形贴图 / 3D 沿经纬网格细分贴球，见 Renderer）。
 *
 * 与 [TileLayer]（分瓦逐级呈现）并列的另一种栅格呈现通路，由宿主按图层「全幅/分瓦」开关分流：
 * 独立容器 [GlobeEngine::rasterImageLayers_] 与索引空间（不占 layers_，避免与底图/注记的
 * setLayerVisible 索引混淆）。与瓦片栅格层同口径：加入后不删除，只经可见性开关显隐。
 *
 * 数据流：[loader] 后台整幅读取 → GL 线程首次 drainReady 把位图移入 [rgba]（CPU 常驻，
 * 供上下文重建懒重传，同 VectorLayer iconRgba→iconTex 口径）→ 下一帧上传 [tex] 后可绘。
 */
struct RasterImageLayer {
    /// 栅格源文件路径（tif/img 等 GDAL 可打开格式）
    std::string sourcePath;
    /// 图层可见性：false 时 Renderer 整层跳过绘制、亦不计入加载提示
    bool visible = true;
    /// 后台整幅加载器（读 GDAL 位图 + 四至）；持有独立线程，故用 unique_ptr
    std::unique_ptr<RasterImageLoader> loader;

    // ── 位图数据（loader 接管后长期保留）──
    // 以下 plain 字段：loader→GL 线程 drainReady 单点接管后，rgba/w/h 只读、extent 只读；
    // 接管前仅 GL 线程读写，UI 线程不触碰（仅经 uploading_ 原子标志感知在途上传）。
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    bool geomReady = false;    // 位图已从 loader 接管（未接管则不可绘）
    std::atomic<bool> uploading{false}; // 已接管待上传/上传中：GL 线程写，UI 经 hasRasterImageLoading 读

    Texture tex;               // 全幅位图纹理（仅 GL 线程）；上下文重建后 abandon，据 rgba 懒重传

    explicit RasterImageLayer(std::string path) : sourcePath(std::move(path)) {}

    RasterImageLayer(const RasterImageLayer &) = delete;
    RasterImageLayer &operator=(const RasterImageLayer &) = delete;
};

} // namespace globecore

#endif // GLOBECORE_LAYER_RASTER_IMAGE_LAYER_H
