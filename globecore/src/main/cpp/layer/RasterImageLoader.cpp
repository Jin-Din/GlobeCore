#include "layer/RasterImageLoader.h"

#include <utility>

#include "raster/RasterRenderer.h"
#include "util/Log.h"

namespace globecore {

RasterImageLoader::RasterImageLoader(std::string path, int maxSide, long long maxPixels, std::string cacheDir)
    : path_(std::move(path)), maxSide_(maxSide), maxPixels_(maxPixels), cacheDir_(std::move(cacheDir)) {}

RasterImageLoader::~RasterImageLoader() {
    abort_ = true;
    if (thread_.joinable()) thread_.join();
}

void RasterImageLoader::start() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ != State::Idle) return; // 幂等
    state_ = State::Loading;
    thread_ = std::thread(&RasterImageLoader::run, this);
}

void RasterImageLoader::run() {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    std::string error;
    const bool ok = RasterRenderer::renderFullImage(path_, maxSide_, maxPixels_,
                                                    w, h, rgba, minLon, minLat, maxLon, maxLat, error,
                                                    cacheDir_);
    // 跨 180° 经线拒载：四至经度跨度接近全球即视为跨线（取极值会把互补大半球误算为包络）。
    // 全幅单矩形贴图无法正确表达跨线数据（瓦片路不受影响），本期明确报错而非贴反。
    std::string finalError = error;
    bool finalOk = ok;
    if (finalOk && maxLon - minLon > 350.0) {
        finalOk = false;
        finalError = "数据跨 180° 经线，暂不支持全幅加载（请改用分瓦加载）";
    }

    std::lock_guard<std::mutex> lock(mtx_);
    if (abort_) return; // 宿主已销毁：结果就地丢弃
    if (finalOk) {
        data_.w = w;
        data_.h = h;
        data_.rgba = std::move(rgba);
        data_.minLon = minLon; data_.minLat = minLat; data_.maxLon = maxLon; data_.maxLat = maxLat;
        state_ = State::Ready;
        LOGI("RasterImageLoader ready path=%s %dx%d", path_.c_str(), w, h);
    } else {
        error_ = finalError;
        state_ = State::Failed;
        LOGW("RasterImageLoader failed path=%s error=%s", path_.c_str(), error_.c_str());
    }
}

bool RasterImageLoader::drainReady(Data &out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ != State::Ready) return false;
    out = std::move(data_);
    state_ = State::Consumed;
    return true;
}

bool RasterImageLoader::hasPendingOrReady() {
    std::lock_guard<std::mutex> lock(mtx_);
    return state_ == State::Loading || state_ == State::Ready;
}

std::string RasterImageLoader::error() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return error_;
}

} // namespace globecore
