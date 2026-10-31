#include "core/GlobeEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "globe/MercatorProjection.h"
#include "globe/Wgs84Globe.h"
#include "net/HttpClient.h"
#include "raster/RasterRenderer.h"
#include "util/Log.h"
#include "vector/GdalBootstrap.h"
#include "vector/VectorReader.h"
#include "vector/VectorSourceResolver.h" // @Jin 在线 GeoJSON 日志脱敏与 URL 识别。

namespace globecore {
namespace {
// @Jin 相机飞行使用平滑 smoothstep：起止速度为零，长距离飞视点不会生硬突跳。
double flightEase(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

// @Jin 标量线性插值；相机各字段口径简单，经纬度另行走最短角度/大圆友好插值。
double lerpDouble(double a, double b, double t) { return a + (b - a) * t; }

// @Jin heading 取最短角方向，避免从 350° 飞到 10° 时反向转一整圈。
double lerpDegreesShortest(double a, double b, double t) {
    double d = b - a;
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return a + d * t;
}
} // namespace

GlobeEngine::GlobeEngine() : renderer_(navigator_, layers_, vectorLayers_, rasterImageLayers_) {
    // 图层由 app 经 addTileLayer 逐个加入（影像底图 + 注记叠加）；相机/图源装配见 MainActivity。
    // 初始化 libcurl（幂等）：链接期强制拉入 curl 符号验证可用性，运行期从 logcat 打印版本自检。
    HttpClient::globalInit();
    // GDAL/OGR 全进程一次引导（幂等）：注册全部矢量驱动并打印版本自检，同时作为链接期强制
    // 拉入 GDAL 符号的锚点——若依赖链未正确链接，加载 .so 时即暴露未定义符号而非等到首次读矢量。
    ensureGdalRegistered();
    LOGI("GlobeEngine created");
}

GlobeEngine::~GlobeEngine() {
    // @Jin 先停 Provider 并等待下载线程退出，确保迟到回调不会再访问即将析构的 Navigator/Renderer。
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    if (terrainProvider_) {
        terrainProvider_->setEnabled(false);
        terrainProvider_.reset();
    }
    LOGI("GlobeEngine destroyed");
}

void GlobeEngine::surfaceCreated() {
    renderer_.onSurfaceCreated();
}

void GlobeEngine::surfaceChanged(int width, int height) {
    navigator_.setViewport(width, height);
    renderer_.onSurfaceChanged(width, height);
}

void GlobeEngine::drawFrame() {
    // @Jin flyTo 必须先于地形跟随和 Renderer：本帧相机确定后，地形 Provider 只围绕最终/当前视野调度。
    const bool cameraFlightWantsFrame = updateCameraFlight();
    // @Jin 地形自动跟随放在帧首检查：native 手势只更新 Navigator，也能在静止后由 GL 线程统一防抖刷新 DEM。
    const bool terrainFollowWantsFrame = updateTerrainAutoFollow();
    // Renderer 返回「是否需再画一帧」（后台仍有瓦片在途/待上传）；省电模式下据此自请求下一帧，
    // 使异步到位的瓦片无需等待手势即可增量补齐；全部就绪后不再回调 → GL 线程停绘省电。
    // 全程持 layersMtx_：整帧对 layers_/vectorLayers_/rasterImageLayers_ 及其对齐缓存 layerTextures_
    // 做下标遍历，须与 UI 线程 add* 的 push_back（可能重分配内部数组）互斥（见 GlobeEngine.h 注释）。
    // renderCallback_（requestRender）非阻塞，锁内调用安全；不嵌套取锁，无死锁路径。
    renderer_.setCameraFlightActive(cameraFlightWantsFrame); // @Jin flyTo 动画帧使用与手势相同的保守 GL 预算。
    std::lock_guard<std::mutex> lock(layersMtx_);
    bool needRedraw = renderer_.onDrawFrame();
    // 处理 2D GPU 拾取请求：UI 线程（pickVector）设 pending→触发本帧→renderPickPass2D→写结果→notify
    {
        bool doPick = false;
        double px = 0.0, py = 0.0;
        {
            std::lock_guard<std::mutex> lk(pickMtx_);
            if (pickPending_ && !pickDone_) { doPick = true; px = pickX_; py = pickY_; }
        }
        if (doPick) {
            std::pair<int, long long> result = renderer_.renderPickPass2D(px, py);
            {
                std::lock_guard<std::mutex> lk(pickMtx_);
                pickResultLayer_ = result.first;
                pickResultFid_ = result.second;
                pickDone_ = true;
            }
            pickCv_.notify_one();
        }
    }
    if ((needRedraw || terrainFollowWantsFrame || cameraFlightWantsFrame) && renderCallback_) renderCallback_();
}

void GlobeEngine::setCamera(const Camera &camera) {
    cancelCameraFlight();
    navigator_.setCamera(camera);
}

void GlobeEngine::flyToCamera(const Camera &camera, double durationMs) {
    if (!(durationMs > 0.0) || navigator_.viewportWidth() <= 0 || navigator_.viewportHeight() <= 0) {
        // @Jin surface 尚未就绪或调用方明确要求即时切换时，保持旧 API 的确定性。
        setCamera(camera);
        if (renderCallback_) renderCallback_();
        return;
    }
    Camera target = camera;
    if (!(target.fieldOfView > 0.0 && target.fieldOfView < 180.0)) {
        target.fieldOfView = Camera{}.fieldOfView;
    }
    {
        std::lock_guard<std::mutex> lk(cameraFlightMtx_);
        cameraFlight_.active = true;
        cameraFlight_.start = navigator_.camera();
        cameraFlight_.target = target;
        cameraFlight_.startTs = std::chrono::steady_clock::now();
        cameraFlight_.duration = std::chrono::duration<double, std::milli>(std::min(durationMs, 10000.0));
    }
    if (renderCallback_) renderCallback_();
}

bool GlobeEngine::updateCameraFlight() {
    Camera next;
    bool finishNow = false;
    {
        std::lock_guard<std::mutex> lk(cameraFlightMtx_);
        if (!cameraFlight_.active) return false;
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cameraFlight_.startTs).count();
        const double t = cameraFlight_.duration.count() > 0.0
            ? elapsed / cameraFlight_.duration.count() : 1.0;
        if (t >= 1.0) {
            next = cameraFlight_.target;
            cameraFlight_.active = false;
            finishNow = true;
        } else {
            const double e = flightEase(t);
            const Camera &a = cameraFlight_.start;
            const Camera &b = cameraFlight_.target;
            // @Jin Demo 视点均在同一半球，线性经纬插值足够平滑；跨日界线的大圆插值留待后续封装。
            next.latitude = lerpDouble(a.latitude, b.latitude, e);
            next.longitude = lerpDouble(a.longitude, b.longitude, e);
            while (next.longitude > 180.0) next.longitude -= 360.0;
            while (next.longitude < -180.0) next.longitude += 360.0;
            next.altitude = lerpDouble(a.altitude, b.altitude, e);
            next.heading = lerpDegreesShortest(a.heading, b.heading, e);
            next.tilt = lerpDouble(a.tilt, b.tilt, e);
            next.roll = lerpDouble(a.roll, b.roll, e);
            next.fieldOfView = lerpDouble(a.fieldOfView, b.fieldOfView, e);
            next.altitudeMode = b.altitudeMode;
        }
    }
    // @Jin 动画插值帧不经过 setCamera，避免每帧 cancelCameraFlight 反向取锁；结束帧精确落到目标相机。
    navigator_.setCamera(next);
    return !finishNow;
}

void GlobeEngine::cancelCameraFlight() {
    std::lock_guard<std::mutex> lk(cameraFlightMtx_);
    cameraFlight_.active = false;
}

Camera GlobeEngine::getCamera() const {
    return navigator_.camera();
}

void GlobeEngine::setViewMode(int mode) {
    cancelCameraFlight(); // @Jin 宿主切换 2D/3D 时取消旧插值，避免飞行动画继续写入上一模式相机。
    navigator_.setViewMode(mode == 1 ? Navigator::ViewMode::MODE_3D : Navigator::ViewMode::MODE_2D);
    // 省电模式（WHEN_DIRTY）下主动请求一帧，使切换即时生效（2D/3D 通路在 Renderer::onDrawFrame 内分发）
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::viewMode() const {
    return navigator_.viewMode() == Navigator::ViewMode::MODE_3D ? 1 : 0;
}

void GlobeEngine::setSunDirection(double x, double y, double z) {
    renderer_.setSunDirection(Vec3(x, y, z));
    // 省电模式（WHEN_DIRTY）下主动请求一帧，使光照变更即时生效（仅 3D 通路消费，2D 无影响）
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::viewportHeight() const {
    return navigator_.viewportHeight();
}

void GlobeEngine::panByPixels(double dxPx, double dyPx) {
    cancelCameraFlight(); // @Jin 用户接管地图时立即停止飞行动画。
    navigator_.panByPixels(dxPx, dyPx);
}

void GlobeEngine::zoomBy(double factor, double focusXpx, double focusYpx) {
    cancelCameraFlight(); // @Jin 双指缩放不应再被旧 flyTo 插值拉回。
    navigator_.zoomBy(factor, focusXpx, focusYpx);
}

void GlobeEngine::rotateHeading(double deltaDeg) {
    cancelCameraFlight(); // @Jin 手势旋转优先于飞行动画。
    navigator_.rotateHeading(deltaDeg);
}

void GlobeEngine::rotateTilt(double deltaDeg) {
    cancelCameraFlight(); // @Jin 手势俯仰优先于飞行动画。
    navigator_.rotateTilt(deltaDeg);
}

void GlobeEngine::addTileLayer(const std::string &cacheDir, const std::string &urlTemplate,
                               int maxLevel, bool overlay) {
    // @Jin 旧 4 参数入口保持原行为：以默认（Web 墨卡托）选项转发到 5 参数版。
    addTileLayer(cacheDir, urlTemplate, maxLevel, overlay, TileLayerOptions{});
}

void GlobeEngine::addTileLayer(const std::string &cacheDir, const std::string &urlTemplate,
                               int maxLevel, bool overlay, const TileLayerOptions &options) {
    // 每层自带 TileCache + TileLoader（独立线程池/URL 模板），按加入顺序绘制（底图在前、注记在后）。
    std::lock_guard<std::mutex> lock(layersMtx_);
    auto layer = std::make_unique<TileLayer>(cacheDir, urlTemplate, maxLevel, overlay);
    const bool geographic = options.crs == "EPSG:4326" || options.crs == "EPSG:4490";
    if (geographic) {
        // @Jin 4326/4490 地理在线瓦片适配（实验、可回退）：原始地理瓦缓存目录独立（-geo-raw），
        // 0 基服务上限＝图源 maxLevel 减去 TILEMATRIX 偏移（天地图 c：18 - 1 = 17，根 2×1 对应 TILEMATRIX=1）。
        const int maxGeoLevel0 = std::max(0, maxLevel - std::max(0, options.tileMatrixOffset));
        layer->loader->setGeographicSource(cacheDir + "-geo-raw", urlTemplate,
                                           maxGeoLevel0, options.tmsY, options.tileMatrixOffset);
    }
    layers_.push_back(std::move(layer));
    LOGI("GlobeEngine addTileLayer maxLevel=%d overlay=%d crs=%s geo=%d", maxLevel, overlay ? 1 : 0,
         options.crs.empty() ? "EPSG:3857" : options.crs.c_str(), geographic ? 1 : 0);
}

void GlobeEngine::setLayerVisible(int index, bool visible) {
    if (index < 0 || static_cast<size_t>(index) >= layers_.size()) return;
    layers_[static_cast<size_t>(index)]->visible = visible;
    LOGI("GlobeEngine setLayerVisible index=%d visible=%d", index, visible ? 1 : 0);
    // 只改了可见性标志（不涉及 GL），省电模式下主动请求一帧使变更立即生效。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::removeTileLayer(int index) {
    if (index < 0 || static_cast<size_t>(index) >= layers_.size()) return;
    TileLayer &layer = *layers_[static_cast<size_t>(index)];
    // 墓碑：置 removed + 隐藏，不 erase 以保持其它瓦片层 index 稳定（与 removeVectorLayer 同口径）。
    // 纹理属 GL 资源，不能在调用线程删除：只置 discardTextures 交 Renderer 下一帧在 GL 线程回收；
    // loader 队列就地清空（本帧起不再为已删层跑 GDAL/联网）；线程保留、挂起零占用。
    layer.visible = false;
    layer.removed = true;
    layer.discardTextures.store(true);
    if (layer.loader) layer.loader->cancelPending();
    LOGI("GlobeEngine removeTileLayer index=%d raster=%d path=%s", index,
         layer.raster ? 1 : 0, layer.rasterPath.c_str());
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::addRasterLayer(const std::string &cacheDir, const std::string &path) {
    // 先读四至 + 据重投影分辨率推算 maxLevel（LOD 上限）：会打开数据集并建重投影 VRT（一次性，入 RasterRenderer
    // 的 LRU 缓存，后续瓦片生成复用）；打不开或四至无效则不建层。与原主界面 GdalRasterLayer（已删除）构造同口径（同步读四至）。
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    int maxLevel = 18;
    std::string error;
    if (!RasterRenderer::info(path, minLon, minLat, maxLon, maxLat, maxLevel, error)) {
        LOGW("GlobeEngine addRasterLayer 无法解析栅格，跳过 path=%s error=%s", path.c_str(), error.c_str());
        {
            std::lock_guard<std::mutex> lock(addErrMtx_);
            lastAddError_ = error;
        }
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(addErrMtx_);
        lastAddError_.clear();
    }
    // 栅格层：overlay=true（alpha 混合、缺失瓦片透明）+ raster=true（Renderer 据此后置于矢量之前绘制）；
    // urlTemplate 空（不联网），瓦片由 TileLoader 调 RasterRenderer 按四至即时生成。numThreads=2（GDAL 全局串行，无需多绪）。
    auto layer = std::make_unique<TileLayer>(cacheDir, "", maxLevel, /*overlay=*/true, /*numThreads=*/2);
    layer->raster = true;
    layer->rasterPath = path;
    // 四至随源路径一并下传：TileLoader 在派发端对无交集瓦片直接剪枝（免空白瓦进 GDAL 空转）
    layer->loader->setRasterSource(path, true, minLon, minLat, maxLon, maxLat);
    // 临界区只包住 push_back+取 index（上方 info() 的 GDAL 打开/建 VRT 耗时大，必须留在锁外免饿死 GL）
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(layersMtx_);
        layers_.push_back(std::move(layer));
        index = static_cast<int>(layers_.size()) - 1;
    }
    LOGI("GlobeEngine addRasterLayer index=%d maxLevel=%d extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
         index, maxLevel, minLon, minLat, maxLon, maxLat, path.c_str());
    if (renderCallback_) renderCallback_();
    return index;
}

std::string GlobeEngine::rasterAddError() const {
    std::lock_guard<std::mutex> lock(addErrMtx_);
    return lastAddError_;
}

int GlobeEngine::addRasterImageLayer(const std::string &cacheDir, const std::string &path) {
    if (path.empty()) {
        std::lock_guard<std::mutex> lock(addErrMtx_);
        lastAddError_ = "文件路径为空";
        return -1;
    }
    auto layer = std::make_unique<RasterImageLayer>(path);
    // 边长帽取当前 GL_MAX_TEXTURE_SIZE（surface 未创建时 Renderer 保守报 2048）；
    // 像素帽 ~24M（RGBA ≈96MB）防 native OOM——超限由 GDAL 沿 overviews 等比降采样，未超则原生分辨率直读。
    // cacheDir 透传给加载器作 sidecar 目录：二次启动命中时免 GDAL 直读位图（容量被双帽封顶）。
    layer->loader = std::make_unique<RasterImageLoader>(path, renderer_.maxTextureSize(),
                                                        24LL * 1000 * 1000, cacheDir);
    layer->loader->start(); // 后台线程整幅读取，UI 线程零 GDAL
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(layersMtx_); // 锁内仅 push_back+取 index（同上口径）
        rasterImageLayers_.push_back(std::move(layer));
        index = static_cast<int>(rasterImageLayers_.size()) - 1;
    }
    LOGI("GlobeEngine addRasterImageLayer index=%d cache=%s path=%s", index,
         cacheDir.empty() ? "off" : cacheDir.c_str(), path.c_str());
    if (renderCallback_) renderCallback_();
    return index;
}

void GlobeEngine::setRasterImageVisible(int index, bool visible) {
    if (index < 0 || static_cast<size_t>(index) >= rasterImageLayers_.size()) return;
    rasterImageLayers_[static_cast<size_t>(index)]->visible = visible;
    LOGI("GlobeEngine setRasterImageVisible index=%d visible=%d", index, visible ? 1 : 0);
    if (renderCallback_) renderCallback_();
}

bool GlobeEngine::hasRasterImageLoading() const {
    // 仅统计可见层（口径同 hasVectorLoading：隐藏层不计入，免隐藏层慢读取长亮提示胶囊）
    for (const auto &rl : rasterImageLayers_) {
        if (!rl->visible) continue;
        if (rl->loader && rl->loader->hasPendingOrReady()) return true;
        if (rl->uploading.load()) return true; // 已接管、位图待/正在上传纹理
    }
    return false;
}

std::string GlobeEngine::rasterImageError(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= rasterImageLayers_.size()) return std::string();
    const RasterImageLayer &rl = *rasterImageLayers_[static_cast<size_t>(index)];
    if (rl.geomReady) return std::string(); // 已成功接管位图（上传/绘制侧自会呈现或下帧重试），不再报错
    return rl.loader ? rl.loader->error() : std::string();
}

int GlobeEngine::addVectorLayer(const std::string &path, const VectorStyle &style,
                                std::vector<uint8_t> iconRgba, int iconW, int iconH,
                                bool hasExtent, double minLon, double minLat,
                                double maxLon, double maxLat, int maxFeatures,
                                const std::string &vectorCacheDir) {
    // 矢量层与瓦片层同口径：以 unique_ptr 持有保证地址稳定，加入后不删除只经可见性开关显隐。
    // @Jin 同一入口同时支持本地路径和实验性 http/https GeoJSON URL；远程源会先落应用私有缓存。
    auto layer = std::make_unique<VectorLayer>(path, style, hasExtent, minLon, minLat, maxLon, maxLat,
                                               maxFeatures, vectorCacheDir);
    // @Jin 只有实验性在线 GeoJSON 源先接入 DEM 贴地烘焙；本地 shp/kml/GeoJSON 保持旧路径，降低本轮回归面。
    layer->remoteVectorSource = VectorSourceResolver::isHttpUrl(path);
    // 点要素图标（可选）：在加入前设定、之后只读，GL 线程懒上传纹理（避开 UI/GL 竞争）。
    if (!iconRgba.empty() && iconW > 0 && iconH > 0) {
        layer->iconRgba = std::move(iconRgba);
        layer->iconW = iconW;
        layer->iconH = iconH;
    }
    layer->loader->start(); // 后台线程读文件 + 重投影 + earcut 三角剖分，就绪后由 GL 线程上传
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(layersMtx_); // 锁内仅 push_back+取 index（同上口径）
        vectorLayers_.push_back(std::move(layer));
        index = static_cast<int>(vectorLayers_.size()) - 1;
    }
    LOGI("GlobeEngine addVectorLayer index=%d hasExtent=%d extent=[%.6f,%.6f,%.6f,%.6f] source=%s remote=%d",
         index, hasExtent ? 1 : 0, minLon, minLat, maxLon, maxLat,
         VectorSourceResolver::redactUrlForLog(path).c_str(),
         VectorSourceResolver::isHttpUrl(path) ? 1 : 0);
    // 新层异步加载中，主动请求一帧启动绘制（加载就绪后 drawVectorLayers 会持续请求重绘直到上传）。
    if (renderCallback_) renderCallback_();
    return index;
}

