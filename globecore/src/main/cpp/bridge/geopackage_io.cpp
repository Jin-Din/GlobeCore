// GeoPackage 工程数据读写（0.4.0.0）
// 使用 OGR GeoPackage 驱动（已内嵌在 GDAL 中）创建/读取 .gpkg 文件。
// Kotlin 侧 Project ↔ JSON（JNI 传输格式）→ 本文件解析 JSON 逐要素写表；
// 读取侧遍历各表要素序列化回同一 JSON 结构（与 Project.fromJson 兼容）。
// Android JNI 导出为薄封装，与 vector_io.cpp 同风格。

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "gdal/ogrsf_frmts.h"
#include "gdal/gdal_priv.h"
#include "gdal/cpl_error.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include <jni.h>

using globecore::JsonValue;
using globecore::parseJson;

// ---- JSON 转义（输出侧，复用 vector_io.cpp 风格）----

static void jsonEscape(std::string &out, const char *s) {
    if (s == nullptr) return;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char) *p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
}

// ---- JSON 读取辅助（解析侧）----

static std::string jsonStr(const JsonValue &v, const char *key) {
    const JsonValue &f = v.at(key);
    return f.isString() ? f.strVal : std::string();
}

static long long jsonInt(const JsonValue &v, const char *key, long long def = 0) {
    const JsonValue &f = v.at(key);
    return f.isNumber() ? (long long) f.numVal : def;
}

static bool jsonBool(const JsonValue &v, const char *key, bool def = false) {
    const JsonValue &f = v.at(key);
    return f.isBool() ? f.boolVal : def;
}

/** 解析 [[lon,lat,alt],...] 点列 */
static void jsonPoints(const JsonValue &v, std::vector<double> &out) {
    const JsonValue &pts = v.at("points");
    if (!pts.isArray()) return;
    for (const JsonValue &pt : pts.arr) {
        if (!pt.isArray() || pt.arr.size() < 2) continue;
        out.push_back(pt.at(0).numVal);
        out.push_back(pt.at(1).numVal);
        const JsonValue &z = pt.at(2);
        out.push_back(z.isNumber() ? z.numVal : 0.0);
    }
}

/** JsonValue → JSON 字符串（用于 survey_plots.attributes 原样存取） */
static void writeJsonValue(std::string &out, const JsonValue &v) {
    switch (v.type) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool:
            out += v.boolVal ? "true" : "false";
            break;
        case JsonValue::Type::Number: {
            char buf[40];
            double d = v.numVal;
            if (d == std::floor(d) && std::fabs(d) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", (long long) d);
            } else {
                snprintf(buf, sizeof(buf), "%.10g", d);
            }
            out += buf;
            break;
        }
        case JsonValue::Type::String:
            out += '"';
            jsonEscape(out, v.strVal.c_str());
            out += '"';
            break;
        case JsonValue::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out += ',';
                writeJsonValue(out, v.arr[i]);
            }
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            {
                bool first = true;
                for (const auto &kv : v.obj) {
                    if (!first) out += ',';
                    first = false;
                    out += '"';
                    jsonEscape(out, kv.first.c_str());
                    out += "\":";
                    writeJsonValue(out, kv.second);
                }
            }
            out += '}';
            break;
    }
}

// ---- OGR 字段写入辅助 ----

static void setFieldStr(OGRFeature *feat, const char *name, const std::string &val) {
    if (val.empty()) return; // 空串视为未设置（对应 Kotlin null）
    feat->SetField(name, val.c_str());
}

static void setFieldInt(OGRFeature *feat, const char *name, long long val) {
    feat->SetField(name, (GIntBig) val);
}

static void setFieldBool(OGRFeature *feat, const char *name, bool val) {
    feat->SetField(name, val ? 1 : 0);
}

// ---- OGR 几何构建 ----

static OGRLineString *pointsToLine(const std::vector<double> &pts) {
    auto *line = new OGRLineString();
    for (size_t i = 0; i + 2 < pts.size(); i += 3) {
        line->addPoint(pts[i], pts[i + 1], pts[i + 2]);
    }
    return line;
}

static OGRPolygon *pointsToPolygon(const std::vector<double> &pts) {
    auto *poly = new OGRPolygon();
    auto *ring = new OGRLinearRing();
    for (size_t i = 0; i + 2 < pts.size(); i += 3) {
        ring->addPoint(pts[i], pts[i + 1], pts[i + 2]);
    }
    ring->closeRings(); // 自动补首点闭合（OGR 面要求）
    poly->addRingDirectly(ring);
    return poly;
}

