#include "raster/RasterRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "gdal/cpl_string.h"
#include "gdal/cpl_vsi.h"
#include "gdal/gdal_alg.h"
#include "gdal/gdal_priv.h"
#include "gdal/gdalwarper.h"
#include "gdal/ogr_spatialref.h"

#include <zlib.h>

#include "util/Log.h"
#include "vector/GdalBootstrap.h"
// ESRI 退化 LOCAL_CS 坐标系重建（与 bridge/layer_info.cpp 四至路共用同一口径，见该头注释）
#include "raster/RasterSrsRepair.h"

namespace globecore {

// ==================== 栅格瓦片渲染（承袭原 app jni_raster.cpp，输出 PNG 内存字节而非落盘） ====================
//
// 与原主界面 RasterEx 的差异：原界面把瓦片写 PNG 文件供 参考实现 ImageSource 加载；此处直接产出 PNG 字节，
// 交 TileLoader 走既有「就绪字节 → GL 线程 ImageDecoder 解码上传」链路，并按 <cacheDir>/<z>/<x>_<y>.tile 回写缓存。

namespace {

// ReadTiffAsciiTag / SrsFromProj4 / TryRepairLocalCsSrs 已提取到 raster/RasterSrsRepair.h
// （globecore 命名空间），供本文件与 bridge/layer_info.cpp 共用，避免两条四至路各自演化。

/// 计算栅格数据集的 WGS84 四至：沿边界采样（覆盖旋转地理变换与投影弯曲），投影坐标系经坐标变换
/// 重投影到 WGS84 后取极值（与矢量四至同策略，避免米制包络被当经纬度使用）。
/// [failReason] 非空时失败写出可展示中文原因（无地理参考/无法重投影/四至无效）。
bool RasterWgs84Extent(GDALDataset *ds, double &minLon, double &minLat, double &maxLon, double &maxLat,
                       std::string *failReason, const std::string &tifPath) {
    double gt[6];
    if (ds->GetGeoTransform(gt) != CE_None) {
        if (failReason != nullptr) *failReason = "无地理参考（缺少 GeoTransform，非地理配准影像）";
        return false;
    }
    const int w = ds->GetRasterXSize();
    const int h = ds->GetRasterYSize();
    if (w <= 0 || h <= 0) {
        if (failReason != nullptr) *failReason = "栅格尺寸无效";
        return false;
    }

    const int STEPS = 16;
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(static_cast<size_t>(STEPS + 1) * 4);
    ys.reserve(static_cast<size_t>(STEPS + 1) * 4);
    bool didReproject = false;
    auto addPoint = [&](double px, double py) {
        xs.push_back(gt[0] + px * gt[1] + py * gt[2]);
        ys.push_back(gt[3] + px * gt[4] + py * gt[5]);
    };
    for (int i = 0; i <= STEPS; i++) {
        const double t = static_cast<double>(i) / STEPS;
        addPoint(t * w, 0.0);
        addPoint(t * w, static_cast<double>(h));
        addPoint(0.0, t * h);
        addPoint(static_cast<double>(w), t * h);
    }

    const OGRSpatialReference *srcSrs = ds->GetSpatialRef();
    // ESRI 退化 LOCAL_CS（无椭球、无法建坐标变换）：从文件 ESRI PE 字符串重建后再参与判定
    OGRSpatialReference *repaired = TryRepairLocalCsSrs(srcSrs, tifPath);
    if (repaired != nullptr) srcSrs = repaired;
    if (srcSrs != nullptr && !srcSrs->IsGeographic()) {
        OGRSpatialReference *src = srcSrs->Clone();
        src->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRSpatialReference dst;
        dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dst.SetWellKnownGeogCS("WGS84");
        OGRCoordinateTransformation *ct = OGRCreateCoordinateTransformation(src, &dst);
        src->Release();
        if (ct == nullptr) {
            char *srcWkt = nullptr;
            srcSrs->exportToWkt(&srcWkt);
            LOGE("RasterWgs84Extent: 无法创建坐标变换 srcSrs=%s", srcWkt != nullptr ? srcWkt : "(null)");
            CPLFree(srcWkt);
            if (failReason != nullptr) *failReason = "坐标系无法重投影到 WGS84（投影定义缺失或 proj 配置异常）";
            if (repaired != nullptr) repaired->Release();
            return false;
        }
        const bool ok = ct->Transform(xs.size(), xs.data(), ys.data());
        OGRCoordinateTransformation::DestroyCT(ct);
        if (!ok) {
            if (failReason != nullptr) *failReason = "坐标系无法重投影到 WGS84（坐标变换失败）";
            if (repaired != nullptr) repaired->Release();
            return false;
        }
        didReproject = true;
    }

    minLon = 180.0; maxLon = -180.0; minLat = 90.0; maxLat = -90.0;
    for (size_t i = 0; i < xs.size(); i++) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) continue;
        minLon = std::min(minLon, xs[i]);
        maxLon = std::max(maxLon, xs[i]);
        minLat = std::min(minLat, ys[i]);
        maxLat = std::max(maxLat, ys[i]);
    }
    // 投影退化护栏（真机症状「非 84 数据加载位置错误」的机制性拦截）：投影源经“成功”的
    // 坐标变换后坐标仍落在经纬度域外——典型如 ESRI LOCAL_CS 缺基准信息时变换退化为恒等，
    // 米制值被直接当度使用。此时宁可明确报错，也不钳截包络后把影像渲染到错误位置。
    if (didReproject && !IsPlausibleLonLat(minLon, minLat, maxLon, maxLat)) {
        if (failReason != nullptr) *failReason = "投影坐标变换后未得到经纬度（源坐标系定义无效或缺失，如 ESRI LOCAL_CS 无基准）";
        if (repaired != nullptr) repaired->Release();
        return false;
    }
    minLon = std::max(-180.0, minLon);
    maxLon = std::min(180.0, maxLon);
    minLat = std::max(-90.0, minLat);
    maxLat = std::min(90.0, maxLat);
    if (!(minLon < maxLon && minLat < maxLat)) {
        if (failReason != nullptr) *failReason = "四至无效（地理变换退化或范围落在有效经纬度之外）";
        if (repaired != nullptr) repaired->Release();
        return false;
    }
    if (repaired != nullptr) repaired->Release();
    return true;
}