void GlobeEngine::updateVectorExtent(int index, bool hasExtent, double minLon, double minLat,
                                     double maxLon, double maxLat, int maxFeatures) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    // 仅对存活的文件矢量层重载（内存叠加层 / 已墓碑层忽略）。
    if (vl.isOverlay || vl.dead || !vl.loader) return;
    LOGI("[VecReload] GlobeEngine updateVectorExtent index=%d hasExtent=%d extent=[%.6f,%.6f,%.6f,%.6f]",
         index, hasExtent ? 1 : 0, minLon, minLat, maxLon, maxLat);
    vl.loader->reload(hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
    // 后台重载完成后 drainReady 会驱动重绘；此处先请求一帧使 hasPendingOrReady 生效持续重绘。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setVectorLayerVisible(int index, bool visible) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    vectorLayers_[static_cast<size_t>(index)]->visible = visible;
    LOGI("GlobeEngine setVectorLayerVisible index=%d visible=%d", index, visible ? 1 : 0);
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setVectorMinLevel(int index, int minLevel) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (vl.isOverlay) return; // 内存叠加层不受级别门控（恒为不限）
    if (vl.minDisplayLevel == minLevel) return;
    vl.minDisplayLevel = minLevel;
    LOGI("GlobeEngine setVectorMinLevel index=%d minLevel=%d", index, minLevel);
    // 级别下限变化可能使层显/隐翻转，请求一帧使变更即时生效（与 setVectorLayerVisible 同口径）。
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::currentZoomLevel() const {
    // 显示级别（与 app 界面同源口径）：用户按界面级别设定的 effectiveMinDisplayLevel 经此比较/门控
    return navigator_.displayLevel();
}

int GlobeEngine::currentTileLevel(int maxLevel) const {
    // @Jin 只读返回渲染选择的目标瓦片 LOD；3D 四叉树仍可能按距离/SSE 在其附近混合更粗子树。
    return navigator_.tileLevel(maxLevel);
}

void GlobeEngine::setTileLodOffset(int offset) {
    // @Jin 实验开关直接给最终目标瓦片 LOD 加整数偏移；屏幕密度仍由 setDisplayDensity 按真实 DPR 参与基准计算。
    navigator_.setTileLodBias(offset);
    LOGI("GlobeEngine setTileLodOffset offset=%d", offset);
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setTileLodAlignDisplay(bool align) {
    // @Jin 同级实验只影响瓦片目标 LOD，不改变相机高度和界面地图级别。
    navigator_.setTileLodAlignDisplay(align);
    LOGI("GlobeEngine setTileLodAlignDisplay align=%d", align ? 1 : 0);
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::removeVectorLayer(int index) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    // 墓碑：置 dead + visible=false，不 erase 以保持其它矢量层 index 稳定（与 removeOverlayLayer 同口径）。
    // 其 VBO/图标纹理/CPU 几何由 Renderer 在下一帧于 GL 线程回收；后台加载线程若仍在读取，
    // VectorLoader 随 VectorLayer unique_ptr 释放（当 Renderer 不再引用后由本函数下一轮清理时自然析构）。
    vl.dead = true;
    vl.visible = false;
    LOGI("GlobeEngine removeVectorLayer index=%d", index);
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::vectorLayerCount() const {
    return static_cast<int>(vectorLayers_.size());
}

bool GlobeEngine::hasVectorLoading() const {
    // 后台读/建几何阶段：loader 处于 Loading 或 Ready（已就绪待 GL 线程 drain），逐层查询（自身持锁、线程安全）。
    const int camLevel = navigator_.displayLevel();
    for (const auto &vl : vectorLayers_) {
        if (vl->dead || vl->isOverlay || !vl->loader) continue;
        // 隐藏/级别不足的层一切不计入（与绘制门控同口径）：其后台读取/上传挂起中，
        // 转「有效可见」后续传再计入——否则整层隐藏的慢读取会把加载提示胶囊长亮误弹（真实用户可见层并无进度）。
        if (!vectorLayerShown(*vl, camLevel)) continue;
        if (vl->loader->hasPendingOrReady()) return true;
        // drainReady 接管后转入逐帧预算的分片流式上传（巨层可达数秒）：streamingUpload 标志接续这段，
        // 提示直到全部 chunk 上传完成、新数据真正画出来才收。
        if (vl->streamingUpload.load()) return true;
    }
    return false;
}

namespace {

/// 由已填入 features 的叠加结果计算 WGS84 四至写入 r（供图层原点 RTC 计算）。
/// 无顶点时 hasBBox=false，buildGeometry 会退化到首要素点作为稳定原点。
inline void computeOverlayBBox(VectorReadResult &r) {
    double minLon = 180.0, minLat = 90.0, maxLon = -180.0, maxLat = -90.0;
    bool any = false;
    auto acc = [&](double lon, double lat) {
        // 脏坐标守卫：跳过非有限（NaN/Inf）或超出 WGS84 有效范围的经纬度。
        // 单个坏点若计入，会把 bbox 中心（图层 RTC 原点 originWx/originWy）撑成天文数字；
        // 由于投影经度不钳制（wx=(lon+180)/360）、纬度钳制，干净标注转 float 相对坐标时
        // 仅 X 轴溢出为 ±inf → 整层文字飞出视口消失（图形层自身 bbox 干净则仍正常）。
        if (!std::isfinite(lon) || !std::isfinite(lat)) return;
        if (lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0) return;
        if (lon < minLon) minLon = lon;
        if (lat < minLat) minLat = lat;
        if (lon > maxLon) maxLon = lon;
        if (lat > maxLat) maxLat = lat;
        any = true;
    };
    for (const auto &f : r.features) {
        if (f.type == VectorGeomType::Point) acc(f.lon, f.lat);
        for (const auto &p : f.parts)
            for (size_t i = 0; i + 1 < p.size(); i += 2) acc(p[i], p[i + 1]);
        for (size_t i = 0; i + 1 < f.outer.size(); i += 2) acc(f.outer[i], f.outer[i + 1]);
        for (const auto &h : f.holes)
            for (size_t i = 0; i + 1 < h.size(); i += 2) acc(h[i], h[i + 1]);
    }
    r.hasBBox = any;
    if (any) { r.minLon = minLon; r.minLat = minLat; r.maxLon = maxLon; r.maxLat = maxLat; }
}

} // namespace

int GlobeEngine::addOverlayLayer(const VectorStyle &style,
                                 std::vector<uint8_t> iconRgba, int iconW, int iconH) {
    // 叠加层与文件矢量层同口径：以 unique_ptr 持有保证地址稳定（Renderer 长期引用），加入后不 erase。
    auto layer = std::make_unique<VectorLayer>(style); // 内存模式 loader（不读文件、不起后台线程）
    if (!iconRgba.empty() && iconW > 0 && iconH > 0) {
        layer->iconRgba = std::move(iconRgba);
        layer->iconW = iconW;
        layer->iconH = iconH;
    }
    std::lock_guard<std::mutex> lock(layersMtx_); // 锁内仅 push_back+取 index（同上口径；叠加层随测量/轨迹高频增建，竞争窗口更密）
    vectorLayers_.push_back(std::move(layer));
    const int index = static_cast<int>(vectorLayers_.size()) - 1;
    LOGI("GlobeEngine addOverlayLayer index=%d", index);
    if (renderCallback_) renderCallback_();
    return index;
}

void GlobeEngine::updateOverlayPoints(int index, const std::vector<double> &lonlat,
                                      const std::vector<long long> &fids,
                                      const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || !vl.loader) return;
    if (vl.dead) return; // 叠加层已失效（被 remove/clear）：静默丢弃写入，不写不可呈现的层

    VectorReadResult r;
    r.ok = true;
    const size_t n = lonlat.size() / 2;
    r.features.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        VectorFeatureData f;
        f.type = VectorGeomType::Point;
        f.lon = lonlat[i * 2];
        f.lat = lonlat[i * 2 + 1];
        f.fid = (i < fids.size()) ? fids[i] : static_cast<long long>(i);
        if (i < labels.size()) f.label = labels[i];
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r)); // 调用线程同步建几何 → Ready；GL 线程下一帧 drainReady 上传
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::updateOverlayLines(int index, const std::vector<double> &lonlat,
                                     const std::vector<int> &vertexCounts,
                                     const std::vector<long long> &fids,
                                     const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || !vl.loader) return;
    if (vl.dead) return; // 叠加层已失效（被 remove/clear）：静默丢弃写入，不写不可呈现的层

    VectorReadResult r;
    r.ok = true;
    const size_t totalPairs = lonlat.size() / 2;
    size_t cursor = 0; // lonlat 坐标对游标
    for (size_t fi = 0; fi < vertexCounts.size(); ++fi) {
        const int vc = vertexCounts[fi];
        if (vc < 1) continue;
        if (cursor + static_cast<size_t>(vc) > totalPairs) break; // 数据不足，截断
        VectorFeatureData f;
        f.type = VectorGeomType::Line;
        f.fid = (fi < fids.size()) ? fids[fi] : static_cast<long long>(fi);
        if (fi < labels.size()) f.label = labels[fi];
        std::vector<double> part;
        part.reserve(static_cast<size_t>(vc) * 2);
        for (int k = 0; k < vc; ++k) {
            part.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2]);
            part.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2 + 1]);
        }
        cursor += static_cast<size_t>(vc);
        f.parts.push_back(std::move(part));
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r));
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::updateOverlayPolygons(int index, const std::vector<double> &lonlat,
                                        const std::vector<int> &ringVertexCounts,
                                        const std::vector<int> &ringsPerFeature,
                                        const std::vector<long long> &fids,
                                        const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || !vl.loader) return;
    if (vl.dead) return; // 叠加层已失效（被 remove/clear）：静默丢弃写入，不写不可呈现的层

    VectorReadResult r;
    r.ok = true;
    const size_t totalPairs = lonlat.size() / 2;
    size_t cursor = 0;   // lonlat 坐标对游标
    size_t ringIdx = 0;  // ringVertexCounts 游标
    auto readRing = [&](std::vector<double> &out) -> bool {
        if (ringIdx >= ringVertexCounts.size()) return false;
        const int vc = ringVertexCounts[ringIdx++];
        if (vc < 1 || cursor + static_cast<size_t>(vc) > totalPairs) return false;
        out.reserve(static_cast<size_t>(vc) * 2);
        for (int k = 0; k < vc; ++k) {
            out.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2]);
            out.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2 + 1]);
        }
        // @Jin 多边形环按首尾重合表达闭合；宿主也可只传真实角点（如四边形传 4 点），
        // @Jin 入口统一补回首点，避免 fill 自动闭合而 outline stroke 仍按开放线少一条边。
        if (vc >= 3 && (out[0] != out[out.size() - 2] || out[1] != out[out.size() - 1])) {
            out.push_back(out[0]);
            out.push_back(out[1]);
        }
        cursor += static_cast<size_t>(vc);
        return true;
    };
    for (size_t fi = 0; fi < ringsPerFeature.size(); ++fi) {
        const int ringCount = ringsPerFeature[fi];
        if (ringCount < 1) continue;
        VectorFeatureData f;
        f.type = VectorGeomType::Polygon;
        f.fid = (fi < fids.size()) ? fids[fi] : static_cast<long long>(fi);
        if (fi < labels.size()) f.label = labels[fi];
        if (!readRing(f.outer)) break; // 外环读取失败（数据耗尽）
        for (int hi = 1; hi < ringCount; ++hi) {
            std::vector<double> hole;
            if (!readRing(hole)) break;
            f.holes.push_back(std::move(hole));
        }
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r));
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::removeOverlayLayer(int index) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay) return; // 仅对叠加层生效；文件矢量层用 setVectorLayerVisible 显隐
    vl.dead = true;
    vl.visible = false;
    LOGI("GlobeEngine removeOverlayLayer index=%d", index);
    if (renderCallback_) renderCallback_(); // 触发一帧，让 GL 线程回收其 VBO/纹理/几何
}

