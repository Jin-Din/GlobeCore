#ifndef GLOBECORE_RASTER_RASTER_SRS_REPAIR_H
#define GLOBECORE_RASTER_RASTER_SRS_REPAIR_H

// ==================== ESRI 退化坐标系（LOCAL_CS）重建 ====================
//
// ArcGIS 导出的 GeoTIFF 常因 ProjectedCSType 为 ESRI 私有码（32xxx / -1 等）无法被 GDAL 查表，
// 导入后退化为 LOCAL_CS（无椭球、无基准），OGRCreateCoordinateTransformation 建不起来 ——
// 表现是「非 84 坐标栅格加载后位置错误 / 缩放到图层飞到米制坐标处」。
// 文件的 34737(GeoAsciiParams) 里带着完整的 ESRI PE 字符串，据此可重建出标准 PROJCS/GEOGCS。
//
// 【单一口径】本头同时服务 raster/RasterRenderer.cpp（瓦片渲染路的四至）与
// bridge/layer_info.cpp（宿主「缩放到图层 / 图层信息」路的四至）。两处曾各写一份，
// 渲染路有修复、四至路没有，导致同一文件「瓦片位置正确但缩放到图层跑飞」。改动只留此一处。

#include <cstdint>
#include <string>
#include <vector>

#include "gdal/cpl_conv.h"
#include "gdal/cpl_string.h"
#include "gdal/cpl_vsi.h"
#include "gdal/ogr_spatialref.h"

#include "util/Log.h"

