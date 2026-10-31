#ifndef GLOBECORE_LAYER_TILE_LAYER_H
#define GLOBECORE_LAYER_TILE_LAYER_H

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "layer/TileCache.h"
#include "layer/TileLoader.h"

namespace globecore {

/**
 * 一个瓦片图源图层，对应 参考实现的一个 TiledSurfaceImage（各层独立取瓦片/缓存，共享同一相机）。
 *
 * 每层自带磁盘缓存目录（[cache]）与后台加载器（[loader]，含独立线程池、URL 模板），
 * 以及各自的图源最大级别 [maxLevel]（LOD 逐层钳制）。多图源叠加时按加入顺序绘制：
 * 底图（影像）层在前、不透明；[overlay] 为 true 的注记层在后、开启 alpha 混合叠加其上
 * （对齐主界面「底图 -> 注记置顶」的渲染顺序，注记瓦片为带透明的 PNG）。
 *
 * 由 [GlobeEngine] 以 unique_ptr 持有（地址稳定，供 Renderer 长期引用）；Renderer 为每层
 * 维护各自的持久 LRU 纹理缓存，层间纹理互不干扰。
 */
/**
 * @Jin 瓦片图层可选切片方案（feature/epsg-4490，实验、可回退）。
 * 默认构造＝Web 墨卡托（与原 addTileLayer 行为完全一致）；crs 传 EPSG:4326/EPSG:4490 时
 * 启用「地理在线瓦片适配」：引擎仍按墨卡托选瓦，图层在 TileLoader 内把地理服务瓦（根 2×1、
 * 纬度线性）重采样到墨卡托瓦。4490(CGCS2000) 与 4326(WGS84) 椭球差异亚米级，可视化按同一地理切片处理。
 */
struct TileLayerOptions {
    std::string crs;               ///< @Jin ""/"EPSG:3857"=墨卡托；"EPSG:4326"/"EPSG:4490"=地理切片。
    bool tmsY = false;             ///< @Jin 服务行序：true=TMS（y=0 在南），false=XYZ（y=0 在北）。
    int tileMatrixOffset = 0;      ///< @Jin 服务 TILEMATRIX 相对 0 基层级的偏移（天地图 c 根 2×1 记 1，传 1）。
};

struct TileLayer {
    /// 该层的磁盘瓦片缓存（baseDir 形如 `/调查宝/tiles/<图源名>`）
    TileCache cache;
    /// 该层的后台异步加载器（先读 [cache] 未命中再联网）；持有独立线程池，故用 unique_ptr
    std::unique_ptr<TileLoader> loader;
    /// 图源数据最大瓦片级别（LOD 钳制上限，超出由投影拉伸末级瓦片）
    int maxLevel;
    /// 是否为叠加层（注记）：true 则绘制在上层并开启 alpha 混合、缺失瓦片不画占位（保持透明）
    bool overlay;
    /// 图层可见性（对齐 参考实现 Layer.isEnabled）：false 时 Renderer 整层跳过绘制、亦不请求瓦片。
    /// 由宿主经 GlobeEngine::setLayerVisible 翻转（如注记显隐开关），默认 true。
    bool visible = true;
    /// 是否为栅格层（本地 tif/img 经 native GDAL 重投影即时生成瓦片）：区别于底图/注记的「联网或只读缓存」来源。
    /// 栅格层以 overlay=true 建（alpha 混合、缺失瓦片保持透明），但绘制次序须在矢量层之前——Renderer 据本标志
    /// 把栅格层归入「底图同趟」绘制（Pass 1），而非注记 overlay 趟（矢量之后），对齐主界面「底图→栅格→矢量→注记」。
    bool raster = false;
    /// 栅格文件路径（raster=true 时有效）：TileLoader 磁盘未命中时据此调 RasterRenderer 按瓦片四至重投影生成。
    std::string rasterPath;
    /// 图层已被宿主移除（墓碑）：不绘制、不请求瓦片，但槽位保留以维持其它层 index 稳定
    /// （口径同 [VectorLayer::dead]——容器不 erase，宿主持有的索引不失效）。
    bool removed = false;
    /// 纹理待回收标志：置 [removed] 时由调用线程置位，GL 线程在下一帧 exchange 消费后释放本层全部瓦片纹理
    /// （GL 资源只可在 GL 线程删除）。回收后该层转入闲置：其 [loader] 排队项已清空、就绪字节已丢弃。
    std::atomic<bool> discardTextures{false};

    // @Jin: 在线影像/注记两层各 6 个 worker，在天地图限流风险和首屏批量补齐速度之间取平衡。
    TileLayer(std::string cacheDir, std::string urlTemplate, int maxLevel_, bool overlay_, int numThreads = 6)
        : maxLevel(maxLevel_), overlay(overlay_) {
        cache.setBaseDir(std::move(cacheDir));
        loader = std::make_unique<TileLoader>(cache, numThreads);
        loader->setUrlTemplate(std::move(urlTemplate));
    }

    TileLayer(const TileLayer &) = delete;
    TileLayer &operator=(const TileLayer &) = delete;
};

} // namespace globecore

#endif // GLOBECORE_LAYER_TILE_LAYER_H