void GlobeEngine::setOverlayNoPick(int index, bool noPick) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay) return; // 仅对叠加层生效；文件矢量层恒可拾取
    vl.noPick = noPick;
}

void GlobeEngine::clearOverlayLayers() {
    for (auto &vlp : vectorLayers_) {
        if (vlp->isOverlay) { vlp->dead = true; vlp->visible = false; }
    }
    LOGI("GlobeEngine clearOverlayLayers");
    if (renderCallback_) renderCallback_();
}

namespace {

/// 点到线段最短距离的平方（相对世界坐标平面）
inline double segDistSq(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = (len2 > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    const double cx = ax + t * dx, cy = ay + t * dy;
    const double ex = px - cx, ey = py - cy;
    return ex * ex + ey * ey;
}

/// 射线法判定点是否在环内（coords 为相对坐标点对序列，取 [start,start+count) 点对）
inline bool pointInRing(double px, double py, const std::vector<float> &coords, int start, int count) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const double xi = coords[(start + i) * 2], yi = coords[(start + i) * 2 + 1];
        const double xj = coords[(start + j) * 2], yj = coords[(start + j) * 2 + 1];
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi + 1e-30) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

/// 点击拾取容差策略（像素）：2D/3D 共用单一来源，避免一处调整另一处漂移
/// （本轮 3D 拾取多处 bug 即因复制 2D 后各自演化所致）。返回均为「像素」容差：
/// 2D 再按 worldPerPx 折算到世界坐标，3D 直接在屏幕像素空间判定。
constexpr double kTapSlopPx = 8.0; // 固定点击余量（像素），保证细小要素也可点中
inline double pickPointTolPx(double pointRadiusDp, double density) {
    return pointRadiusDp * density + kTapSlopPx;
}
inline double pickLineTolPx(double lineWidth) {
    return std::max(lineWidth * 0.5, 2.0) + kTapSlopPx;
}

} // namespace