// ---- 表创建 ----

static OGRSpatialReference *makeWgs84() {
    auto *srs = new OGRSpatialReference();
    srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs->SetWellKnownGeogCS("WGS84");
    return srs;
}

static void createLayerFields(OGRLayer *layer, const std::vector<std::pair<const char *, OGRFieldType>> &fields,
                              const std::vector<int> &widths = {}) {
    for (size_t i = 0; i < fields.size(); i++) {
        OGRFieldDefn fd(fields[i].first, fields[i].second);
        if (i < widths.size() && widths[i] > 0) fd.SetWidth(widths[i]);
        layer->CreateField(&fd);
    }
}

// ---- GeoPackage 写入 ----

/**
 * 创建 GeoPackage 文件并写入工程数据。
 * 表结构：
 *   - tracks:        LineString, name, date, color, hidden, line_width
 *   - measures:      Geometry(Point/LineString/Polygon), name, date, type, result, color, hidden, closed, line_width
 *   - photos:        Point, name, photo_path, timestamp, attachments(JSON), visible, group_id
 *   - media_groups:  (非空间) name, created_time, visible（fid 即 id）
 *   - survey_plots:  Geometry(Point/Polygon), name, plot_type, attributes(JSON)
 *   - project_meta:  key, value
 *   - layer_refs:    layer_path
 */