namespace globecore {

/// 直读 TIFF/GeoTIFF 的 ASCII 标签原文（经典 TIFF 足够；BigTIFF/异常结构返回空串）。
/// GeoTIFF 的 34737(GeoAsciiParams) 携带 ESRI PE 字符串，GDAL 导入退化后可用于重建坐标系。
inline std::string ReadTiffAsciiTag(const char *path, int tag) {
    std::string out;
    VSILFILE *f = VSIFOpenL(path, "rb");
    if (f == nullptr) return out;
    uint8_t hdr[8];
    if (VSIFReadL(hdr, 1, 8, f) != 8) { VSIFCloseL(f); return out; }
    bool le;
    if (hdr[0] == 'I' && hdr[1] == 'I') le = true;
    else if (hdr[0] == 'M' && hdr[1] == 'M') le = false;
    else { VSIFCloseL(f); return out; }
    if (le ? (hdr[2] != 42) : (hdr[3] != 42)) { VSIFCloseL(f); return out; } // 不支持 BigTIFF
    auto u16 = [le](const uint8_t *p) { return le ? (int)(p[0] | (p[1] << 8)) : (int)((p[0] << 8) | p[1]); };
    auto u32 = [le](const uint8_t *p) {
        return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
                  : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
    };
    const uint32_t ifdOff = u32(hdr + 4);
    if (VSIFSeekL(f, (vsi_l_offset)ifdOff, SEEK_SET) != 0) { VSIFCloseL(f); return out; }
    uint8_t cntB[2];
    if (VSIFReadL(cntB, 1, 2, f) != 2) { VSIFCloseL(f); return out; }
    const int cnt = u16(cntB);
    if (cnt <= 0 || cnt > 65535) { VSIFCloseL(f); return out; }
    std::vector<uint8_t> ifd((size_t)cnt * 12);
    if (VSIFReadL(ifd.data(), 1, ifd.size(), f) != ifd.size()) { VSIFCloseL(f); return out; }
    for (int i = 0; i < cnt; i++) {
        const uint8_t *e = ifd.data() + (size_t)i * 12;
        if (u16(e) != tag) continue;
        const int typ = u16(e + 2);
        if (typ != 2) break; // 仅取 ASCII
        const uint32_t len = u32(e + 4);
        if (len == 0 || len > (16u << 20)) break;
        std::vector<uint8_t> buf(len);
        const vsi_l_offset srcOff = (len <= 4) ? (vsi_l_offset)((size_t)(i) * 12 + ifdOff + 2 + 8) : (vsi_l_offset)u32(e + 8);
        if (VSIFSeekL(f, srcOff, SEEK_SET) == 0 && VSIFReadL(buf.data(), 1, len, f) == len)
            out.assign(reinterpret_cast<char *>(buf.data()), len);
        break;
    }
    VSIFCloseL(f);
    return out;
}

/// 按 proj4 构造 SRS（传统经纬度轴序），失败返回 nullptr。
inline OGRSpatialReference *SrsFromProj4(const char *proj4) {
    OGRSpatialReference *s = new OGRSpatialReference();
    s->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (s->importFromProj4(proj4) != OGRERR_NONE) { delete s; return nullptr; }
    return s;
}

/// ESRI 退化的 LOCAL_CS：从文件 GeoAsciiParams 中的 ESRI PE 字符串重建完整 PROJCS/GEOGCS。
/// 仅当 srcSrs 确为 LOCAL_CS 且重建结果为标准坐标系时才返回新对象（调用者 Release）；
/// 其余情形返回 nullptr（调用方沿用原 srs）。
inline OGRSpatialReference *TryRepairLocalCsSrs(const OGRSpatialReference *srcSrs, const std::string &tifPath) {
    if (srcSrs == nullptr) return nullptr;
    char *wktRaw = nullptr;
    srcSrs->exportToWkt(&wktRaw);
    const std::string wkt = wktRaw != nullptr ? wktRaw : "";
    CPLFree(wktRaw);
    if (wkt.compare(0, 8, "LOCAL_CS") != 0) return nullptr;

    const std::string ascii = ReadTiffAsciiTag(tifPath.c_str(), 34737);
    const size_t pos = ascii.find("ESRI PE String = ");
    if (pos == std::string::npos) return nullptr;
    std::string pe = ascii.substr(pos + 17);
    const size_t semi = pe.find(';');
    if (semi != std::string::npos) pe.erase(semi);
    if (pe.empty()) return nullptr;

    // PE 串中出现即视为该投影（按名称匹配，避免依赖 GDAL_DATA 映射表）。本表均为 WGS84 基准，
    // proj4 导入经 proj.db 重建后椭球/基准完整、可建坐标变换。
    struct PeNameProj4 { const char *name; const char *proj4; };
    static const PeNameProj4 kPeNameProj4[] = {
            {"WGS_1984_Web_Mercator_Auxiliary_Sphere",
             "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wgs84=0,0,0 +no_defs"},
            {"WGS_1984_Pseudo_Mercator",
             "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wgs84=0,0,0 +no_defs"},
            {"WGS_1984_Master_Grid",
             "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wgs84=0,0,0 +no_defs"},
            {"Popular Visualisation Pseudo Mercator",
             "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wgs84=0,0,0 +no_defs"},
    };

    // 首选：已知投影名 → proj4 权威定义（Web Mercator 等 ESRI 私有码高发场景）
    for (const auto &m : kPeNameProj4) {
        if (pe.find(m.name) != std::string::npos) {
            OGRSpatialReference *fixed = SrsFromProj4(m.proj4);
            if (fixed != nullptr) {
                LOGI("TryRepairLocalCsSrs: PE 名匹配 %s，proj4 重建", m.name);
                return fixed;
            }
        }
    }

    // 次选：GDAL ESRI PE 导入（拷贝可写缓冲、判首 token 后再调，规避退化输入崩溃）
    std::string peSp = pe;
    for (auto &c : peSp) if (c == '|') c = ' ';
    const bool peShaped = peSp.rfind("PROJCS[", 0) == 0 || peSp.rfind("GEOGCS[", 0) == 0;
    if (peShaped) {
        CPLString buf(peSp.c_str());
        char *texts[1] = { const_cast<char *>(buf.c_str()) };
        OGRSpatialReference *fixed = new OGRSpatialReference();
        fixed->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (fixed->importFromESRI(texts) == OGRERR_NONE) {
            char *fw = nullptr;
            fixed->exportToWkt(&fw);
            const std::string fwkt = fw != nullptr ? fw : "";
            CPLFree(fw);
            if (!fwkt.empty() && fwkt.compare(0, 8, "LOCAL_CS") != 0) {
                LOGI("TryRepairLocalCsSrs: ESRI PE 导入成功 %.16s", fwkt.c_str());
                return fixed;
            }
        }
        delete fixed;
    }
    LOGW("TryRepairLocalCsSrs: PE 串无法重建坐标系 path=%s", tifPath.c_str());
    return nullptr;
}

/// 投影退化护栏：源为投影坐标系、且（变换后）坐标仍落在有效经纬度域外——典型如 ESRI LOCAL_CS
/// 缺基准信息时变换退化为恒等，米制值被直接当度使用。此时绝不返回该包络（上层会把米当经纬度
/// 缩放/过滤，导致视角与加载错位）。
inline bool IsPlausibleLonLat(double minLon, double minLat, double maxLon, double maxLat) {
    return minLon >= -180.001 && maxLon <= 180.001 && minLat >= -90.001 && maxLat <= 90.001;
}

} // namespace globecore

#endif // GLOBECORE_RASTER_RASTER_SRS_REPAIR_H