bool GlobeEngine::screenToGeo(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const {
    if (navigator_.viewportWidth() <= 0 || navigator_.viewportHeight() <= 0) return false;
    // 3D：屏幕射线∩椭球反算（指向球外空间时无命中返回 false，不同于 2D 平面恒可反算）
    if (navigator_.viewMode() == Navigator::ViewMode::MODE_3D) {
        return navigator_.screenToGeo3D(sxPx, syPx, outLonDeg, outLatDeg);
    }
    double wx = 0.0, wy = 0.0;
    navigator_.screenToWorld(sxPx, syPx, wx, wy);
    double lon = 0.0, lat = 0.0;
    MercatorProjection::worldToLonLat(wx, wy, lon, lat);
    // 经度环绕到 [-180, 180)、纬度夹取到墨卡托有效范围（与 Navigator::setCenterWorld 同口径）
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    outLonDeg = lon;
    outLatDeg = MercatorProjection::clampLatitude(lat);
    return true;
}

bool GlobeEngine::featureGeometry(int layerIndex, long long fid, int &outType,
                                  std::vector<double> &outLonLat,
                                  std::vector<int> &outRingCounts,
                                  std::vector<int> &outRingsPerFeature) const {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(vectorLayers_.size())) return false;
    const VectorGeometry &g = vectorLayers_[static_cast<size_t>(layerIndex)]->geom;
    outLonLat.clear();
    outRingCounts.clear();
    outRingsPerFeature.clear();

    // 同 fid 多派生的聚合：VectorReader 将 MultiPolygon / MultiLineString / GeometryCollection
    // 展平为多个共享同一 fid 的 PickPrimitive（每个子几何一个）。旧实现命中首个即 break，
    // 选中高亮/量算只回第一块几何 → 飞地类要素仅显一块、面积不完整。
    // 现逐 pp 摊平到同一 lonlat 缓冲：
    //   Point   每 pp 取首点对，多派生依次 push（无 ringCounts/ringsPerFeature）；
    //   Line    逐 range 展平，每段 push 一个顶点数到 ringCounts（宿主拆段同口径）；
    //   Polygon 逐 pp 当独立子面，每 pp 写入一个 ringsPerFeature 项（外环+各洞数）。
    // 类型取首个匹配 pp（异类派生同 fid 极罕见，不混排）；无任何匹配返回 false。
    const double ox = g.originWx, oy = g.originWy;
    auto pushLonLat = [&](float rx, float ry) {
        double lon = 0.0, lat = 0.0;
        MercatorProjection::worldToLonLat(ox + rx, oy + ry, lon, lat);
        while (lon < -180.0) lon += 360.0;
        while (lon >= 180.0) lon -= 360.0;
        outLonLat.push_back(lon);
        outLonLat.push_back(MercatorProjection::clampLatitude(lat));
    };
    int aggType = -1;
    for (const auto &pp : g.pickPrims) {
        if (pp.fid != fid) continue;
        const int t = (pp.type == PickType::Point) ? 0
                    : (pp.type == PickType::Line)  ? 1 : 2;
        if (aggType < 0) aggType = t;
        else if (t != aggType) continue;
        const std::vector<float> &c = pp.coords;
        if (t == 0) { // Point
            if (c.size() >= 2) pushLonLat(c[0], c[1]);
        } else if (t == 1) { // Line
            for (const auto &rg : pp.ranges) {
                const int start = rg.first, count = rg.second;
                for (int i = 0; i < count; ++i) pushLonLat(c[(start + i) * 2], c[(start + i) * 2 + 1]);
                outRingCounts.push_back(count);
            }
        } else { // Polygon：ranges[0] 外环、其余洞环，逐环写入 ringCounts，本 pp 环数写入 ringsPerFeature
            int rings = 0;
            for (const auto &rg : pp.ranges) {
                const int start = rg.first, count = rg.second;
                for (int i = 0; i < count; ++i) pushLonLat(c[(start + i) * 2], c[(start + i) * 2 + 1]);
                outRingCounts.push_back(count);
                ++rings;
            }
            if (rings > 0) outRingsPerFeature.push_back(rings);
        }
    }
    if (aggType < 0) return false;
    outType = aggType;
    return true;
}