static bool writeGeoPackage(const std::string &filePath, const std::string &projectJson) {
    GDALAllRegister();

    // 解析工程 JSON（失败则只建空表结构）
    JsonValue root = parseJson(projectJson);
    if (!root.isObject()) {
        LOGE("工程 JSON 解析失败: %s", filePath.c_str());
    }

    // 删除旧文件（如果存在）
    remove(filePath.c_str());

    // 创建 GeoPackage 数据集
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (driver == nullptr) {
        LOGE("GPKG 驱动未注册");
        return false;
    }

    GDALDataset *ds = driver->Create(filePath.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (ds == nullptr) {
        LOGE("GeoPackage 创建失败: %s", filePath.c_str());
        return false;
    }

    // ---- tracks 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("tracks", srs, wkbLineString, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"date",       OFTString},
                    {"color",      OFTString},
                    {"hidden",     OFTInteger},
                    {"line_width", OFTInteger},
            }, {256, 64, 16, 0, 0});

            const JsonValue &tracks = root.at("tracks");
            if (tracks.isArray()) {
                for (const JsonValue &t : tracks.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    setFieldStr(feat, "name", jsonStr(t, "name"));
                    setFieldStr(feat, "date", jsonStr(t, "date"));
                    setFieldStr(feat, "color", jsonStr(t, "color"));
                    setFieldBool(feat, "hidden", jsonBool(t, "hidden"));
                    setFieldInt(feat, "line_width", jsonInt(t, "lineWidth", 3));
                    std::vector<double> pts;
                    jsonPoints(t, pts);
                    feat->SetGeometryDirectly(pointsToLine(pts));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("tracks 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- measures 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("measures", srs, wkbUnknown, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"date",       OFTString},
                    {"type",       OFTString},
                    {"result",     OFTString},
                    {"color",      OFTString},
                    {"hidden",     OFTInteger},
                    {"closed",     OFTInteger},
                    {"line_width", OFTInteger},
            }, {256, 64, 64, 512, 16, 0, 0, 0});

            const JsonValue &measures = root.at("measures");
            if (measures.isArray()) {
                for (const JsonValue &m : measures.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    std::string type = jsonStr(m, "type");
                    setFieldStr(feat, "name", jsonStr(m, "name"));
                    setFieldStr(feat, "date", jsonStr(m, "date"));
                    setFieldStr(feat, "type", type);
                    setFieldStr(feat, "result", jsonStr(m, "result"));
                    setFieldStr(feat, "color", jsonStr(m, "color"));
                    setFieldBool(feat, "hidden", jsonBool(m, "hidden"));
                    setFieldBool(feat, "closed", jsonBool(m, "closed"));
                    setFieldInt(feat, "line_width", jsonInt(m, "lineWidth", 3));

                    std::vector<double> pts;
                    jsonPoints(m, pts);
                    OGRGeometry *geom;
                    if (type == "point" && pts.size() >= 3) {
                        geom = new OGRPoint(pts[0], pts[1], pts[2]);
                    } else if (type == "area" || type == "polygon") {
                        geom = pointsToPolygon(pts);
                    } else {
                        // distance / line / angle 均为线
                        geom = pointsToLine(pts);
                    }
                    feat->SetGeometryDirectly(geom);
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("measures 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- photos 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("photos", srs, wkbPoint, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",        OFTString},
                    {"photo_path",  OFTString},
                    {"timestamp",   OFTInteger64},
                    {"attachments", OFTString},
                    {"visible",     OFTInteger},
                    {"group_id",    OFTInteger64},
            }, {256, 1024, 0, 0, 0, 0});

            const JsonValue &photos = root.at("photos");
            if (photos.isArray()) {
                for (const JsonValue &p : photos.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    setFieldStr(feat, "name", jsonStr(p, "name"));
                    setFieldInt(feat, "timestamp", jsonInt(p, "timestamp"));
                    // attachments 数组整体序列化为 JSON 字符串存列；
                    // photo_path 列继续写入（= 首个附件 path），供 QGIS 直接查看主照片
                    const JsonValue &atts = p.at("attachments");
                    if (atts.isArray() && !atts.arr.empty()) {
                        std::string attsJson;
                        writeJsonValue(attsJson, atts);
                        setFieldStr(feat, "attachments", attsJson);
                        setFieldStr(feat, "photo_path", jsonStr(atts.arr[0], "path"));
                    }
                    setFieldBool(feat, "visible", jsonBool(p, "visible", true));
                    setFieldInt(feat, "group_id", jsonInt(p, "groupId", -1));
                    double lon = p.at("lon").numVal;
                    double lat = p.at("lat").numVal;
                    double alt = p.at("alt").numVal;
                    feat->SetGeometryDirectly(new OGRPoint(lon, lat, alt));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("photos 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- media_groups 图层（非空间表，fid 即 id）----
    {
        OGRLayer *layer = ds->CreateLayer("media_groups", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",         OFTString},
                    {"created_time", OFTInteger64},
                    {"visible",      OFTInteger},
            }, {256, 0, 0});

            const JsonValue &groups = root.at("mediaGroups");
            if (groups.isArray()) {
                for (const JsonValue &g : groups.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    setFieldStr(feat, "name", jsonStr(g, "name"));
                    setFieldInt(feat, "created_time", jsonInt(g, "createdTime"));
                    setFieldBool(feat, "visible", jsonBool(g, "visible", true));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("media_groups 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- survey_plots 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("survey_plots", srs, wkbUnknown, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"plot_type",  OFTString},
                    {"attributes", OFTString},
            }, {256, 64, 4096});

            const JsonValue &plots = root.at("surveyPlots");
            if (plots.isArray()) {
                for (const JsonValue &s : plots.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    std::string plotType = jsonStr(s, "plotType");
                    setFieldStr(feat, "name", jsonStr(s, "name"));
                    setFieldStr(feat, "plot_type", plotType);
                    const JsonValue &attrs = s.at("attributes");
                    if (attrs.type != JsonValue::Type::Null) {
                        std::string attrsJson;
                        writeJsonValue(attrsJson, attrs);
                        setFieldStr(feat, "attributes", attrsJson);
                    }

                    std::vector<double> pts;
                    jsonPoints(s, pts);
                    OGRGeometry *geom;
                    if (plotType == "point" && pts.size() >= 3) {
                        geom = new OGRPoint(pts[0], pts[1], pts[2]);
                    } else {
                        geom = pointsToPolygon(pts);
                    }
                    feat->SetGeometryDirectly(geom);
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("survey_plots 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- project_meta 图层（非空间表）----
    {
        OGRLayer *layer = ds->CreateLayer("project_meta", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"key",   OFTString},
                    {"value", OFTString},
            }, {256, 4096});

            auto writeMeta = [layer](const char *key, const std::string &value) {
                if (value.empty()) return;
                OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                feat->SetField("key", key);
                feat->SetField("value", value.c_str());
                layer->CreateFeature(feat);
                OGRFeature::DestroyFeature(feat);
            };
            writeMeta("name", jsonStr(root, "name"));
            writeMeta("description", jsonStr(root, "description"));
            writeMeta("srs", jsonStr(root, "srs"));
            writeMeta("createTime", std::to_string(jsonInt(root, "createTime")));
            writeMeta("updateTime", std::to_string(jsonInt(root, "updateTime")));
        }
    }

    // ---- layer_refs 图层（非空间表）----
    {
        OGRLayer *layer = ds->CreateLayer("layer_refs", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"layer_path", OFTString},
            }, {4096});

            const JsonValue &refs = root.at("layerRefs");
            if (refs.isArray()) {
                for (const JsonValue &r : refs.arr) {
                    if (!r.isString() || r.strVal.empty()) continue;
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    feat->SetField("layer_path", r.strVal.c_str());
                    layer->CreateFeature(feat);
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    GDALClose(ds);
    LOGI("GeoPackage 写入成功: %s", filePath.c_str());
    return true;
}

// ---- GeoPackage 读取 ----

// ---- OGR 字段读取辅助 ----

static std::string getFieldStr(OGRFeature *feat, const char *name) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return "";
    return feat->GetFieldAsString(idx);
}

static long long getFieldInt(OGRFeature *feat, const char *name) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return 0;
    return (long long) feat->GetFieldAsInteger64(idx);
}

/** 带默认值的整列读取：旧版 gpkg 缺列时回默认值（读侧兼容） */
static long long getFieldIntDef(OGRFeature *feat, const char *name, long long def) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return def;
    return (long long) feat->GetFieldAsInteger64(idx);
}

// ---- JSON 输出辅助 ----

static void outStrField(std::string &out, const char *key, const std::string &value, bool &first) {
    if (value.empty()) return; // 空字段跳过（Kotlin 侧 optString 有兜底）
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":\"";
    jsonEscape(out, value.c_str());
    out += "\"";
}

static void outIntField(std::string &out, const char *key, long long value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    out += buf;
}

static void outDoubleField(std::string &out, const char *key, double value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.8f", value);
    out += buf;
}

static void outBoolField(std::string &out, const char *key, bool value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    out += value ? "true" : "false";
}

/** 输出线/环点列为 [[x,y,z],...]；skipClosingDup=true 时去掉与首点重合的末点（面闭合环） */
static void outLinePoints(std::string &out, const OGRLineString *line, bool skipClosingDup) {
    out += "[";
    int n = line->getNumPoints();
    if (skipClosingDup && n > 1 &&
        line->getX(0) == line->getX(n - 1) && line->getY(0) == line->getY(n - 1)) {
        n--;
    }
    for (int i = 0; i < n; i++) {
        if (i) out += ",";
        char buf[128];
        snprintf(buf, sizeof(buf), "[%.8f,%.8f,%.3f]",
                 line->getX(i), line->getY(i), line->getZ(i));
        out += buf;
    }
    out += "]";
}

/** 通用几何 → points 数组（Point→单点；Line→点列；Polygon→外环去闭合点） */
static void outGeomPoints(std::string &out, const OGRGeometry *geom) {
    if (geom == nullptr) {
        out += "[]";
        return;
    }
    switch (wkbFlatten(geom->getGeometryType())) {
        case wkbPoint: {
            auto *pt = (const OGRPoint *) geom;
            char buf[128];
            snprintf(buf, sizeof(buf), "[[%.8f,%.8f,%.3f]]", pt->getX(), pt->getY(), pt->getZ());
            out += buf;
            break;
        }
        case wkbLineString:
            outLinePoints(out, (const OGRLineString *) geom, false);
            break;
        case wkbPolygon: {
            auto *poly = (const OGRPolygon *) geom;
            const OGRLinearRing *ring = poly->getExteriorRing();
            if (ring != nullptr) {
                outLinePoints(out, ring, true);
            } else {
                out += "[]";
            }
            break;
        }
        default:
            out += "[]";
            break;
    }
}

/** 读取指定图层，逐要素输出 JSON 数组元素（first 跨要素维护逗号） */
static void readSpatialLayer(GDALDataset *ds, const char *layerName, std::string &out,
                             bool &firstElem, const char *kind) {
    OGRLayer *layer = ds->GetLayerByName(layerName);
    if (layer == nullptr) return;

    layer->ResetReading();
    OGRFeature *feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        if (!firstElem) out += ",";
        firstElem = false;
        out += "{";

        bool first = true;
        outIntField(out, "id", (long long) feat->GetFID(), first);
        outStrField(out, "name", getFieldStr(feat, "name"), first);

        if (strcmp(kind, "tracks") == 0) {
            outStrField(out, "date", getFieldStr(feat, "date"), first);
            outStrField(out, "color", getFieldStr(feat, "color"), first);
            outBoolField(out, "hidden", getFieldInt(feat, "hidden") != 0, first);
            outIntField(out, "lineWidth", getFieldIntDef(feat, "line_width", 3), first);
        } else if (strcmp(kind, "measures") == 0) {
            outStrField(out, "date", getFieldStr(feat, "date"), first);
            outStrField(out, "type", getFieldStr(feat, "type"), first);
            outStrField(out, "result", getFieldStr(feat, "result"), first);
            outStrField(out, "color", getFieldStr(feat, "color"), first);
            outBoolField(out, "hidden", getFieldInt(feat, "hidden") != 0, first);
            outBoolField(out, "closed", getFieldInt(feat, "closed") != 0, first);
            outIntField(out, "lineWidth", getFieldIntDef(feat, "line_width", 3), first);
        } else if (strcmp(kind, "photos") == 0) {
            // photos 的 lon/lat/alt 由几何提取（下方单独处理）
            long long ts = getFieldInt(feat, "timestamp");
            outIntField(out, "timestamp", ts, first);
            outBoolField(out, "visible", getFieldIntDef(feat, "visible", 1) != 0, first);
            outIntField(out, "groupId", getFieldIntDef(feat, "group_id", -1), first);
            // attachments：优先 attachments 列 JSON（原样嵌入）；空且 photo_path 非空时
            // 回退生成单元素 attachments（name 取文件名、path=photo_path、time=timestamp）
            std::string atts = getFieldStr(feat, "attachments");
            if (atts.empty() || atts[0] != '[') {
                std::string photoPath = getFieldStr(feat, "photo_path");
                if (!photoPath.empty()) {
                    size_t pos = photoPath.find_last_of("/\\");
                    std::string fname = (pos == std::string::npos) ? photoPath : photoPath.substr(pos + 1);
                    std::string fb = "[{\"name\":\"";
                    jsonEscape(fb, fname.c_str());
                    fb += "\",\"path\":\"";
                    jsonEscape(fb, photoPath.c_str());
                    fb += "\",\"time\":";
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", ts);
                    fb += buf;
                    fb += "}]";
                    atts = fb;
                } else {
                    atts = "[]";
                }
            }
            if (!first) out += ",";
            first = false;
            out += "\"attachments\":";
            out += atts;
        } else if (strcmp(kind, "survey_plots") == 0) {
            outStrField(out, "plotType", getFieldStr(feat, "plot_type"), first);
            // attributes 为 JSON 字符串，原样嵌入输出（非转义字符串）
            std::string attrs = getFieldStr(feat, "attributes");
            if (!attrs.empty() && attrs[0] == '{') {
                if (!first) out += ",";
                first = false;
                out += "\"attributes\":";
                out += attrs;
            }
        }

        if (strcmp(kind, "photos") == 0) {
            // PhotoRecord 需要 lon/lat/alt 显式字段
            const OGRGeometry *geom = feat->GetGeometryRef();
            double lon = 0, lat = 0, alt = 0;
            if (geom != nullptr && wkbFlatten(geom->getGeometryType()) == wkbPoint) {
                auto *pt = (const OGRPoint *) geom;
                lon = pt->getX();
                lat = pt->getY();
                alt = pt->getZ();
            }
            outDoubleField(out, "lon", lon, first);
            outDoubleField(out, "lat", lat, first);
            outDoubleField(out, "alt", alt, first);
        } else {
            // 其余空间表输出 points 数组
            if (!first) out += ",";
            first = false;
            out += "\"points\":";
            outGeomPoints(out, feat->GetGeometryRef());
        }

        out += "}";
        OGRFeature::DestroyFeature(feat);
    }
}

/**
 * 从 GeoPackage 读取工程数据，返回与 Project.fromJson 兼容的 JSON 字符串。
 * 文件不存在或结构不符时返回空串。
 */
static std::string readGeoPackage(const std::string &filePath) {
    GDALAllRegister();

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(
            filePath.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr);

    if (ds == nullptr) {
        LOGE("GeoPackage 打开失败: %s", filePath.c_str());
        return "";
    }

    std::string out = "{";
    bool first = true;

    // ---- project_meta ----
    {
        OGRLayer *meta = ds->GetLayerByName("project_meta");
        long long createTime = 0, updateTime = 0;
        if (meta != nullptr) {
            meta->ResetReading();
            OGRFeature *feat;
            while ((feat = meta->GetNextFeature()) != nullptr) {
                std::string key = getFieldStr(feat, "key");
                std::string value = getFieldStr(feat, "value");
                if (key == "name") {
                    outStrField(out, "name", value, first);
                } else if (key == "description") {
                    outStrField(out, "description", value, first);
                } else if (key == "srs") {
                    outStrField(out, "srs", value, first);
                } else if (key == "createTime") {
                    createTime = std::strtoll(value.c_str(), nullptr, 10);
                } else if (key == "updateTime") {
                    updateTime = std::strtoll(value.c_str(), nullptr, 10);
                }
                OGRFeature::DestroyFeature(feat);
            }
        }
        outIntField(out, "createTime", createTime, first);
        outIntField(out, "updateTime", updateTime, first);
    }

    // ---- 空间数据表 ----
    out += ",\"tracks\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "tracks", out, firstElem, "tracks");
    }
    out += "]";

    out += ",\"measures\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "measures", out, firstElem, "measures");
    }
    out += "]";

    out += ",\"photos\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "photos", out, firstElem, "photos");
    }
    out += "]";

    // ---- media_groups ----
    out += ",\"mediaGroups\":[";
    {
        OGRLayer *groups = ds->GetLayerByName("media_groups");
        bool firstElem = true;
        if (groups != nullptr) {
            groups->ResetReading();
            OGRFeature *feat;
            while ((feat = groups->GetNextFeature()) != nullptr) {
                if (!firstElem) out += ",";
                firstElem = false;
                out += "{";
                bool first = true;
                outIntField(out, "id", (long long) feat->GetFID(), first);
                outStrField(out, "name", getFieldStr(feat, "name"), first);
                outIntField(out, "createdTime", getFieldInt(feat, "created_time"), first);
                outBoolField(out, "visible", getFieldIntDef(feat, "visible", 1) != 0, first);
                out += "}";
                OGRFeature::DestroyFeature(feat);
            }
        }
    }
    out += "]";

    out += ",\"surveyPlots\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "survey_plots", out, firstElem, "survey_plots");
    }
    out += "]";

    // ---- layer_refs ----
    out += ",\"layerRefs\":[";
    {
        OGRLayer *refs = ds->GetLayerByName("layer_refs");
        bool firstElem = true;
        if (refs != nullptr) {
            refs->ResetReading();
            OGRFeature *feat;
            while ((feat = refs->GetNextFeature()) != nullptr) {
                std::string path = getFieldStr(feat, "layer_path");
                if (!path.empty()) {
                    if (!firstElem) out += ",";
                    firstElem = false;
                    out += "\"";
                    jsonEscape(out, path.c_str());
                    out += "\"";
                }
                OGRFeature::DestroyFeature(feat);
            }
        }
    }
    out += "]";

    out += "}";

    GDALClose(ds);
    LOGI("GeoPackage 读取成功: %s", filePath.c_str());
    return out;
}