/// 构造 WGS84 的 WKT（传统经纬度轴序）
bool BuildWgs84Wkt(std::string &out) {
    OGRSpatialReference wgs;
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    wgs.SetWellKnownGeogCS("WGS84");
    char *wktRaw = nullptr;
    if (wgs.exportToWkt(&wktRaw) != OGRERR_NONE || wktRaw == nullptr) return false;
    out = wktRaw;
    CPLFree(wktRaw);
    return true;
}

// 数据集 / 重投影 VRT 缓存（瓦片提速）：按路径缓存打开的数据集 + WGS84 VRT，同一栅格全部瓦片共用。
// GDAL 数据集非线程安全，全局互斥锁串行化访问（瓦片生成本为 CPU 密集，竞争低）。
struct RasterEntry {
    GDALDataset *ds = nullptr;
    GDALDataset *warped = nullptr;
    double wgt[6] = {0, 0, 0, 0, 0, 0};
    int bandCount = 0;
    bool palette = false;
    int alphaBand = -1;
    bool envelopeOk = false;
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    bool ready = false;
    // 源坐标系显式覆盖 WKT（供 AutoCreateWarpedVRT 显式源定义；空=用 ds 自带）。两类场景需要：
    // ① ESRI 退化 LOCAL_CS 重建后的投影定义（ds 自带定义建不出变换）；
    // ② 无 SRS 源按「度」口径显式声明为 WGS84，建恒等 VRT。
    // 注：曾有「WGS84 源免 VRT 整幅直读 ds」分流（nativeGeographic），因整幅级源窗口在 GDAL 内
    // 疑似 32 位线性像素寻址溢出（宽×高×波段 > INT_MAX）导致大图 GL 路 SIGSEGV，已整体下线——
    // 全部读带统一走 warped VRT（warp 引擎分块取源、缓冲有界，无整幅线性索引）。
    std::string srcOverrideWkt;
    std::string failReason; // envelopeOk/ready 不成立时的可展示中文原因（供 info/renderFullImage 上抛）
};

std::mutex g_rasterMutex;
std::map<std::string, RasterEntry *> g_rasterCache;
std::vector<std::string> g_rasterOrder; // LRU：头部最旧，尾部最新
const size_t MAX_RASTER_CACHE = 4;

/// 单瓦片 PNG 上限（防异常大文件撑爆内存）：256×256 RGBA 的 PNG 远小于此
const size_t MAX_PNG_BYTES = 64ULL * 1024 * 1024;