bool GlobeEngine::featureRenderColor(int layerIndex, long long fid, unsigned int &outFillArgb,
                                     unsigned int &outLineArgb) const {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(vectorLayers_.size())) return false;
    const VectorGeometry &g = vectorLayers_[static_cast<size_t>(layerIndex)]->geom;
    // [0,1] RGBA → #AARRGGBB 无符号打包（四舍五入 + 鈐位 [0,255]）
    auto pack = [](const float *c) -> unsigned int {
        auto ch = [](float v) -> unsigned int {
            int q = static_cast<int>(v * 255.0f + 0.5f);
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            return static_cast<unsigned int>(q);
        };
        return (ch(c[3]) << 24) | (ch(c[0]) << 16) | (ch(c[1]) << 8) | ch(c[2]);
    };
    for (const auto &pp : g.pickPrims) {
        if (pp.fid != fid) continue;
        outFillArgb = pack(pp.renderFill);
        outLineArgb = pack(pp.renderLine);
        return true;
    }
    return false;
}

bool GlobeEngine::pickVector(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const {
    // 3D：矢量已贴地绘制（P2），命中检测改走屏幕空间投影通路（pickVector3D，与渲染同口径）；
    // 2D 正交下的射线∩椭球反算不适用于平面 RTC 判据，不可复用。
    if (navigator_.viewMode() == Navigator::ViewMode::MODE_3D)
        return pickVector3D(sxPx, syPx, outLayerIndex, outFid);

    // 2D GPU 拾取（PickID color-attachment）：UI 线程设请求→触发 GL 线程下一帧渲染拾取 pass→
    // glReadPixels 解码回 (layerIndex, fid)。超时 100ms 回退 CPU 逐要素命中检测（避免 ANR）。
    // 仅在有 renderCallback_（= requestRender 注入）时走 GPU 路径；否则直接 CPU。
    if (renderCallback_) {
        {
            std::lock_guard<std::mutex> lk(pickMtx_);
            pickPending_ = true;
            pickX_ = sxPx;
            pickY_ = syPx;
            pickDone_ = false;
        }
        renderCallback_();
        {
            std::unique_lock<std::mutex> lk(pickMtx_);
            pickCv_.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return pickDone_; });
        }
        std::lock_guard<std::mutex> lk(pickMtx_);
        pickPending_ = false;
        if (pickDone_ && pickResultLayer_ >= 0) {
            outLayerIndex = pickResultLayer_;
            outFid = pickResultFid_;
            return true;
        }
        // 超时或 GPU 未命中 → CPU 回退（CPU 也覆盖点要素，GPU pass 不画点）
    }
    return pickVectorCpu(sxPx, syPx, outLayerIndex, outFid);
}

