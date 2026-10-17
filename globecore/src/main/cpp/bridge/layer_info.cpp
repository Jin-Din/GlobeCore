// 图层四至 / 字段名 / 坐标系描述 —— 核心实现见 bridge_api.h（双平台共享），
// Android JNI 导出为文末 #ifndef __OHOS__ 薄封装；鸿蒙侧由 napi_layer_info.cpp 调同一核心。
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>

#include "gdal/ogrsf_frmts.h"
#include "gdal/gdal_priv.h"
#include "gdal/cpl_conv.h"

#include "srs_resolve.h"
// ESRI 退化 LOCAL_CS 重建 + 投影四至护栏：与 raster/RasterRenderer.cpp 瓦片渲染路同源口径，
// 避免「瓦片位置正确、缩放到图层却跑到米制坐标处」这类两路漂移。
#include "raster/RasterSrsRepair.h"
#include "bridge_api.h"
#include "util/Log.h"

/**
 * 投影源四至护栏：源坐标系为投影系时，最终四至必须落在有效经纬度域内，否则说明坐标变换
 * 建不起来或退化为恒等（米制值被当度使用）——返回 false，绝不把米制包络交给上层缩放/过滤。
 * 地理坐标系源（含 srs 为空，按既定约定视为经纬度）不拦截。
 */
static bool projectedExtentPlausible(const OGRSpatialReference *srs, const double extent[4]) {
    if (srs == nullptr || srs->IsGeographic()) return true;
    if (globecore::IsPlausibleLonLat(extent[0], extent[1], extent[2], extent[3])) return true;
    LOGE("四至护栏拦截：投影源未得到经纬度 minLon=%.3f minLat=%.3f maxLon=%.3f maxLat=%.3f", extent[0],
         extent[1], extent[2], extent[3]);
    return false;
}

// ==================== 图层四至与字段名（NativeLayerInfo 门面） ====================

/**
 * 图层四至计算（矢量优先，栅格兜底），宿主图层管理与缩放到图层共用。
 */