// ---- JNI 导出 ----

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_zys_mobilemap_project_GeoPackageWriter_nativeWriteGeoPackage(
        JNIEnv *env, jobject /*thiz*/, jstring filePath, jstring projectJson) {

    if (filePath == nullptr || projectJson == nullptr) return JNI_FALSE;

    const char *fp = env->GetStringUTFChars(filePath, nullptr);
    if (fp == nullptr) return JNI_FALSE;
    std::string fpStr(fp);
    env->ReleaseStringUTFChars(filePath, fp);

    const char *pj = env->GetStringUTFChars(projectJson, nullptr);
    if (pj == nullptr) return JNI_FALSE;
    std::string pjStr(pj);
    env->ReleaseStringUTFChars(projectJson, pj);

    return writeGeoPackage(fpStr, pjStr) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_mobilemap_project_GeoPackageWriter_nativeReadGeoPackage(
        JNIEnv *env, jobject /*thiz*/, jstring filePath) {

    if (filePath == nullptr) return nullptr;

    const char *fp = env->GetStringUTFChars(filePath, nullptr);
    if (fp == nullptr) return nullptr;
    std::string fpStr(fp);
    env->ReleaseStringUTFChars(filePath, fp);

    std::string json = readGeoPackage(fpStr);
    if (json.empty()) return nullptr;
    return env->NewStringUTF(json.c_str());
}