bool GlobeEngine::pickVectorCpu(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const {
    // 屏幕点 → 世界坐标（与相机同口径），再转各图层「相对原点」坐标做命中检测
    double wx = 0.0, wy = 0.0;
    navigator_.screenToWorld(sxPx, syPx, wx, wy);

    const int vw = navigator_.viewportWidth();
    const double worldPerPx = (vw > 0) ? (2.0 * navigator_.halfWorldWidth()) / vw : 0.0;
    if (worldPerPx <= 0.0) return false;
    const double density = navigator_.displayDensity();

    // 两阶段判据：pass 0 仅扫动态叠加层（测量/轨迹/拍照/样地），pass 1 仅扫文件矢量层；
    // 各阶段内索引逆序（后加入优先）。叠加层无论何时加入均优先于文件矢量层命中——旧单趟逆序
    // 在相机静止后补加入大数据矢量层（晚于叠加层）时会把矢量层排到叠加层之上，遮蔽测量/拍照点击。
    // 层内点/线优先于面（小目标优先）；noPick 瞬态层（选中/查询高亮）不参与拾取。
    const int camLevel = navigator_.displayLevel();
    for (int pass = 0; pass < 2; ++pass)
    for (int li = static_cast<int>(vectorLayers_.size()) - 1; li >= 0; --li) {
        const VectorLayer &vl = *vectorLayers_[static_cast<size_t>(li)];
        if ((pass == 0) != vl.isOverlay) continue; // 本趟只扫本类别层：pass 0 叠加层、pass 1 文件矢量层
        // 级别不足/隐藏/墓碑层不可拾取（与绘制同进同退，免点到看不见的图斑）；noPick 瞬态层整层跳过
        if (!vectorLayerShown(vl, camLevel) || vl.noPick) continue;
        const VectorGeometry &g = vl.geom;
        if (g.pickPrims.empty()) continue;

        const double relX = wx - g.originWx;
        const double relY = wy - g.originWy;
        const double pointRadWorld = pickPointTolPx(vl.style.pointRadiusDp, density) * worldPerPx;
        const double lineTolWorld = pickLineTolPx(vl.style.lineWidth) * worldPerPx;

        long long pointLineFid = -1;
        long long polygonFid = -1;
        for (int pi = static_cast<int>(g.pickPrims.size()) - 1; pi >= 0; --pi) {
            const PickPrimitive &pp = g.pickPrims[static_cast<size_t>(pi)];
            if (pp.type == PickType::Point) {
                if (pointLineFid >= 0 || pp.coords.size() < 2) continue;
                const double dx = pp.coords[0] - relX, dy = pp.coords[1] - relY;
                if (dx * dx + dy * dy <= pointRadWorld * pointRadWorld) pointLineFid = pp.fid;
            } else if (pp.type == PickType::Line) {
                if (pointLineFid >= 0) continue;
                const double tol2 = lineTolWorld * lineTolWorld;
                for (const auto &rg : pp.ranges) {
                    const int start = rg.first, count = rg.second;
                    for (int i = 0; i + 1 < count; ++i) {
                        const double ax = pp.coords[(start + i) * 2], ay = pp.coords[(start + i) * 2 + 1];
                        const double bx = pp.coords[(start + i + 1) * 2], by = pp.coords[(start + i + 1) * 2 + 1];
                        if (segDistSq(relX, relY, ax, ay, bx, by) <= tol2) { pointLineFid = pp.fid; break; }
                    }
                    if (pointLineFid >= 0) break;
                }
            } else { // Polygon
                if (polygonFid >= 0 || pp.ranges.empty()) continue;
                const auto outer = pp.ranges[0];
                if (!pointInRing(relX, relY, pp.coords, outer.first, outer.second)) continue;
                bool inHole = false;
                for (size_t hi = 1; hi < pp.ranges.size(); ++hi) {
                    const auto hole = pp.ranges[hi];
                    if (pointInRing(relX, relY, pp.coords, hole.first, hole.second)) { inHole = true; break; }
                }
                if (!inHole) polygonFid = pp.fid;
            }
        }
        const long long hit = (pointLineFid >= 0) ? pointLineFid : polygonFid;
        if (hit >= 0) {
            outLayerIndex = li;
            outFid = hit;
            return true;
        }
    }
    return false;
}

bool GlobeEngine::pickVector3D(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const {
    // 1) 眼点过屏幕点击点的射线 ∩ 椭球：未命中（点击指向天空/屏外）不可拾取
    const Navigator::Camera3D cam = navigator_.camera3D();
    if (!cam.valid) return false;
    double tapLon = 0.0, tapLat = 0.0;
    if (!navigator_.screenToGeo3D(sxPx, syPx, tapLon, tapLat)) return false;
    // tapLon/tapLat 仅作 screenToGeo3D 命中守卫（返回 false 即指向天空不可拾取）；后续命中判据
    // 用绝对 ECEF，无需点击点坐标，故不再换算 tapEcef。
    const Wgs84Globe &globe = Wgs84Globe::instance();
    (void) tapLon; (void) tapLat;

    // 2) RTC 投影矩阵（与 Renderer::projectEcef3D 同口径：列主序，顶点 double 减眼后转 float）
    const Matrix4 viewRtc = Matrix4::lookAt(Vec3{0, 0, 0}, cam.center - cam.eye, cam.up);
    const Matrix4 viewProjRtc = Matrix4::multiply(cam.proj, viewRtc);
    const float *m = viewProjRtc.data();
    const double vw = static_cast<double>(navigator_.viewportWidth());
    const double vh = static_cast<double>(navigator_.viewportHeight());
    if (vw <= 0.0 || vh <= 0.0) return false;
    const double density = navigator_.displayDensity();

    // 与渲染端 renderGlobeVectors3D 同口径的贴地抬升（米）：矢量绘制时 shader 沿地心法向抬 uVecAlt
    //（按弦切矢高口径 camAlt²/2e7 钳 [2,500]，见 Renderer kGlobeVecAltMeters），拾取须同样抬升方能与屏幕绘制位置对齐；
    // 否则斜视下命中点系统性偏离。两式须逐字同步（改其一必改另一）。
    const double camAlt = (cam.eye - cam.center).length();
    double vecAlt = camAlt * camAlt / 2.0e7;
    if (vecAlt > 500.0) vecAlt = 500.0; // 同步 Renderer kGlobeVecAltMeters
    if (vecAlt < 2.0) vecAlt = 2.0;

    // 屏幕点击点是否落在投影四至内（含容差 r 像素；屏外顶点仍参与投影计算，与渲染视口裁剪同效）
    auto boxHit = [&](double bx0, double by0, double bx1, double by1, double r) {
        return bx0 - r <= sxPx && sxPx <= bx1 + r && by0 - r <= syPx && syPx <= by1 + r;
    };

    // 候选集：(层类别优先序 0=叠加层 1=文件矢量层, 类型优先序 0=点线 1=面, 眼距, layerIndex, fid)；
    // 与 2D 同口径两阶段优先（叠加层恒先于文件矢量层，见 pickVector 注释），但 3D 跨层不一经命中
    // 即停（斜视下背半球图元可能先遍历到），全量收集后按 (层类别, 类型, 眼距) 三级择优。
    struct Cand { int pass; int kind; double dist; int li; long long fid; };
    std::vector<Cand> cands;

    const int camLevel = navigator_.displayLevel();
    for (int pass = 0; pass < 2; ++pass)
    for (int li = static_cast<int>(vectorLayers_.size()) - 1; li >= 0; --li) {
        const VectorLayer &vl = *vectorLayers_[static_cast<size_t>(li)];
        if ((pass == 0) != vl.isOverlay) continue; // 本趟只扫本类别层：pass 0 叠加层、pass 1 文件矢量层
        // 与绘制同进同退（含隐藏/级别不足/墓碑）；noPick 瞬态层整层跳过
        if (!vectorLayerShown(vl, camLevel) || vl.noPick) continue;
        const VectorGeometry &g = vl.geom;
        if (g.pickPrims.empty()) continue;

        // RTC coords 是「相对图层原点的归一化墨卡托世界偏移」，须与渲染端 worldXYToCartesian 同口径
        // 精确反算：绝对世界 (originWx+rx, originWy+ry) → 经纬度（墨卡托逆）→ ECEF。
        // 旧实现按局部切平面米制线性化（dLon/=cosLat、dLat 漏 *cosLat），与渲染不一致，点离图层
        // 原点稍远即命中位置错位（表现为“点不中”，仅原点附近能命中）。
        const double oWx = g.originWx;
        const double oWy = g.originWy;
        auto rtcToEcef = [&](float rx, float ry, float alt, float mode) {
            double lon = 0.0, lat = 0.0;
            MercatorProjection::worldToLonLat(oWx + static_cast<double>(rx),
                                              oWy + static_cast<double>(ry), lon, lat);
            Vec3 p{0.0, 0.0, 0.0};
            // 与渲染端同口径：mode=1（高程要素）按真实 alt 烘焙 ECEF、不叠加贴地抬升；
            // mode=0（贴地）alt=0 + 沿地心法向抬 vecAlt（与非高程层旧行为一致）。
            const double useAlt = (mode > 0.5f) ? static_cast<double>(alt) : 0.0;
            globe.geographicToCartesian(lon, MercatorProjection::clampLatitude(lat), useAlt, p);
            if (mode <= 0.5f) p = p + p.normalized() * vecAlt;
            return p;
        };
        // 逐点对高程安全取：非高程层 alt/mode 为空 → 退化到 alt=0/mode=0（贴地抬升）。
        auto vertEcef = [&](const PickPrimitive &pp, size_t j) {
            const float a = j < pp.alt.size() ? pp.alt[j] : 0.0f;
            const float md = j < pp.mode.size() ? pp.mode[j] : 0.0f;
            return rtcToEcef(pp.coords[2 * j], pp.coords[2 * j + 1], a, md);
        };
        // ECEF → 屏幕像素；相机背后（clip w≤0）返回 false
        auto projectEcef = [&](const Vec3 &p, double &ox, double &oy) {
            const float x = static_cast<float>(p.x - cam.eye.x);
            const float y = static_cast<float>(p.y - cam.eye.y);
            const float z = static_cast<float>(p.z - cam.eye.z);
            const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
            if (cw <= 1e-4f) return false;
            const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
            const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            ox = (static_cast<double>(cx) / cw * 0.5 + 0.5) * vw;
            oy = (0.5 - static_cast<double>(cy) / cw * 0.5) * vh;
            return true;
        };
        // 顶点可见性（地球遮挡判据）：地表点 P 位于朝向眼点 E 的可见半球 ⇔ 外法向(≈地心径向 P̂)·(E−P) > 0，
        // 即 dot(P,E) > dot(P,P)=|P|²（等号即切点地平线，与瓦片网格深度剔除同效，避免拾到被地球挡住的背面图元）。
        // 阈值必须取「该点自身地心距平方 |P|²」而非固定赤道半径 A²：WGS84 是椭球，纬度 φ≠0 处地面点地心距
        // r<A，用 A² 作阈值会把镜头正下方的可见点也误判为背面——需相机高 alt > (A²−r²)/r（40°N 约 18km）才放过，
        // 故拉近到样地级别（低空）时 3D 拾取全灭（点不中标识）。用 |P|² 判据任意高度/纬度均正确，远半球点仍被剔。
        auto frontSide = [&](const Vec3 &p) {
            return p.dot(cam.eye - p) > 0.0;
        };
        // 图元眼距（近侧候选优先）：逐顶点最小值（顶点少，开销可忽）
        auto minEyeDist = [&](const PickPrimitive &pp) {
            double best = 1e300;
            for (size_t j = 0; j * 2 + 1 < pp.coords.size(); ++j) {
                const double d = (vertEcef(pp, j) - cam.eye).length();
                if (d < best) best = d;
            }
            return best;
        };

        const double pointRadPx = pickPointTolPx(vl.style.pointRadiusDp, density);
        const double lineTolPx = pickLineTolPx(vl.style.lineWidth);

        for (const PickPrimitive &pp : g.pickPrims) {
            if (pp.coords.empty()) continue;
            if (pp.type == PickType::Point) {
                if (pp.coords.size() < 2) continue;
                const Vec3 p = vertEcef(pp, 0);
                if (!frontSide(p)) continue; // 背半球遮挡（与渲染深度剔除同效）
                double px, py;
                if (!projectEcef(p, px, py)) continue;
                const double dx = px - sxPx, dy = py - syPx;
                if (dx * dx + dy * dy <= pointRadPx * pointRadPx)
                    cands.push_back({pass, 0, (p - cam.eye).length(), li, pp.fid});
                continue;
            }
            if (pp.type == PickType::Line) {
                const double tol2 = lineTolPx * lineTolPx;
                bool hit = false;
                for (const auto &rg : pp.ranges) {
                    if (hit) break;
                    const int start = rg.first, count = rg.second;
                    if (count < 2) continue;
                    // 逐段：先四至快速排除（段屏包围盒含点击点±容差才算垂距），再剔被地球遮挡的整段
                    for (int i = 0; i + 1 < count && !hit; ++i) {
                        const Vec3 a = vertEcef(pp, static_cast<size_t>(start + i));
                        const Vec3 b = vertEcef(pp, static_cast<size_t>(start + i + 1));
                        if (!frontSide(a) && !frontSide(b)) continue; // 整段被地球遮挡
                        double ax, ay, bx, by;
                        if (!projectEcef(a, ax, ay) || !projectEcef(b, bx, by)) continue;
                        if (ax > bx) { std::swap(ax, bx); }
                        if (ay > by) { std::swap(ay, by); }
                        if (sxPx < ax - lineTolPx || sxPx > bx + lineTolPx ||
                            syPx < ay - lineTolPx || syPx > by + lineTolPx) continue; // 段四至排除
                        if (segDistSq(sxPx, syPx, ax, ay, bx, by) <= tol2) hit = true;
                    }
                }
                if (hit) cands.push_back({pass, 0, minEyeDist(pp), li, pp.fid});
                continue;
            }
            // Polygon：外环包含 && 各洞不含（屏幕空间 PIP，投影坐标为透视真值）。
            // 单趟逐环：先投影外环累计全图元四至并排除，再逐环（含外环）PIP（环顶点量小，重投可忽）。
            if (pp.ranges.empty()) continue;
            const auto outer = pp.ranges[0];
            if (outer.second < 3) continue;
            const int oStart = outer.first, oCount = outer.second;
            std::vector<float> oscr;
            oscr.reserve(static_cast<size_t>(oCount) * 2);
            double b0x = 1e300, b0y = 1e300, b1x = -1e300, b1y = -1e300;
            bool projOk = true;
            for (int i = 0; i < oCount; ++i) {
                const Vec3 p = vertEcef(pp, static_cast<size_t>(oStart + i));
                double px, py;
                if (!projectEcef(p, px, py)) { projOk = false; break; } // 有顶点在相机背后：整图元弃判
                oscr.push_back(static_cast<float>(px));
                oscr.push_back(static_cast<float>(py));
                if (px < b0x) b0x = px; else if (px > b1x) b1x = px;
                if (py < b0y) b0y = py; else if (py > b1y) b1y = py;
            }
            if (!projOk) continue;
            if (!boxHit(b0x, b0y, b1x, b1y, 0.0)) continue; // 四至排除
            if (!pointInRing(sxPx, syPx, oscr, 0, oCount)) continue;
            bool inHole = false;
            for (size_t hi = 1; hi < pp.ranges.size() && !inHole; ++hi) {
                const int hStart = pp.ranges[hi].first, hCount = pp.ranges[hi].second;
                if (hCount < 3) continue;
                std::vector<float> hscr;
                hscr.reserve(static_cast<size_t>(hCount) * 2);
                bool hOk = true;
                for (int i = 0; i < hCount; ++i) {
                    const Vec3 p = vertEcef(pp, static_cast<size_t>(hStart + i));
                    double px, py;
                    if (!projectEcef(p, px, py)) { hOk = false; break; }
                    hscr.push_back(static_cast<float>(px));
                    hscr.push_back(static_cast<float>(py));
                }
                // 洞投影失败(-)保守不作洞（外环已含，宁可多判命中）
                if (hOk && pointInRing(sxPx, syPx, hscr, 0, hCount)) inHole = true;
            }
            if (!inHole) cands.push_back({pass, 1, minEyeDist(pp), li, pp.fid});
        }
    }

    if (cands.empty()) return false;
    // 择优：叠加层候选恒先于文件矢量层候选（同 2D 两阶段）；再点/线优先于面（小目标优先）；
    // 同类取离眼点最近（斜视下背半球穿地图元让位近侧）
    const Cand *best = &cands[0];
    for (const Cand &c : cands) {
        if (c.pass != best->pass) { if (c.pass < best->pass) best = &c; continue; }
        if (c.kind < best->kind || (c.kind == best->kind && c.dist < best->dist)) best = &c;
    }
    outLayerIndex = best->li;
    outFid = best->fid;
    return true;
}

void GlobeEngine::setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg) {
    renderer_.setLocationMarker(lonDeg, latDeg, visible, headingDeg);
    // 只改了标记状态（不涉及 GL 资源），省电模式下主动请求一帧使其立即生效。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h) {
    renderer_.setLocationMarkerIcon(std::move(rgba), w, h);
    // 图标像素已更新，旧纹理已失效；请求一帧使 GL 线程据新像素重传纹理并画罗盘图标。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setFontPath(const std::string &path) {
    renderer_.setFontPath(path); // FontAtlas::load 幂等、纯 CPU、线程安全（path 空则自动探测）
    // 字体就绪后主动请求一帧，使已有矢量标注立即显示（RENDERMODE_WHEN_DIRTY 下）。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setDisplayDensity(double density) {
    navigator_.setDisplayDensity(density); // LOD densityFactor（对齐 参考实现 displayMetrics.density）
}

void GlobeEngine::setRenderCallback(std::function<void()> cb) {
    renderCallback_ = std::move(cb);
}

void GlobeEngine::releaseGl() {
    // 与 drawFrame 同口径持锁：GL 上下文销毁回收资源时同样遍历层容器，须与 UI 线程 add* 互斥
    std::lock_guard<std::mutex> lock(layersMtx_);
    renderer_.release();
}

void GlobeEngine::setTerrainTile(int z, int x, int y, const std::vector<uint8_t> &bytes,
                                 double lonMin, double latMin, double lonMax, double latMax) {
    // 转发到 Renderer DEM 缓存（须 GL 线程调用，与 buildGlobeTileMesh 同上下文）。
    // 不持 layersMtx_：terrainCache_ 是 Renderer 私有成员，只在 GL 线程访问（与瓦片纹理缓存同口径）。
    renderer_.setTerrainTile(z, x, y, bytes, lonMin, latMin, lonMax, latMax);
    if (renderCallback_) renderCallback_(); // 触发重绘使地形抬升即时生效
}

void GlobeEngine::clearTerrain() {
    // @Jin 外部清空和关闭地形时同步失效 Provider 已交付 LRU，确保下一次刷新会重新注入。
    if (terrainProvider_) terrainProvider_->resetDeliveredTiles();
    renderer_.clearTerrain();
    if (renderCallback_) renderCallback_();
}

bool GlobeEngine::configureTerrainProvider(
    const std::string &baseUrl, const std::string &cacheDir,
    int maxZoom, int concurrency, int tileRadius,
    const std::vector<std::pair<std::string, std::string>> &headers) {
    if (baseUrl.empty() || cacheDir.empty()) return false;

    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    // @Jin 换源时先停旧 Provider 再清空 DEM，避免旧地址的迟到结果混入新地形。
    if (terrainProvider_) {
        terrainProvider_->setEnabled(false);
        terrainProvider_.reset();
    }
    clearTerrain();

    TerrainProviderOptions options;
    options.baseUrl = baseUrl;
    options.cacheDir = cacheDir;
    options.maxZoom = maxZoom;
    options.concurrency = concurrency;
    options.tileRadius = tileRadius;
    options.headers = headers;

    std::function<void(const std::string &)> eventCallback;
    {
        // @Jin 回调跨 Provider 重建保留；新实例创建后立即安装，metadata 预取事件不会丢失。
        std::lock_guard<std::mutex> lock(terrainEventMtx_);
        eventCallback = terrainEventCallback_;
    }
    terrainProvider_ = std::make_unique<CesiumTerrainProvider>(
        std::move(options),
        [this]() -> TerrainViewportSnapshot {
            TerrainViewportSnapshot viewport;
            const Camera camera = navigator_.camera();
            viewport.valid = navigator_.viewportHeight() > 0;
            viewport.viewMode = viewMode();
            viewport.viewportHeight = navigator_.viewportHeight();
            viewport.latitude = camera.latitude;
            viewport.longitude = camera.longitude;
            viewport.altitude = camera.altitude;
            viewport.tilt = camera.tilt;
            viewport.displayZoom = static_cast<double>(currentZoomLevel());
            return viewport;
        },
        [this](int z, int x, int y, const std::vector<uint8_t> &bytes,
               const TerrainGeoBounds &bounds) {
            // @Jin C++ worker 直接入队 Renderer；GL 线程下一帧帧首消费，不再经过 ArkTS 调度。
            setTerrainTile(z, x, y, bytes, bounds.lonMin, bounds.latMin, bounds.lonMax, bounds.latMax);
        });
    terrainProvider_->setEventCallback(std::move(eventCallback));
    terrainProvider_->prefetchMetadata();
    LOGI("GlobeEngine terrain provider configured url=%s cache=%s maxZoom=%d concurrency=%d radius=%d",
         baseUrl.c_str(), cacheDir.c_str(), maxZoom, concurrency, tileRadius);
    return true;
}

bool GlobeEngine::setTerrainProviderEnabled(bool enabled) {
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    if (!terrainProvider_) return false;
    terrainProvider_->setEnabled(enabled);
    // @Jin 开启时 Provider 已立即刷新当前视口；下一帧只记录签名，不重复中断首刷。
    terrainFollowLastSignature_.store(UINT64_MAX);
    terrainFollowReset_.store(false);
    terrainFollowSuppressInit_.store(enabled);
    terrainFollowPendingSinceNs_.store(0);
    if (!enabled) {
        // @Jin Provider 关闭后清空 Renderer 的 DEM 缓存；重新开启时由新视口重新注入。
        clearTerrain();
    }
    return true;
}

void GlobeEngine::refreshTerrainProvider() {
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    if (terrainProvider_) terrainProvider_->requestRefresh();
}

void GlobeEngine::setTerrainProviderAutoFollow(bool enabled) {
    terrainAutoFollow_.store(enabled);
    // @Jin 开启自动跟随时等待下一帧记录当前签名；相机已经稳定时不额外打断正在下载的地形批次。
    terrainFollowReset_.store(false);
    terrainFollowSuppressInit_.store(true);
    terrainFollowLastSignature_.store(UINT64_MAX);
    terrainFollowPendingSinceNs_.store(0);
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::prefetchTerrainProviderMetadata() {
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    if (terrainProvider_) terrainProvider_->prefetchMetadata();
}

std::string GlobeEngine::terrainProviderStatsJson() const {
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    return terrainProvider_ ? terrainProvider_->statsJson() : std::string("{}");
}

bool GlobeEngine::updateTerrainAutoFollow() {
    if (!terrainAutoFollow_.load()) return false;

    TerrainViewportSnapshot viewport;
    {
        // @Jin 仅在取 Provider 指针和计算签名时短持生命周期锁，不阻塞换源/开关太久。
        std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
        if (!terrainProvider_ || !terrainProvider_->enabled()) return false;

        const Camera camera = navigator_.camera();
        viewport.valid = navigator_.viewportHeight() > 0;
        viewport.viewMode = viewMode();
        viewport.viewportHeight = navigator_.viewportHeight();
        viewport.latitude = camera.latitude;
        viewport.longitude = camera.longitude;
        viewport.altitude = camera.altitude;
        viewport.tilt = camera.tilt;
        viewport.displayZoom = static_cast<double>(currentZoomLevel());

        if (!viewport.valid || viewport.viewMode != 1) {
            // @Jin 2D 不消费 DEM；回到 3D 后重新记录签名，保证视口切换会触发一次地形刷新。
            terrainFollowReset_.store(true);
            terrainFollowSuppressInit_.store(false);
            terrainFollowLastSignature_.store(UINT64_MAX);
            terrainFollowPendingSinceNs_.store(0);
            return false;
        }

        const uint64_t signature = terrainProvider_->viewportTaskSignature(viewport);
        const auto now = std::chrono::steady_clock::now();
        uint64_t lastSignature = terrainFollowLastSignature_.load();
        const bool forceRefresh = terrainFollowReset_.exchange(false);
        const bool initialize = lastSignature == UINT64_MAX;
        if (initialize) {
            terrainFollowLastSignature_.store(signature);
            if (terrainFollowSuppressInit_.exchange(false) && !forceRefresh) {
                // @Jin 打开地形/打开自动跟随后的首刷已由 setEnabled 或宿主显式触发，这里只建立基线。
                return false;
            }
            if (!forceRefresh) return false;
        }
        if (forceRefresh || initialize || signature != lastSignature) {
            terrainFollowLastSignature_.store(signature);
            terrainFollowPendingSinceNs_.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
            return true; // @Jin 防抖窗口内持续请求续帧，防止手势结束后 RENDERMODE_WHEN_DIRTY 提前停绘。
        }

        const long long pendingNs = terrainFollowPendingSinceNs_.load();
        if (pendingNs == 0) return false;
        const auto pendingAt =
            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(pendingNs));
        constexpr auto kTerrainFollowDebounce = std::chrono::milliseconds(700);
        if (now < pendingAt + kTerrainFollowDebounce) return true;

        terrainFollowPendingSinceNs_.store(0);
        terrainProvider_->requestRefresh();
    }
    return false;
}

void GlobeEngine::setTerrainProviderEventCallback(std::function<void(const std::string &)> callback) {
    std::function<void(const std::string &)> installed;
    // @Jin 统一生命周期锁→事件锁顺序，避免帧循环/换源与回调替换并发访问 Provider。
    std::lock_guard<std::mutex> terrainLock(terrainProviderMtx_);
    {
        std::lock_guard<std::mutex> lock(terrainEventMtx_);
        terrainEventCallback_ = callback;
        installed = terrainEventCallback_;
    }
    if (terrainProvider_) terrainProvider_->setEventCallback(std::move(installed));
}

} // namespace globecore