static bool computeLayerExtent(const char *p, double extent[4]) {
    bool ok = false;

    // 矢量数据：OGR 读取第一个图层的四至（支持 shp、kml，kmz 走 /vsizip/ 前缀）
    GDALDataset *ds = (GDALDataset *) GDALOpenEx(p, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    if (ds != nullptr) {
        OGRLayer *layer = ds->GetLayer(0);
        if (layer != nullptr) {
            OGREnvelope ext;
            if (layer->GetExtent(&ext, TRUE) == OGRERR_NONE) {
                OGRSpatialReference *srs = resolveSourceSrs(p, layer);
                // CAD 无 SRS（无同名 .prj 且坐标不含投影带号）：米制坐标无法定位，直接返回失败，
                // 不返回米制四至（否则上层把米当经纬度缩放/过滤导致视角飞错）；改由加载失败提示补 .prj。
                if (srs == nullptr && srsIsCad(p)) {
                    GDALClose(ds);
                    return false;
                }
                extent[0] = ext.MinX;
                extent[1] = ext.MinY;
                extent[2] = ext.MaxX;
                extent[3] = ext.MaxY;
                ok = true;

                // GetExtent 返回图层源坐标系下的包络；投影坐标系（如高斯-克吕格）下为米制，
                // 需重投影到 WGS84，否则上层把米当经纬度缩放/过滤导致视角与加载错位。
                // 四角全变换后取极值，兼容轴序翻转情形。
                if (srs != nullptr && !srs->IsGeographic()) {
                    OGRSpatialReference dst;
                    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    dst.SetWellKnownGeogCS("WGS84");
                    OGRCoordinateTransformation *ct = OGRCreateCoordinateTransformation(srs, &dst);
                    if (ct != nullptr) {
                        double xs[4] = {ext.MinX, ext.MaxX, ext.MinX, ext.MaxX};
                        double ys[4] = {ext.MinY, ext.MinY, ext.MaxY, ext.MaxY};
                        if (ct->Transform(4, xs, ys)) {
                            extent[0] = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
                            extent[2] = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
                            extent[1] = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
                            extent[3] = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
                        }
                        OGRCoordinateTransformation::DestroyCT(ct);
                    } else {
                        LOGE("四至重投影失败：无法创建坐标变换（proj.db 未配置？）");
                    }
                }
                // 变换缺失/退化时 extent 仍是源坐标系米制包络：宁可不给四至，也不让相机飞错
                if (!projectedExtentPlausible(srs, extent)) {
                    if (srs != nullptr) srs->Release();
                    GDALClose(ds);
                    return false;
                }
                if (srs != nullptr) srs->Release();
            }
        }
        GDALClose(ds);
    }

    // 栅格数据：GeoTransform 计算四至（tif/tiff）
    if (!ok) {
        ds = (GDALDataset *) GDALOpenEx(p, GDAL_OF_RASTER | GDAL_OF_READONLY,
                                        nullptr, nullptr, nullptr);
        if (ds != nullptr) {
            double gt[6];
            if (ds->GetGeoTransform(gt) == CE_None) {
                int w = ds->GetRasterXSize();
                int h = ds->GetRasterYSize();
                // 四角（含旋转项）
                double xs[4] = {gt[0],
                                gt[0] + w * gt[1],
                                gt[0] + h * gt[2],
                                gt[0] + w * gt[1] + h * gt[2]};
                double ys[4] = {gt[3],
                                gt[3] + w * gt[4],
                                gt[3] + h * gt[5],
                                gt[3] + w * gt[4] + h * gt[5]};

                // 投影坐标系（如高斯-克吕格）下 GeoTransform 为米制，
                // 需重投影到 WGS84 再取极值（同矢量分支策略），否则缩放定位错位。
                const OGRSpatialReference *srs = ds->GetSpatialRef();
                // ESRI 退化 LOCAL_CS（无椭球、无法建坐标变换）：从文件 ESRI PE 字符串重建后再参与判定
                // （ArcGIS 导出的 Web Mercator 影像即此情形，与瓦片渲染路同一修复）。
                OGRSpatialReference *repaired = globecore::TryRepairLocalCsSrs(srs, std::string(p));
                if (repaired != nullptr) srs = repaired;
                bool transformed = false;
                if (srs != nullptr && !srs->IsGeographic()) {
                    OGRSpatialReference *src = srs->Clone();
                    src->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    OGRSpatialReference dst;
                    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    dst.SetWellKnownGeogCS("WGS84");
                    OGRCoordinateTransformation *ct = OGRCreateCoordinateTransformation(src, &dst);
                    src->Release();
                    if (ct != nullptr) {
                        transformed = ct->Transform(4, xs, ys);
                        OGRCoordinateTransformation::DestroyCT(ct);
                        if (!transformed) LOGE("栅格四至重投影失败：坐标变换返回失败");
                    } else {
                        LOGE("栅格四至重投影失败：无法创建坐标变换");
                    }
                }

                extent[0] = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
                extent[2] = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
                extent[1] = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
                extent[3] = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
                // 护栏：投影源没拿到经纬度（变换缺失/退化为恒等）时绝不返回米制包络——否则
                // 上层把米当度缩放，视角飞到数据坐标系下的错误位置（表现为「加载后看不到瓦片」）。
                const bool plausible = (srs == nullptr || srs->IsGeographic() || transformed) &&
                        globecore::IsPlausibleLonLat(extent[0], extent[1], extent[2], extent[3]);
                if (repaired != nullptr) repaired->Release();
                if (plausible) ok = true;
                else LOGE("栅格四至不可用：返回源坐标系外的范围，交由上层提示");
            }
            GDALClose(ds);
        }
    }

    return ok;
}

namespace gcbridge {

bool layerExtent(const std::string &path, double extent[4]) {
    GDALAllRegister();
    return computeLayerExtent(path.c_str(), extent);
}

std::vector<std::string> vectorFieldNames(const std::string &path) {
    GDALAllRegister();
    std::vector<std::string> names;

    // OGR 读取第一个图层的字段定义（shp 取 DBF 字段，供整层样式标注字段下拉）
    GDALDataset *ds = (GDALDataset *) GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    if (ds != nullptr) {
        OGRLayer *layer = ds->GetLayer(0);
        if (layer != nullptr) {
            OGRFeatureDefn *defn = layer->GetLayerDefn();
            const int count = defn->GetFieldCount();
            names.reserve(count);
            for (int i = 0; i < count; i++) {
                const char *name = defn->GetFieldDefn(i)->GetNameRef();
                names.emplace_back(name != nullptr ? name : "");
            }
        }
        GDALClose(ds);
    }
    return names;
}

} // namespace gcbridge

/**
 * 图层原始坐标系描述（矢量优先，栅格兜底）：
 * 返回“坐标系名称 + proj4 定义”（两者以换行分隔），无 SRS 或打开失败返回 nullptr。
 * 供图层信息对话框展示原始文件坐标系。
 */
static char *computeLayerSrs(const char *p) {
    std::string out;

    // KML/KMZ 按 OGC KML 标准固定为 WGS84 地理坐标系；部分 GDAL KML 驱动不显式返回 SRS，
    // 直接按扩展名判定（无需打开文件），避免图层信息对 kml/kmz 不显示坐标系。
    std::string lp(p);
    std::transform(lp.begin(), lp.end(), lp.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    if (lp.size() >= 4 &&
        (lp.compare(lp.size() - 4, 4, ".kml") == 0 || lp.compare(lp.size() - 4, 4, ".kmz") == 0)) {
        const char *wgs84 = "WGS 84\n+proj=longlat +datum=WGS84 +no_defs";
        char *kbuf = (char *) CPLMalloc(strlen(wgs84) + 1);
        memcpy(kbuf, wgs84, strlen(wgs84) + 1);
        return kbuf;
    }

    // 矢量数据：OGR 读取第一个图层的空间参考（shp 等）
    GDALDataset *ds = (GDALDataset *) GDALOpenEx(p, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    OGRSpatialReference *srs = nullptr;
    if (ds != nullptr) {
        OGRLayer *layer = ds->GetLayer(0);
        if (layer != nullptr) {
            srs = resolveSourceSrs(p, layer);
        }
        GDALClose(ds);
    }

    // 栅格数据兜底：读取数据集空间参考（ESRI 退化 LOCAL_CS 按瓦片渲染路同一口径重建，
    // 使图层信息展示的坐标系与渲染实际使用的一致）
    if (srs == nullptr) {
        ds = (GDALDataset *) GDALOpenEx(p, GDAL_OF_RASTER | GDAL_OF_READONLY,
                                        nullptr, nullptr, nullptr);
        if (ds != nullptr) {
            const OGRSpatialReference *rsrs = ds->GetSpatialRef();
            OGRSpatialReference *repaired = globecore::TryRepairLocalCsSrs(rsrs, std::string(p));
            if (repaired != nullptr) {
                srs = repaired;
            } else if (rsrs != nullptr) {
                srs = rsrs->Clone();
            }
            GDALClose(ds);
        }
    }

    if (srs != nullptr) {
        srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        const char *name = srs->GetName();
        if (name != nullptr && name[0] != '\0') {
            out += name;
        }
        char *proj4 = nullptr;
        if (srs->exportToProj4(&proj4) == OGRERR_NONE && proj4 != nullptr) {
            if (!out.empty()) out += "\n";
            out += proj4;
            CPLFree(proj4);
        }
        srs->Release();
    }

    if (out.empty()) {
        return nullptr;
    }
    char *buf = (char *) CPLMalloc(out.size() + 1);
    memcpy(buf, out.c_str(), out.size() + 1);
    return buf;
}

namespace gcbridge {

std::string layerSrs(const std::string &path) {
    GDALAllRegister();
    char *srs = computeLayerSrs(path.c_str());
    if (srs == nullptr) return "";
    std::string out(srs);
    CPLFree(srs);
    return out;
}

} // namespace gcbridge

// ── Android JNI 导出薄封装（com.zys.globecore.NativeLayerInfo 门面）──
#if !defined(__OHOS__) && !defined(GLOBECORE_DESKTOP)
#include <jni.h>

extern "C"
JNIEXPORT jdoubleArray JNICALL
Java_com_zys_globecore_NativeLayerInfo_getLayerExtent(
        JNIEnv *env, jobject /*thiz*/, jstring path) {

    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    double extent[4] = {0, 0, 0, 0};
    const bool ok = gcbridge::layerExtent(p, extent);
    env->ReleaseStringUTFChars(path, p);
    if (!ok) {
        return nullptr;
    }
    jdoubleArray result = env->NewDoubleArray(4);
    env->SetDoubleArrayRegion(result, 0, 4, extent);
    return result;
}

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_com_zys_globecore_NativeLayerInfo_getVectorFieldNames(
        JNIEnv *env, jobject /*thiz*/, jstring path) {

    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    std::vector<std::string> names = gcbridge::vectorFieldNames(p);
    env->ReleaseStringUTFChars(path, p);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(names.size()), stringClass, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(names.size()); i++) {
        jstring name = env->NewStringUTF(names[i].c_str());
        env->SetObjectArrayElement(result, i, name);
        env->DeleteLocalRef(name);
    }
    return result;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeLayerInfo_getLayerSrs(
        JNIEnv *env, jobject /*thiz*/, jstring path) {

    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    std::string srs = gcbridge::layerSrs(p);
    env->ReleaseStringUTFChars(path, p);
    if (srs.empty()) {
        return nullptr;
    }
    return env->NewStringUTF(srs.c_str());
}

#endif // !__OHOS__
