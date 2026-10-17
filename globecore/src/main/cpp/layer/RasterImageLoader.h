#ifndef GLOBECORE_LAYER_RASTER_IMAGE_LOADER_H
#define GLOBECORE_LAYER_RASTER_IMAGE_LOADER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace globecore {

/**
 * 全幅栅格加载器（镜像 [VectorLoader] 的异步口径：一次性整幅加载、后台线程产出、GL 线程 drain 接管）。
 *
 * 职责：在后台线程经 [RasterRenderer::renderFullImage] 把整个 tif/img 直读为一张 RGBA 位图 +
 * WGS84 四至（原生分辨率，仅在超过边长帽/像素帽时由 GDAL 沿 overviews 降采样），产出 [Data]；
 * GL 线程经 [drainReady] 一次性取走后上传纹理。UI/GL 线程绝不同步触碰 GDAL
 * （区别于瓦片路 addRasterLayer 在调用线程同步读四至——大文件建 VRT 会卡 UI）。
 *
 * 状态机：Idle → (start) Loading → Ready（位图就绪待取）→ (drainReady) Consumed；
 * 读取失败转 Failed，[error] 给出可展示中文原因。所有公开方法线程安全。
 */
class RasterImageLoader {
public:
    /// 一块就绪的全幅位图：RGBA 像素（w×h×4，顶行对应 maxLat）+ WGS84 四至
    struct Data {
        int w = 0;
        int h = 0;
        std::vector<uint8_t> rgba;
        double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    };

    /// [maxSide] 边长帽（一般传 GL_MAX_TEXTURE_SIZE）、[maxPixels] 总像素帽（内存上限护栏）。
    /// [cacheDir] 非空时启用全幅 sidecar 磁盘缓存（见 [RasterRenderer::renderFullImage]）：二次启动
    /// 免 GDAL 直读位图；空串禁用。与瓦片路共用同一目录（按原文件名称命名）。
    RasterImageLoader(std::string path, int maxSide, long long maxPixels, std::string cacheDir = std::string());
    ~RasterImageLoader();

    RasterImageLoader(const RasterImageLoader &) = delete;
    RasterImageLoader &operator=(const RasterImageLoader &) = delete;

    /// 启动后台整幅读取（幂等：非 Idle 状态忽略）。构造后由宿主调用。
    void start();

    /// GL 线程：位图就绪则移动取出（一次性），返回 true 并转 Consumed；否则 false。
    bool drainReady(Data &out);

    /// 是否仍在加载或就绪待上传（供 Renderer 判断是否需再画一帧，驱动 RENDERMODE_WHEN_DIRTY 重绘）。
    bool hasPendingOrReady();

    /// 加载失败原因（Failed 状态时非空，供上层 Toast 提示）。
    std::string error() const;

private:
    enum class State { Idle, Loading, Ready, Failed, Consumed };

    void run();

    const std::string path_;
    const int maxSide_;
    const long long maxPixels_;
    const std::string cacheDir_;

    mutable std::mutex mtx_;
    State state_ = State::Idle;
    Data data_;
    std::string error_;

    std::thread thread_;
    std::atomic<bool> abort_{false};
};

} // namespace globecore

#endif // GLOBECORE_LAYER_RASTER_IMAGE_LOADER_H