/// RGBA 四平面 → 标准 PNG 字节（RGBA8、非隔行、逐行 filter=None、zlib deflate）。
/// 工程内捆绑的静态 GDAL 无 "PNG" 驱动（真机 GetDriverByName("PNG") 为空，见
/// 「renderTile 缺驱动」日志），瓦片即时生成路不能用 MEM+CreateCopy 编码，
/// 改以同进程已链接的 libz 手写编码；产物与既有 ImageDecoder 解码链路完全兼容。
bool EncodePngRgbaPlanes(int width, int height,
                         const uint8_t *red, const uint8_t *green,
                         const uint8_t *blue, const uint8_t *alpha,
                         std::vector<uint8_t> &out) {
    if (width <= 0 || height <= 0) return false;
    const size_t rawLine = 1 + static_cast<size_t>(width) * 4;
    const size_t rawLen = rawLine * static_cast<size_t>(height);
    if (rawLen > 512ULL * 1024 * 1024) return false; // ~512MB 原始域护栏（远超瓦片所需）
    std::vector<uint8_t> raw(rawLen);
    for (int y = 0; y < height; ++y) {
        uint8_t *line = raw.data() + static_cast<size_t>(y) * rawLine;
        line[0] = 0; // filter type: None
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            uint8_t *p = line + 1 + static_cast<size_t>(x) * 4;
            p[0] = red[i];
            p[1] = green[i];
            p[2] = blue[i];
            p[3] = alpha[i];
        }
    }
    uLongf compLen = compressBound(static_cast<uLong>(rawLen));
    std::vector<uint8_t> comp(compLen);
    if (compress(comp.data(), &compLen, raw.data(), static_cast<uLong>(rawLen)) != Z_OK) return false;
    comp.resize(compLen);

    auto putBe32 = [](uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    };
    out.clear();
    out.insert(out.end(), {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    auto putChunk = [&out, &putBe32](const char type[4], const uint8_t *data, uint32_t len) {
        uint8_t head[4];
        putBe32(head, len);
        out.insert(out.end(), head, head + 4);
        const size_t crcStart = out.size();
        out.insert(out.end(), type, type + 4);
        if (len > 0) out.insert(out.end(), data, data + len);
        const uint32_t c = static_cast<uint32_t>(
            crc32(crc32(0L, Z_NULL, 0), out.data() + crcStart, static_cast<uInt>(4 + len)));
        uint8_t crc[4];
        putBe32(crc, c);
        out.insert(out.end(), crc, crc + 4);
    };
    uint8_t ihdr[13] = {};
    putBe32(ihdr, static_cast<uint32_t>(width));
    putBe32(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type: RGBA
    putChunk("IHDR", ihdr, 13);
    putChunk("IDAT", comp.data(), static_cast<uint32_t>(comp.size()));
    putChunk("IEND", nullptr, 0);
    return !out.empty() && out.size() <= MAX_PNG_BYTES;
}

void FreeRasterEntry(RasterEntry *e) {
    if (e->warped != nullptr) GDALClose(e->warped);
    if (e->ds != nullptr) GDALClose(e->ds);
    delete e;
}

/// 持锁调用：取或创建缓存条目（含打开数据集、四至、重投影 VRT、波段属性解析）。
/// 本函数绝不做建塔等分钟级重操作（全局锁串行化全部栅格访问，持锁重 IO 会饿死所有层瓦片）。
/// 金字塔策略（第五轮回退定稿）：不在设备端建塔——持锁/放锁建塔均引发过真机回归（瓦片饿死
/// 空白、写坏源文件），根治方案回到 PC 端 gdaladdo 预建外部 .ovr 随数据分发；无塔大源（含 WGS84
/// 原生源，读带已统一走 VRT）的低级别大窗口瓦片维持既有降级链（读带失败 → failedUntil_ 30s 冷却重试）。
RasterEntry *GetRasterEntryLocked(const std::string &path) {
    ensureGdalRegistered();

    auto it = g_rasterCache.find(path);
    if (it != g_rasterCache.end()) {
        auto &order = g_rasterOrder;
        order.erase(std::remove(order.begin(), order.end(), path), order.end());
        order.push_back(path);
        return it->second;
    }

    while (g_rasterCache.size() >= MAX_RASTER_CACHE && !g_rasterOrder.empty()) {
        const std::string &oldest = g_rasterOrder.front();
        auto oldIt = g_rasterCache.find(oldest);
        if (oldIt != g_rasterCache.end()) {
            FreeRasterEntry(oldIt->second);
            g_rasterCache.erase(oldIt);
        }
        g_rasterOrder.erase(g_rasterOrder.begin());
    }

    RasterEntry *e = new RasterEntry();
    // 只读打开（第五轮回退定稿：设备端不建塔，UPDATE 打开徒增 .aux.xml 副作用与只读介质失败面）
    e->ds = (GDALDataset *) GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (e->ds == nullptr) {
        LOGW("GetRasterEntry: GDALOpenEx 失败 path=%s", path.c_str());
        delete e;
        return nullptr;
    }
    e->bandCount = e->ds->GetRasterCount();
    e->envelopeOk = RasterWgs84Extent(e->ds, e->minLon, e->minLat, e->maxLon, e->maxLat, &e->failReason, path);

    if (e->envelopeOk) {
        std::string wkt;
        if (BuildWgs84Wkt(wkt)) {
            // 单一口径（本轮定稿）：所有源——含 WGS84 原生经纬度（恒等变换）与无 SRS 按度源——一律
            // 建 WGS84 重投影 VRT，瓦片/全幅读带只经 warped 数据集。此前对 WGS84 源做「免 VRT 整幅
            // 原生直读」快路，整幅级源窗口在 GDAL 内的线性像素寻址于 宽×高×波段 > INT_MAX 时疑似
            // 32 位溢出（28510×32386×3 ≈ 2.77e9，真机 GL 路 SIGSEGV，见 MobileMap-android/doc/
            // 鸿蒙3.0启动闪退.md 第九节）；VRT warp 引擎按分块窗口取源、缓冲有界，故快路整体下线。
            const OGRSpatialReference *rawSrs = e->ds->GetSpatialRef();
            // ESRI 退化 LOCAL_CS：先尝试用文件 ESRI PE 字符串重建源坐标系
            OGRSpatialReference *repaired = TryRepairLocalCsSrs(rawSrs, path);
            const OGRSpatialReference *srcSrs = repaired != nullptr ? repaired : rawSrs;
            if (srcSrs == nullptr) {
                // 与 RasterWgs84Extent 同口径：无 SRS 即按「度」使用——显式声明源为 WGS84 建恒等 VRT
                // （AutoCreateWarpedVRT 省略源定义时，数据集无 SRS 会直接构建失败）
                e->srcOverrideWkt = wkt;
                LOGW("GetRasterEntry: 无 SRS，按经纬度显式声明 WGS84 源定义建 VRT path=%s", path.c_str());
            } else if (repaired != nullptr && !srcSrs->IsGeographic()) {
                // 修复后仍是投影系：VRT 用显式源 WKT（ds 自带的 LOCAL_CS 无法建变换）
                char *rw = nullptr;
                repaired->exportToWkt(&rw);
                if (rw != nullptr) {
                    e->srcOverrideWkt = rw;
                    CPLFree(rw);
                }
                LOGW("GetRasterEntry: LOCAL_CS 修复为投影系，VRT 显式源定义 path=%s", path.c_str());
            }
            if (repaired != nullptr) repaired->Release();

            const char *srcWktArg = e->srcOverrideWkt.empty() ? nullptr : e->srcOverrideWkt.c_str();
            e->warped = (GDALDataset *) GDALAutoCreateWarpedVRT(e->ds, srcWktArg, wkt.c_str(), GRA_Bilinear, 0.0, nullptr);
            if (e->warped != nullptr
                && e->warped->GetGeoTransform(e->wgt) == CE_None
                && e->bandCount >= 1 && e->wgt[1] != 0.0 && e->wgt[5] != 0.0) {
                e->ready = true;
                // 取证日志：加载位置/范围异常时凭源投影名与 VRT 网格原点、分辨率定位识别问题
                // （84 原生源现也在此打印，srcProj=GEOGCS WGS84 即恒等 VRT）
                const OGRSpatialReference *rs = e->ds->GetSpatialRef();
                const char *pn = nullptr;
                if (rs != nullptr) {
                    pn = rs->GetAttrValue("PROJCS");
                    if (pn == nullptr) pn = rs->GetAttrValue("GEOGCS");
                    if (pn == nullptr) pn = rs->GetName();
                }
                LOGI("GetRasterEntry: VRT 就绪 src=%dx%d srcProj=%s wgt=[%.6f,%.8g,%.8g,%.6f,%.8g,%.8g] vrt=%dx%d override=%d path=%s",
                     e->ds->GetRasterXSize(), e->ds->GetRasterYSize(),
                     pn != nullptr ? pn : "?", e->wgt[0], e->wgt[1], e->wgt[2], e->wgt[3], e->wgt[4], e->wgt[5],
                     e->warped->GetRasterXSize(), e->warped->GetRasterYSize(),
                     e->srcOverrideWkt.empty() ? 0 : 1, path.c_str());
            }
            if (!e->ready) e->failReason = "重投影 VRT 构建失败（分辨率退化或驱动限制）";
        } else {
            e->failReason = "重投影 VRT 构建失败（WGS84 定义异常）";
        }
    }

    // 波段属性按「实际读带数据集」解析：ready 即 warped——VRT warp 引擎会把调色板源展开为 RGB
    // 三带（色表已由引擎应用），若仍按 ds 口径判 palette 会对展开后的 R 带二次查表错色；
    // 未 ready 仅为 info 四至服务，退回 ds 口径不影响渲染。
    GDALDataset *propDs = (e->ready && e->warped != nullptr) ? e->warped : e->ds;
    e->bandCount = propDs->GetRasterCount();
    GDALRasterBand *firstBand = propDs->GetRasterBand(1);
    e->palette = firstBand != nullptr && firstBand->GetColorInterpretation() == GCI_PaletteIndex;
    if (e->bandCount >= 4 && propDs->GetRasterBand(4)->GetColorInterpretation() == GCI_AlphaBand) {
        e->alphaBand = 4;
    } else if (e->bandCount == 2 && propDs->GetRasterBand(2)->GetColorInterpretation() == GCI_AlphaBand) {
        e->alphaBand = 2;
    }

    g_rasterCache[path] = e;
    g_rasterOrder.push_back(path);
    return e;
}

/// GDALOpenEx 失败（GetRasterEntryLocked 返回空）的细分原因：文件缺失 vs 驱动/格式问题
void ClassifyOpenFailure(const std::string &path, std::string &outError) {
    VSIStatBufL st;
    outError = (VSIStatL(path.c_str(), &st) == 0)
        ? "GDAL 无法打开该文件（格式不支持、驱动缺失或文件损坏）"
        : "文件不存在或已被移动";
}

/// 持锁调用：把 warped 数据集源窗口 [srcX0,srcY0,srcW,srcH] 重采样读入 dstW×dstH 的 RGBA 四平面，
/// 含调色板展开、alpha 带与 nodata 透明化（renderTile 交集窗口与 renderFullImage 全幅共用一份口径，
/// 避免两处漂移）。失败返回 false（缓冲内容未定义）。
bool ReadRasterRgbaLocked(GDALDataset *ds, const RasterEntry &entry,
                          int srcX0, int srcY0, int srcW, int srcH,
                          int dstW, int dstH,
                          std::vector<uint8_t> &red, std::vector<uint8_t> &green,
                          std::vector<uint8_t> &blue, std::vector<uint8_t> &alpha) {
    // 统一经 warped VRT 读取（网格窗口口径：「度」仿射 wgt）；ds 仅作防御路径的后备数据集
    // （ready 成立已保证 warped 非空，两路窗口口径一致）
    GDALDataset *grid = entry.warped != nullptr ? entry.warped : ds;
    const int bandCount = entry.bandCount;
    const size_t pxCount = static_cast<size_t>(dstW) * dstH;
    red.assign(pxCount, 0);
    green.assign(pxCount, 0);
    blue.assign(pxCount, 0);
    alpha.assign(pxCount, 255);

    auto readBand = [&](int bandIndex, uint8_t *dstBuf) -> bool {
        if (grid == nullptr) return false;
        GDALRasterBand *band = grid->GetRasterBand(bandIndex);
        if (band == nullptr) return false;
        return band->RasterIO(GF_Read, srcX0, srcY0, srcW, srcH, dstBuf, dstW, dstH, GDT_Byte, 0, 0, nullptr) == CE_None;
    };

    const bool palette = entry.palette;
    const int alphaBand = entry.alphaBand;
    uint8_t *tmpR = red.data();
    uint8_t *tmpG = green.data();
    uint8_t *tmpB = blue.data();
    uint8_t *tmpA = alpha.data();

    bool readOk;
    if (palette) {
        readOk = readBand(1, tmpR);
        if (readOk) {
            GDALColorTable *table = grid->GetRasterBand(1)->GetColorTable();
            for (size_t i = 0; i < pxCount; i++) {
                const GDALColorEntry *ce = table != nullptr ? table->GetColorEntry(tmpR[i]) : nullptr;
                if (ce != nullptr) {
                    tmpG[i] = static_cast<uint8_t>(ce->c2);
                    tmpB[i] = static_cast<uint8_t>(ce->c3);
                    tmpA[i] = static_cast<uint8_t>(ce->c4);
                    tmpR[i] = static_cast<uint8_t>(ce->c1);
                } else {
                    tmpA[i] = 0;
                }
            }
        }
    } else if (bandCount == 1 || alphaBand == 2) {
        readOk = readBand(1, tmpR);
        if (readOk) {
            std::memcpy(tmpG, tmpR, pxCount);
            std::memcpy(tmpB, tmpR, pxCount);
            if (alphaBand == 2) readOk = readBand(2, tmpA);
        }
    } else {
        readOk = readBand(1, tmpR) && readBand(2, tmpG) && readBand(3, tmpB);
        if (readOk && alphaBand == 4) readOk = readBand(4, tmpA);
    }
    if (!readOk) return false;

    // nodata 掩膜 → alpha=0（RGB 三波段均定义 nodata 且像素全部命中时置透明；单波段灰度按单波段判定）
    if (!palette) {
        const int checkBands = (bandCount == 1 || alphaBand == 2) ? 1 : std::min(3, bandCount);
        bool hasNoData[3] = {false, false, false};
        double noData[3] = {0, 0, 0};
        int defined = 0;
        for (int b = 0; b < checkBands; b++) {
            int has = FALSE;
            noData[b] = grid->GetRasterBand(b + 1)->GetNoDataValue(&has);
            hasNoData[b] = (has == TRUE) && noData[b] >= 0.0 && noData[b] <= 255.0;
            if (hasNoData[b]) defined++;
        }
        if (defined == checkBands) {
            const auto nd0 = static_cast<uint8_t>(std::lround(noData[0]));
            const auto nd1 = checkBands > 1 ? static_cast<uint8_t>(std::lround(noData[1])) : nd0;
            const auto nd2 = checkBands > 2 ? static_cast<uint8_t>(std::lround(noData[2])) : nd0;
            for (size_t i = 0; i < pxCount; i++) {
                if (tmpR[i] == nd0 && tmpG[i] == nd1 && tmpB[i] == nd2) tmpA[i] = 0;
            }
        }
    }
    return true;
}

} // namespace

bool RasterRenderer::info(const std::string &path, double &outMinLon, double &outMinLat,
                          double &outMaxLon, double &outMaxLat, int &outMaxLevel) {
    std::string ignored;
    return info(path, outMinLon, outMinLat, outMaxLon, outMaxLat, outMaxLevel, ignored);
}

bool RasterRenderer::info(const std::string &path, double &outMinLon, double &outMinLat,
                          double &outMaxLon, double &outMaxLat, int &outMaxLevel,
                          std::string &outError) {
    outError.clear();
    if (path.empty()) {
        outError = "文件路径为空";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_rasterMutex);
    RasterEntry *e = GetRasterEntryLocked(path);
    if (e == nullptr) {
        ClassifyOpenFailure(path, outError);
        return false;
    }
    if (!e->envelopeOk) {
        outError = e->failReason.empty() ? "四至无效" : e->failReason;
        return false;
    }
    outMinLon = e->minLon; outMinLat = e->minLat; outMaxLon = e->maxLon; outMaxLat = e->maxLat;

    // 推荐最大级别：令瓦片分辨率（度/像素 = 360/(2^z·256) = 1.40625/2^z）不劣于栅格重投影分辨率 wgt[1]，
    // 即再细分只会拉伸而无新增细节。z = ceil(log2(1.40625/degPerPx))，钳制到 [1,20]（对齐 Navigator MAX_LEVEL）。
    int maxLevel = 18;
    const double degPerPx = e->ready ? std::fabs(e->wgt[1]) : 0.0;
    if (degPerPx > 1e-12) {
        const double z = std::log2(1.40625 / degPerPx);
        maxLevel = static_cast<int>(std::ceil(z));
    }
    if (maxLevel < 1) maxLevel = 1;
    if (maxLevel > 20) maxLevel = 20;
    outMaxLevel = maxLevel;
    return true;
}

bool RasterRenderer::renderTile(const std::string &path, double minLon, double minLat,
                                double maxLon, double maxLat, int size, std::vector<uint8_t> &outPng) {
    if (path.empty() || size <= 0) return false;
    if (minLon >= maxLon || minLat >= maxLat) {
        LOGW("renderTile 瓦片范围无效 path=%s", path.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(g_rasterMutex);
    RasterEntry *entry = GetRasterEntryLocked(path);
    if (entry == nullptr) {
        LOGW("renderTile 栅格打开失败 path=%s", path.c_str());
        return false;
    }
    if (!entry->ready) {
        LOGW("renderTile 栅格未就绪 path=%s reason=%s", path.c_str(), entry->failReason.c_str());
        return false;
    }

    GDALDataset *ds = entry->ds;
    GDALDataset *grid = entry->warped; // 网格窗口数据集（度仿射 entry->wgt，统一 VRT 口径；ready 已保证非空）
    const double *wgt = entry->wgt;
    const int width = size;
    const int height = size;

    // 瓦片范围与栅格范围求交集；无交集跳过该瓦片。仅渲染交集区域，交集外像素保持 alpha=0（全透明）。
    const double ixMinLon = std::max(minLon, entry->minLon);
    const double ixMaxLon = std::min(maxLon, entry->maxLon);
    const double ixMinLat = std::max(minLat, entry->minLat);
    const double ixMaxLat = std::min(maxLat, entry->maxLat);
    if (ixMinLon >= ixMaxLon || ixMinLat >= ixMaxLat) {
        LOGW("renderTile 无交集: tile=[%.6f,%.6f,%.6f,%.6f] extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
             minLon, minLat, maxLon, maxLat,
             entry->minLon, entry->minLat, entry->maxLon, entry->maxLat, path.c_str());
        return false;
    }

    const size_t pxCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> red(pxCount, 0);
    std::vector<uint8_t> green(pxCount, 0);
    std::vector<uint8_t> blue(pxCount, 0);
    std::vector<uint8_t> alpha(pxCount, 0);

    // 交集 → 瓦片像素窗口（行 0 对应 maxLat）
    const double tileDegW = maxLon - minLon;
    const double tileDegH = maxLat - minLat;
    int dstX0 = static_cast<int>(std::floor((ixMinLon - minLon) / tileDegW * width));
    int dstX1 = static_cast<int>(std::ceil((ixMaxLon - minLon) / tileDegW * width));
    int dstY0 = static_cast<int>(std::floor((maxLat - ixMaxLat) / tileDegH * height));
    int dstY1 = static_cast<int>(std::ceil((maxLat - ixMinLat) / tileDegH * height));
    dstX0 = std::max(0, std::min(dstX0, width));
    dstX1 = std::max(0, std::min(dstX1, width));
    dstY0 = std::max(0, std::min(dstY0, height));
    dstY1 = std::max(0, std::min(dstY1, height));
    const int dstW = dstX1 - dstX0;
    const int dstH = dstY1 - dstY0;
    if (dstW <= 0 || dstH <= 0) {
        LOGW("renderTile 目标窗空: dst=[%d,%d,%d,%d] tile=[%.6f,%.6f,%.6f,%.6f] ix=[%.6f,%.6f,%.6f,%.6f] path=%s",
             dstX0, dstY0, dstW, dstH, minLon, minLat, maxLon, maxLat,
             ixMinLon, ixMinLat, ixMaxLon, ixMaxLat, path.c_str());
        return false;
    }

    // 交集 → 重投影 VRT 像素窗口（VRT 地理变换为 WGS84 度）
    int srcX0 = static_cast<int>(std::floor((ixMinLon - wgt[0]) / wgt[1]));
    int srcX1 = static_cast<int>(std::ceil((ixMaxLon - wgt[0]) / wgt[1]));
    int srcY0 = static_cast<int>(std::floor((wgt[3] - ixMaxLat) / (-wgt[5])));
    int srcY1 = static_cast<int>(std::ceil((wgt[3] - ixMinLat) / (-wgt[5])));
    srcX0 = std::max(0, std::min(srcX0, grid->GetRasterXSize()));
    srcX1 = std::max(0, std::min(srcX1, grid->GetRasterXSize()));
    srcY0 = std::max(0, std::min(srcY0, grid->GetRasterYSize()));
    srcY1 = std::max(0, std::min(srcY1, grid->GetRasterYSize()));
    const int srcW = srcX1 - srcX0;
    const int srcH = srcY1 - srcY0;
    if (srcW <= 0 || srcH <= 0) {
        LOGW("renderTile 源窗口空: srcX=[%d,%d] srcY=[%d,%d] wgt=[%.8g,%.8g,%.8g] extent=[%.6f,%.6f,%.6f,%.6f] tile=[%.6f,%.6f,%.6f,%.6f] path=%s",
             srcX0, srcX1, srcY0, srcY1, wgt[0], wgt[1], wgt[3],
             entry->minLon, entry->minLat, entry->maxLon, entry->maxLat,
             minLon, minLat, maxLon, maxLat, path.c_str());
        return false;
    }

    // 交集窗口重采样 → RGBA 四平面（与全幅路共用同一读带/调色板/nodata 口径）
    std::vector<uint8_t> tmpR;
    std::vector<uint8_t> tmpG;
    std::vector<uint8_t> tmpB;
    std::vector<uint8_t> tmpA;
    if (!ReadRasterRgbaLocked(ds, *entry, srcX0, srcY0, srcW, srcH, dstW, dstH, tmpR, tmpG, tmpB, tmpA)) {
        LOGW("renderTile 读带失败 src=[%d,%d,%d,%d] dst=[%d,%d] grid=warped path=%s",
             srcX0, srcY0, srcW, srcH, dstW, dstH, path.c_str());
        return false;
    }

    // 子窗口拷贝到瓦片输出缓冲（窗口外保持透明）
    for (int y = 0; y < dstH; y++) {
        const size_t srcOff = static_cast<size_t>(y) * dstW;
        const size_t dstOff = static_cast<size_t>(dstY0 + y) * width + dstX0;
        std::memcpy(red.data() + dstOff, tmpR.data() + srcOff, dstW);
        std::memcpy(green.data() + dstOff, tmpG.data() + srcOff, dstW);
        std::memcpy(blue.data() + dstOff, tmpB.data() + srcOff, dstW);
        std::memcpy(alpha.data() + dstOff, tmpA.data() + srcOff, dstW);
    }

    // 四平面交织 → PNG 字节（捆绑 GDAL 无 PNG 驱动，走 libz 手写编码器）
    if (!EncodePngRgbaPlanes(width, height, red.data(), green.data(), blue.data(), alpha.data(), outPng)) {
        LOGW("renderTile PNG 编码失败 w=%d h=%d path=%s", width, height, path.c_str());
        return false;
    }
    return true;
}

// ==================== 全幅 raw RGBA sidecar 缓存（仅 renderFullImage 通路） ====================
//
// 磁盘字节与内存 outRgba 1:1（RGBA8 无压缩），命中 = stat 校验 + 顺序读 w×h×4 字节，零解码、
// 免 GDAL；体积被边长/像素双帽封顶。meta 为完整性标记（最后写），主体按恰 w×h×4 字节校验，
// 中途崩溃只造成 miss 重生成，不会脏命中。

namespace {

/// 源文件指纹（mtime+size），sidecar 失效判据。
bool SidecarSourceStamp(const std::string &srcPath, long long &mtime, long long &size) {
    struct stat st;
    if (stat(srcPath.c_str(), &st) != 0) return false;
    mtime = static_cast<long long>(st.st_mtime);
    size = static_cast<long long>(st.st_size);
    return true;
}

/// 读 meta：`1 <mtime> <size> <minLon> <minLat> <maxLon> <maxLat>`，版本不符/格式错返回 false。
bool SidecarReadMeta(const std::string &metaPath, long long &mtime, long long &size, double env[4]) {
    std::ifstream f(metaPath, std::ios::binary);
    if (!f) return false;
    int version = 0;
    if (!(f >> version >> mtime >> size >> env[0] >> env[1] >> env[2] >> env[3])) return false;
    return version == 1;
}

/// 扫描 [sidecarDir] 寻找与源文件匹配的 sidecar：命中则填尺寸/位图/四至。未命中返回 false（不区分原因）。
bool TryLoadFullSidecar(const std::string &sidecarDir, const std::string &srcPath,
                        int maxSide, long long maxPixels,
                        int &outW, int &outH, std::vector<uint8_t> &outRgba, double outEnv[4]) {
    long long srcMtime = 0, srcSize = 0;
    if (!SidecarSourceStamp(srcPath, srcMtime, srcSize)) return false;
    DIR *dir = opendir(sidecarDir.c_str());
    if (dir == nullptr) return false;
    bool hit = false;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        const std::string nm = de->d_name;
        if (nm.compare(0, 5, "full_") != 0 || nm.size() <= 10 ||
            nm.compare(nm.size() - 5, 5, ".meta") != 0)
            continue;
        int w = 0, h = 0;
        if (std::sscanf(nm.c_str(), "full_%dx%d.rgba.meta", &w, &h) != 2 || w <= 0 || h <= 0)
            continue;
        // 跨设备帽可能不同：超出当前帽即视为未命中，保证回传位图仍可上传纹理
        if (static_cast<long long>(w) > maxSide || static_cast<long long>(h) > maxSide) continue;
        if (static_cast<long long>(w) * h > maxPixels) continue;
        long long mtime = 0, size = 0;
        double metaEnv[4] = {0, 0, 0, 0};
        if (!SidecarReadMeta(sidecarDir + "/" + nm, mtime, size, metaEnv)) continue;
        if (mtime != srcMtime || size != srcSize) continue; // 源文件已改动 → 失效
        const std::string body = sidecarDir + "/" + nm.substr(0, nm.size() - 5);
        struct stat bs;
        if (stat(body.c_str(), &bs) != 0) continue;
        const long long need = static_cast<long long>(w) * h * 4;
        if (static_cast<long long>(bs.st_size) != need) continue; // 残缺主体，拒绝脏命中
        std::ifstream rf(body, std::ios::binary);
        if (!rf) continue;
        std::vector<uint8_t> buf(static_cast<size_t>(need));
        if (!rf.read(reinterpret_cast<char *>(buf.data()), need)) continue;
        outRgba.swap(buf);
        outW = w;
        outH = h;
        std::memcpy(outEnv, metaEnv, sizeof(metaEnv));
        hit = true;
        break;
    }
    closedir(dir);
    return hit;
}

/// 逐级 mkdir（缓存目录通常已由瓦片路建好，此处仅鲁棒兼顾单独全幅路）。
void SidecarMkdirs(const std::string &path) {
    std::string acc;
    for (char c : path) {
        acc += c;
        if (c == '/') mkdir(acc.c_str(), 0755);
    }
    mkdir(acc.c_str(), 0755);
}

/// 回写 sidecar：先写 rgba 主体、后写 meta（完整性标记）；失败仅记日志不影响主流程。
void WriteFullSidecar(const std::string &sidecarDir, const std::string &srcPath,
                      int w, int h, const std::vector<uint8_t> &rgba, const double env[4]) {
    long long mtime = 0, size = 0;
    if (!SidecarSourceStamp(srcPath, mtime, size)) return;
    SidecarMkdirs(sidecarDir);
    char stem[64];
    std::snprintf(stem, sizeof(stem), "full_%dx%d.rgba", w, h);
    const std::string body = sidecarDir + "/" + stem;

    std::ofstream wf(body, std::ios::binary | std::ios::trunc);
    if (!wf) {
        LOGW("sidecar 打开主体失败 %s", body.c_str());
        return;
    }
    wf.write(reinterpret_cast<const char *>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    wf.close();
    if (!wf) {
        LOGW("sidecar 写主体失败 %s", body.c_str());
        std::remove(body.c_str());
        return;
    }

    char metaBuf[192];
    std::snprintf(metaBuf, sizeof(metaBuf), "1 %lld %lld %.9f %.9f %.9f %.9f\n",
                  mtime, size, env[0], env[1], env[2], env[3]);
    std::ofstream mf(body + ".meta", std::ios::binary | std::ios::trunc);
    if (!mf || !(mf << metaBuf) || !mf.flush()) {
        LOGW("sidecar 写 meta 失败 %s.meta", body.c_str());
        std::remove((body + ".meta").c_str());
        return;
    }
    mf.close();

    // 清理尺寸不同的旧 sidecar（换源/换设备帽变化所致；命中路径自会校验，此处仅作磁盘用量控制）
    DIR *dir = opendir(sidecarDir.c_str());
    if (dir != nullptr) {
        struct dirent *de;
        while ((de = readdir(dir)) != nullptr) {
            std::string nm = de->d_name;
            if (nm.compare(0, 5, "full_") != 0) continue;
            std::string base = nm;
            if (base.size() > 5 && base.compare(base.size() - 5, 5, ".meta") == 0)
                base.erase(base.size() - 5);
            if (base == stem) continue;
            std::remove((sidecarDir + "/" + nm).c_str());
        }
        closedir(dir);
    }
}

} // namespace

bool RasterRenderer::renderFullImage(const std::string &path, int maxSide, long long maxPixels,
                                     int &outW, int &outH, std::vector<uint8_t> &outRgba,
                                     double &outMinLon, double &outMinLat,
                                     double &outMaxLon, double &outMaxLat,
                                     std::string &outError,
                                     const std::string &sidecarDir) {
    outError.clear();
    outW = 0; outH = 0;
    outRgba.clear();
    if (path.empty()) {
        outError = "文件路径为空";
        return false;
    }
    if (maxSide < 64) maxSide = 64;      // 下限护栏：至少保证可读回极小缩略
    if (maxPixels < 64 * 64) maxPixels = 64 * 64;

    // sidecar 命中检查置于取锁/GDAL 之前：免锁免解码，二次启动最快路径
    if (!sidecarDir.empty()) {
        double env[4] = {0, 0, 0, 0};
        if (TryLoadFullSidecar(sidecarDir, path, maxSide, maxPixels, outW, outH, outRgba, env)) {
            outMinLon = env[0]; outMinLat = env[1];
            outMaxLon = env[2]; outMaxLat = env[3];
            LOGI("RasterRenderer renderFullImage sidecar 命中 %dx%d path=%s", outW, outH, path.c_str());
            return true;
        }
    }

    std::lock_guard<std::mutex> lock(g_rasterMutex);
    RasterEntry *entry = GetRasterEntryLocked(path);
    if (entry == nullptr) {
        ClassifyOpenFailure(path, outError);
        return false;
    }
    // 全幅路需 VRT 就绪（瓦片路 info 仅需四至）：VRT 失败即读带坐标系不可转换或变换退化
    if (!entry->envelopeOk || !entry->ready) {
        outError = entry->failReason.empty() ? "栅格不可读（见 logcat globecore）" : entry->failReason;
        return false;
    }
    outMinLon = entry->minLon; outMinLat = entry->minLat;
    outMaxLon = entry->maxLon; outMaxLat = entry->maxLat;

    // 网格窗口数据集（度仿射 entry->wgt）：统一走 warped VRT（ready 已保证非空）
    GDALDataset *grid = entry->warped;
    if (grid == nullptr) {
        outError = "栅格数据集不可用（见 logcat globecore）";
        return false;
    }
    const int srcW = grid->GetRasterXSize();
    const int srcH = grid->GetRasterYSize();
    if (srcW <= 0 || srcH <= 0) {
        outError = "栅格尺寸无效";
        return false;
    }

    // 原生分辨率直读；仅超过边长帽/像素帽时等比缩小（GDAL RasterIO 自动沿 overviews 降采样，不放大）
    double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(srcW, srcH));
    scale = std::min(scale, std::sqrt(static_cast<double>(maxPixels) / (static_cast<double>(srcW) * srcH)));
    int dstW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    int dstH = std::max(1, static_cast<int>(std::lround(srcH * scale)));
    // 像素帽为硬约束：sqrt 令积恰等于 maxPixels，再 lround 逐维进位可令 dstW*dstH 略超（如 4899²=24,000,201>24M）。
    // 后果严重：TryLoadFullSidecar 的「w*h>maxPixels 即未命中」判据会把本设备刚回写的 sidecar 永久拒绝，
    // 使全幅位图缓存在任何「抽稀到帽附近」的栅格上彻底失效（冷启动白写、每次重 warp）。逐维回退保证积 ≤ 帽，
    // 既守住内存上限，又令自校验命中。小图（积未超帽）时本循环空转，行为不变。
    while (static_cast<long long>(dstW) * dstH > maxPixels) {
        if (dstW >= dstH) --dstW; else --dstH;
    }

    std::vector<uint8_t> red, green, blue, alpha;
    if (!ReadRasterRgbaLocked(entry->ds, *entry, 0, 0, srcW, srcH, dstW, dstH, red, green, blue, alpha)) {
        outError = "整幅读取失败（GDAL RasterIO）";
        return false;
    }

    const size_t pxCount = static_cast<size_t>(dstW) * dstH;
    outRgba.resize(pxCount * 4);
    for (size_t i = 0; i < pxCount; i++) {
        outRgba[i * 4 + 0] = red[i];
        outRgba[i * 4 + 1] = green[i];
        outRgba[i * 4 + 2] = blue[i];
        outRgba[i * 4 + 3] = alpha[i];
    }
    outW = dstW;
    outH = dstH;
    if (!sidecarDir.empty()) {
        // 回写放在取锁内：与后续同路径请求串行，避免重复写同一 sidecar；写盘量被双帽封顶（百 MB 级内）
        const double env[4] = {outMinLon, outMinLat, outMaxLon, outMaxLat};
        WriteFullSidecar(sidecarDir, path, dstW, dstH, outRgba, env);
    }
    LOGI("RasterRenderer renderFullImage src=%dx%d dst=%dx%d extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
         srcW, srcH, dstW, dstH, outMinLon, outMinLat, outMaxLon, outMaxLat, path.c_str());
    return true;
}

} // namespace globecore
