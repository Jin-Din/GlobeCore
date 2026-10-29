// @Jin HarmonyOS NEXT LITE 构建专用：不引入 GDAL/OGR，直接把标准 GeoJSON DOM 转成 VectorFeatureData。
// @Jin 远程 URL 的下载、原子缓存、大小限制仍在共享 C++ RemoteVectorCache 中，本文件只负责缓存落地后的解析。
#include "ohos/lite_geojson_reader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stack>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "util/JsonParser.h"
#include "util/Log.h"

namespace globecore {
namespace {

constexpr int kMaxFeatures = 200000;
constexpr int64_t kMaxVertices = 2000000;

// @Jin 缓存文件和本地样例均使用 .geojson/.json 后缀；其他矢量格式继续返回 LITE 不支持。
bool endsWithIgnoreCase(std::string value, const std::string &suffix) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isLiteGeoJsonPath(const std::string &path) {
    return endsWithIgnoreCase(path, ".geojson") || endsWithIgnoreCase(path, ".json");
}

// @Jin labelField 可能指向字符串、数字或布尔属性，统一转成可绘制文本，对象/数组不标注。
std::string scalarToString(const JsonValue &value) {
    if (value.isString()) return value.strVal;
    if (value.isBool()) return value.boolVal ? "true" : "false";
    if (value.isNumber()) {
        // @Jin P0 标签只需要可读文本；整数去掉小数尾零，其他数值保留短格式。
        if (std::isfinite(value.numVal) && std::fabs(value.numVal - std::round(value.numVal)) < 1e-9 &&
            std::fabs(value.numVal) < 1e15) {
            return std::to_string(static_cast<long long>(std::llround(value.numVal)));
        }
        return std::to_string(value.numVal);
    }
    return "";
}

// @Jin GeoJSON 按 WGS84 经纬度提供；用几何包围盒做视口粗过滤，跨 180° 经线边界场景留待后续增强。
struct Bounds {
    bool valid = false;
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;

    void add(double lon, double lat) {
        if (!std::isfinite(lon) || !std::isfinite(lat) || lon < -180.0 || lon > 180.0 ||
            lat < -90.0 || lat > 90.0) {
            return;
        }
        if (!valid) {
            valid = true;
            minLon = maxLon = lon;
            minLat = maxLat = lat;
        } else {
            minLon = std::min(minLon, lon);
            maxLon = std::max(maxLon, lon);
            minLat = std::min(minLat, lat);
            maxLat = std::max(maxLat, lat);
        }
    }

    bool intersects(double minX, double minY, double maxX, double maxY) const {
        return valid && !(maxLon < minX || minLon > maxX || maxLat < minY || minLat > maxY);
    }
};

// @Jin 读取并校验一个坐标分量；GeoJSON P0 只消费前两维 lon/lat，高度统一按贴地处理。
bool coordAt(const JsonValue &array, size_t index, double &out) {
    if (!array.isArray() || index >= array.arr.size() || !array.arr[index].isNumber()) return false;
    out = array.arr[index].numVal;
    return std::isfinite(out);
}

// @Jin 递归扫描任意深度坐标数组，生成 extent 过滤所需的几何包围盒。
void collectPositionBounds(const JsonValue &coords, Bounds &bounds) {
    if (!coords.isArray()) return;
    double lon = 0.0, lat = 0.0;
    if (coordAt(coords, 0, lon) && coordAt(coords, 1, lat) &&
        coords.arr.size() >= 2 &&
        (!coords.arr[0].isArray() && !coords.arr[1].isArray())) {
        bounds.add(lon, lat);
        return;
    }
    for (const JsonValue &child : coords.arr) collectPositionBounds(child, bounds);
}

// @Jin 单个坐标点转为引擎既有的 [lon,lat] 摊平数组，非法经纬度直接跳过。
void appendPosition(const JsonValue &position, std::vector<double> &out, int64_t &vertexCount) {
    double lon = 0.0, lat = 0.0;
    if (!coordAt(position, 0, lon) || !coordAt(position, 1, lat) ||
        lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0) {
        return;
    }
    out.push_back(lon);
    out.push_back(lat);
    ++vertexCount;
}

// @Jin LineString/Polygon 环共用同一坐标展开逻辑，后续继续交给 VectorBuilder 剖分和贴地。
void appendLine(const JsonValue &positions, std::vector<double> &out, int64_t &vertexCount) {
    if (!positions.isArray()) return;
    for (const JsonValue &position : positions.arr) appendPosition(position, out, vertexCount);
}

// @Jin 在线 GeoJSON 可能直接携带数万级行政界点；HarmonyOS NEXT LITE 路径不依赖 GDAL 几何简化，
// @Jin 这里仅对超大闭合环做保守 Douglas-Peucker 抽稀，阈值 300m，小环保持原坐标不变。
constexpr bool kLiteSimplifyRemoteLargeRings = true; // @Jin 实验开关：false 可完整回退在线 GeoJSON 超大环抽稀，恢复原始坐标解析。
constexpr size_t kLiteLargeRingSimplifyPoints = 2000;
constexpr double kLiteLargeRingSimplifyMeters = 300.0; // @Jin drape20 远程省域实验阈值：75m->300m 后陕西外环约 2.4k 点，岛洞小环不抽稀，可回退到 75m。

double liteRingPerpDistanceMeters(double lon, double lat, double aLon, double aLat,
                                  double bLon, double bLat) {
    const double meanLatRad = (aLat + bLat) * 0.5 * 3.14159265358979323846 / 180.0;
    const double metersPerLon = 111320.0 * std::max(0.1, std::cos(meanLatRad));
    const double metersPerLat = 111320.0;
    const double px = (lon - aLon) * metersPerLon;
    const double py = (lat - aLat) * metersPerLat;
    const double bx = (bLon - aLon) * metersPerLon;
    const double by = (bLat - aLat) * metersPerLat;
    const double len2 = bx * bx + by * by;
    if (len2 <= 1e-12) return std::hypot(px, py);
    const double t = std::max(0.0, std::min(1.0, (px * bx + py * by) / len2));
    const double qx = t * bx;
    const double qy = t * by;
    return std::hypot(px - qx, py - qy);
}

// @Jin 对闭合环的去重闭合点序列做线性 DP；首尾接缝点保留，输出仍由调用方补回首点闭合。
void appendSimplifiedClosedLine(const JsonValue &positions, std::vector<double> &out,
                                int64_t &vertexCount, bool enabled) {
    if (!enabled || !positions.isArray() || positions.arr.size() < 4) {
        appendLine(positions, out, vertexCount);
        return;
    }
    std::vector<double> lons, lats;
    lons.reserve(positions.arr.size());
    lats.reserve(positions.arr.size());
    for (const JsonValue &position : positions.arr) {
        double lon = 0.0, lat = 0.0;
        if (coordAt(position, 0, lon) && coordAt(position, 1, lat)) {
            lons.push_back(lon);
            lats.push_back(lat);
        }
    }
    const bool closed = lons.size() >= 4 &&
                        std::fabs(lons.front() - lons.back()) < 1e-10 &&
                        std::fabs(lats.front() - lats.back()) < 1e-10;
    if (!closed || lons.size() - 1 < kLiteLargeRingSimplifyPoints) {
        appendLine(positions, out, vertexCount);
        return;
    }

    const size_t n = lons.size() - 1; // @Jin 不把重复闭合点放进 DP，避免重复端点影响最大偏差搜索。
    std::vector<unsigned char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    struct Span { size_t first; size_t last; };
    std::stack<Span> stack;
    stack.push({0, n - 1});
    while (!stack.empty()) {
        const Span span = stack.top();
        stack.pop();
        if (span.last <= span.first + 1) continue;
        size_t bestIndex = 0;
        double bestDistance = -1.0;
        for (size_t i = span.first + 1; i < span.last; ++i) {
            const double d = liteRingPerpDistanceMeters(
                lons[i], lats[i],
                lons[span.first], lats[span.first],
                lons[span.last], lats[span.last]);
            if (d > bestDistance) {
                bestDistance = d;
                bestIndex = i;
            }
        }
        if (bestDistance > kLiteLargeRingSimplifyMeters && bestIndex > 0) {
            keep[bestIndex] = 1;
            stack.push({span.first, bestIndex});
            stack.push({bestIndex, span.last});
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        out.push_back(lons[i]);
        out.push_back(lats[i]);
        ++vertexCount;
    }
    out.push_back(lons[0]); // @Jin 恢复 GeoJSON 闭合环的重复首点。
    out.push_back(lats[0]);
    ++vertexCount;
}

// @Jin 将 GeoJSON Feature/Geometry 映射为引擎内部 VectorFeatureData，映射口径对齐 GDAL VectorReader：
// @Jin MultiPoint 拆多个点，MultiLineString 合并成多 parts，MultiPolygon 拆多个 Polygon 要素。
class GeoJsonEmitter {
public:
    GeoJsonEmitter(VectorReadResult &result, const std::string &labelField,
                   bool useExtent, double extentMinLon, double extentMinLat,
                   double extentMaxLon, double extentMaxLat,
                   const std::unordered_map<long long, std::string> *overrides,
                   int requestedMaxFeatures,
                   bool simplifyRemoteLargeRings)
        : result_(result), labelField_(labelField), useExtent_(useExtent),
          minLon_(extentMinLon), minLat_(extentMinLat), maxLon_(extentMaxLon), maxLat_(extentMaxLat),
          overrides_(overrides),
          featureCap_(requestedMaxFeatures > 0
                              ? std::min(requestedMaxFeatures, kMaxFeatures)
                              : kMaxFeatures),
          simplifyRemoteLargeRings_(simplifyRemoteLargeRings) {}

    bool truncated() const { return truncated_; }

    void emitRoot(const JsonValue &root) {
        if (!root.isObject()) return;
        const std::string rootType = root.at("type").isString() ? root.at("type").strVal : "";
        if (rootType == "FeatureCollection" && root.at("features").isArray()) {
            for (size_t i = 0; i < root.at("features").arr.size() && !truncated_; ++i) {
                emitFeature(root.at("features").arr[i], static_cast<long long>(i));
            }
        } else if (rootType == "Feature") {
            emitFeature(root, 0);
        } else {
            // @Jin 也接受裸 Geometry 对象，方便宿主把单个几何作为 GeoJSON 文件传入。
            emitGeometry(root, "", 0);
        }
    }

private:
    VectorReadResult &result_;
    std::string labelField_;
    bool useExtent_ = false;
    double minLon_ = 0.0, minLat_ = 0.0, maxLon_ = 0.0, maxLat_ = 0.0;
    const std::unordered_map<long long, std::string> *overrides_ = nullptr;
    int featureCap_ = kMaxFeatures;
    int featureCount_ = 0;
    int64_t vertexCount_ = 0;
    bool truncated_ = false;
    bool simplifyRemoteLargeRings_ = false; // @Jin 仅远程 URL 缓存大面环启用 75m 抽稀，本地高精度 GeoJSON 不改坐标。

    bool reachCap() const {
        return featureCount_ >= featureCap_ || (useExtent_ && vertexCount_ >= kMaxVertices);
    }

    // @Jin extent 重载时先按包围盒跳过屏外 Feature，避免大 GeoJSON 仍构建全部三角网。
    bool geometryVisible(const JsonValue &geometry) {
        if (!useExtent_) return true;
        Bounds bounds;
        collectPositionBounds(geometry.at("coordinates"), bounds);
        if (geometry.at("type").strVal == "GeometryCollection") {
            for (const JsonValue &child : geometry.at("geometries").arr) {
                Bounds childBounds;
                collectPositionBounds(child.at("coordinates"), childBounds);
                if (childBounds.intersects(minLon_, minLat_, maxLon_, maxLat_)) return true;
            }
            return false;
        }
        return bounds.intersects(minLon_, minLat_, maxLon_, maxLat_);
    }

    std::string labelFor(const JsonValue &properties, long long fid) const {
        if (overrides_ != nullptr) {
            const auto it = overrides_->find(fid);
            if (it != overrides_->end()) return it->second;
        }
        if (labelField_.empty() || !properties.isObject()) return "";
        return scalarToString(properties.at(labelField_));
    }

    // @Jin 统一做要素数/顶点数预算保护，防止 LITE 包在极端大文件上一次性占满内存。
    void publish(VectorFeatureData &feature) {
        if (reachCap()) {
            truncated_ = true;
            return;
        }
        ++featureCount_;
        if (feature.type == VectorGeomType::Point) {
            vertexCount_ += 1;
        } else if (feature.type == VectorGeomType::Line) {
            for (const auto &part : feature.parts) vertexCount_ += static_cast<int64_t>(part.size() / 2);
        } else {
            vertexCount_ += static_cast<int64_t>(feature.outer.size() / 2);
            for (const auto &hole : feature.holes) vertexCount_ += static_cast<int64_t>(hole.size() / 2);
        }
        result_.features.push_back(std::move(feature));
        if (reachCap()) truncated_ = true;
    }

    void emitFeature(const JsonValue &feature, long long fid) {
        if (truncated_ || !feature.isObject() || feature.at("type").strVal != "Feature") return;
        const JsonValue &geometry = feature.at("geometry");
        emitGeometry(geometry, labelFor(feature.at("properties"), fid), fid);
    }

    // @Jin 支持 P0 常见 GeoJSON 几何；未知类型安全跳过，不影响同文件其他要素。
    void emitGeometry(const JsonValue &geometry, const std::string &label, long long fid) {
        if (truncated_ || !geometry.isObject() || !geometry.at("type").isString()) return;
        if (!geometryVisible(geometry)) return;
        const std::string type = geometry.at("type").strVal;
        const JsonValue &coords = geometry.at("coordinates");

        if (type == "Point") {
            VectorFeatureData f;
            f.type = VectorGeomType::Point;
            f.fid = fid;
            f.label = label;
            double lon = 0.0, lat = 0.0;
            if (coordAt(coords, 0, lon) && coordAt(coords, 1, lat) &&
                lon >= -180.0 && lon <= 180.0 && lat >= -90.0 && lat <= 90.0) {
                f.lon = lon;
                f.lat = lat;
                publish(f);
            }
        } else if (type == "MultiPoint") {
            if (!coords.isArray()) return;
            for (const JsonValue &point : coords.arr) {
                if (truncated_) return;
                VectorFeatureData f;
                f.type = VectorGeomType::Point;
                f.fid = fid;
                f.label = label;
                double lon = 0.0, lat = 0.0;
                if (coordAt(point, 0, lon) && coordAt(point, 1, lat) &&
                    lon >= -180.0 && lon <= 180.0 && lat >= -90.0 && lat <= 90.0) {
                    f.lon = lon;
                    f.lat = lat;
                    publish(f);
                }
            }
        } else if (type == "LineString") {
            VectorFeatureData f;
            f.type = VectorGeomType::Line;
            f.fid = fid;
            f.label = label;
            f.parts.emplace_back();
            appendLine(coords, f.parts.back(), vertexCount_);
            if (!f.parts.back().empty()) publish(f);
        } else if (type == "MultiLineString") {
            if (!coords.isArray()) return;
            VectorFeatureData f;
            f.type = VectorGeomType::Line;
            f.fid = fid;
            f.label = label;
            for (const JsonValue &line : coords.arr) {
                f.parts.emplace_back();
                appendLine(line, f.parts.back(), vertexCount_);
                if (f.parts.back().empty()) f.parts.pop_back();
            }
            if (!f.parts.empty()) publish(f);
        } else if (type == "Polygon") {
            emitPolygon(coords, label, fid);
        } else if (type == "MultiPolygon") {
            if (!coords.isArray()) return;
            for (const JsonValue &polygon : coords.arr) {
                if (truncated_) return;
                emitPolygon(polygon, label, fid);
            }
        } else if (type == "GeometryCollection") {
            if (!geometry.at("geometries").isArray()) return;
            for (const JsonValue &child : geometry.at("geometries").arr) {
                if (truncated_) return;
                emitGeometry(child, label, fid);
            }
        }
    }

    // @Jin 计算闭合环的有向面积：RFC 7946 外环通常为负（CW），洞环为正（CCW），日志用于核对岛洞映射。
    static double ringSignedArea(const std::vector<double> &ring) {
        double area2 = 0.0;
        for (size_t i = 0; i + 3 < ring.size(); i += 2) {
            area2 += ring[i] * ring[i + 3] - ring[i + 2] * ring[i + 1];
        }
        return area2 * 0.5;
    }

    // @Jin 规范化在线 GeoJSON 环：剔除连续重复点并强制只保留一个闭合点，避免异常数据在 earcut/描边批次中生成飞线。
    static bool normalizeClosedRing(std::vector<double> &ring) {
        std::vector<double> cleaned;
        cleaned.reserve(ring.size());
        constexpr double kDupEpsDeg = 1e-12;
        for (size_t i = 0; i + 1 < ring.size(); i += 2) {
            const double lon = ring[i];
            const double lat = ring[i + 1];
            if (!std::isfinite(lon) || !std::isfinite(lat) ||
                lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0) {
                continue; // @Jin 单点非法不拖垮整个 Feature，其余有效环继续渲染。
            }
            if (cleaned.size() >= 2) {
                const double prevLon = cleaned[cleaned.size() - 2];
                const double prevLat = cleaned[cleaned.size() - 1];
                if (std::fabs(lon - prevLon) <= kDupEpsDeg &&
                    std::fabs(lat - prevLat) <= kDupEpsDeg) {
                    continue; // @Jin 连续重复坐标会产生退化三角形和异常 miter，这里在进入 earcut 前移除。
                }
            }
            cleaned.push_back(lon);
            cleaned.push_back(lat);
        }
        if (cleaned.size() < 6) return false; // @Jin 至少 3 个不重复坐标点才允许构成面环。
        // @Jin 若输入环错误地没有闭合，这里补齐；若已闭合则去掉重复尾点后再统一补回。
        if (std::fabs(cleaned[0] - cleaned[cleaned.size() - 2]) > kDupEpsDeg ||
            std::fabs(cleaned[1] - cleaned[cleaned.size() - 1]) > kDupEpsDeg) {
            cleaned.push_back(cleaned[0]);
            cleaned.push_back(cleaned[1]);
        } else {
            const double firstLon = cleaned[0];
            const double firstLat = cleaned[1];
            while (cleaned.size() >= 4 &&
                   std::fabs(cleaned[cleaned.size() - 2] - firstLon) <= kDupEpsDeg &&
                   std::fabs(cleaned[cleaned.size() - 1] - firstLat) <= kDupEpsDeg) {
                cleaned.pop_back();
                cleaned.pop_back();
            }
            cleaned.push_back(firstLon);
            cleaned.push_back(firstLat);
        }
        ring.swap(cleaned);
        return ring.size() >= 8;
    }

    static bool pointInClosedRing(double lon, double lat, const std::vector<double> &ring) {
        bool inside = false;
        for (size_t i = 0; i + 3 < ring.size(); i += 2) {
            const size_t j = i + 2; // @Jin 输入环末尾重复首点，j 自然指向下一条闭合边。
            const double xi = ring[i], yi = ring[i + 1];
            const double xj = ring[j], yj = ring[j + 1];
            const bool crosses = ((yi > lat) != (yj > lat)) &&
                                 (lon < (xj - xi) * (lat - yi) / ((yj - yi) == 0.0 ? 1e-300 : (yj - yi)) + xi);
            if (crosses) inside = !inside;
        }
        return inside;
    }

    static void ringRepresentativePoint(const std::vector<double> &ring, double &lon, double &lat) {
        double area2 = 0.0;
        double cx = 0.0;
        double cy = 0.0;
        for (size_t i = 0; i + 3 < ring.size(); i += 2) {
            const size_t j = i + 2;
            const double cross = ring[i] * ring[j + 1] - ring[j] * ring[i + 1];
            area2 += cross;
            cx += (ring[i] + ring[j]) * cross;
            cy += (ring[i + 1] + ring[j + 1]) * cross;
        }
        if (std::fabs(area2) > 1e-18) {
            lon = cx / (3.0 * area2);
            lat = cy / (3.0 * area2);
        } else {
            lon = ring[0]; // @Jin 退化环无法可靠求质心时，用首点做包含关系判断。
            lat = ring[1];
        }
    }

    // @Jin 标准 GeoJSON Polygon 只能有一个外环；遇到非标准数据把多个独立外环塞进一个 Polygon 时，
    // @Jin 按面积从大到小和“代表点是否位于已知外环内”重新分组，避免独立外环被误当洞而让 earcut 生成跨环桥接。
    void emitPolygon(const JsonValue &polygon, const std::string &label, long long fid) {
        if (!polygon.isArray() || polygon.arr.empty()) return;
        std::vector<std::vector<double>> rings;
        rings.reserve(polygon.arr.size());
        for (size_t i = 0; i < polygon.arr.size(); ++i) {
            rings.emplace_back();
            appendSimplifiedClosedLine(polygon.arr[i], rings.back(), vertexCount_, simplifyRemoteLargeRings_); // @Jin 远程外环/洞环使用同一 300m 阈值，关闭实验开关即可完整回退。
            if (!normalizeClosedRing(rings.back())) {
                LOGW("[LiteGeoJson] @Jin 跳过非法 Polygon ring fid=%lld ringIndex=%zu points=%zu",
                     fid, i, rings.back().size() / 2);
                rings.pop_back();
            }
        }
        if (rings.empty()) return;

        std::vector<size_t> order(rings.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return std::fabs(ringSignedArea(rings[a])) > std::fabs(ringSignedArea(rings[b]));
        });

        struct RingGroup {
            size_t outer = 0;
            std::vector<size_t> holes;
        };
        std::vector<RingGroup> groups;
        for (size_t idx : order) {
            double lon = 0.0, lat = 0.0;
            ringRepresentativePoint(rings[idx], lon, lat);
            size_t container = groups.size();
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (pointInClosedRing(lon, lat, rings[groups[gi].outer])) {
                    container = gi;
                    break;
                }
            }
            if (container == groups.size()) groups.push_back(RingGroup{idx, {}});
            else groups[container].holes.push_back(idx);
        }

        if (groups.size() > 1) {
            LOGW("[LiteGeoJson] @Jin 非标准 Polygon 含 %zu 个独立外环，fid=%lld 已拆分为多个面，避免岛洞误剖分",
                 groups.size(), fid);
        }

        for (RingGroup &group : groups) {
            VectorFeatureData f;
            f.type = VectorGeomType::Polygon;
            f.fid = fid;
            f.label = label;
            f.outer = std::move(rings[group.outer]);
            for (size_t hi : group.holes) f.holes.push_back(std::move(rings[hi]));
            LOGI("[LiteGeoJson] @Jin Polygon fid=%lld outerPts=%zu outerArea=%.8f holes=%zu holePts=%zu",
                 fid, f.outer.size() / 2, ringSignedArea(f.outer), f.holes.size(),
                 std::accumulate(f.holes.begin(), f.holes.end(), size_t{0},
                                 [](size_t n, const std::vector<double> &r) { return n + r.size() / 2; }));
            publish(f);
        }
    }
};

// @Jin 输出结果 bbox 供图层 RTC 原点和后续渲染裁剪使用，口径与 GDAL VectorReader 保持一致。
void expandResultBBox(VectorReadResult &result) {
    bool has = false;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    auto add = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        if (!has) {
            minX = maxX = x;
            minY = maxY = y;
            has = true;
        } else {
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
    };
    auto addRing = [&](const std::vector<double> &ring) {
        for (size_t i = 0; i + 1 < ring.size(); i += 2) add(ring[i], ring[i + 1]);
    };

    for (const VectorFeatureData &f : result.features) {
        if (f.type == VectorGeomType::Point) {
            add(f.lon, f.lat);
        } else if (f.type == VectorGeomType::Line) {
            for (const auto &part : f.parts) addRing(part);
        } else {
            addRing(f.outer);
            for (const auto &hole : f.holes) addRing(hole);
        }
    }
    result.hasBBox = has;
    result.minLon = minX;
    result.minLat = minY;
    result.maxLon = maxX;
    result.maxLat = maxY;
}

} // @Jin anonymous namespace


// @Jin LITE GeoJSON 读取入口：文件 → JsonParser DOM → VectorFeatureData → VectorBuilder。
VectorReadResult readLiteGeoJsonFile(const std::string &path,
                                     const std::string &labelField,
                                     bool hasExtent,
                                     double minLon, double minLat,
                                     double maxLon, double maxLat,
                                     int maxFeatures,
                                     const std::unordered_map<long long, std::string> *labelFieldOverrides) {
    VectorReadResult result;
    if (!isLiteGeoJsonPath(path)) {
        result.error = "当前为 LITE 构建（未链接 GDAL/OGR），仅支持 GeoJSON 文件";
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "GeoJSON 文件读取失败";
        return result;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        result.error = "GeoJSON 内容为空";
        return result;
    }

    const JsonValue root = parseJson(text);
    if (!root.isObject()) {
        result.error = "GeoJSON 解析失败：根节点必须是对象";
        return result;
    }
    const JsonValue &rootType = root.at("type");
    if (!rootType.isString()) {
        result.error = "GeoJSON 解析失败：缺少 type 字段";
        return result;
    }

    std::string normalizedPath = path;
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    const bool simplifyRemoteLargeRings =
        kLiteSimplifyRemoteLargeRings && normalizedPath.find("/url/") != std::string::npos; // @Jin 仅 RemoteVectorCache 的 URL 落盘目录允许超大环抽稀。
    GeoJsonEmitter emitter(result, labelField, hasExtent, minLon, minLat, maxLon, maxLat,
                           labelFieldOverrides, maxFeatures, simplifyRemoteLargeRings);
    emitter.emitRoot(root);
    if (result.features.empty()) {
        result.error = hasExtent ? "当前范围内没有 GeoJSON 要素" : "GeoJSON 中没有可渲染要素";
        return result;
    }

    expandResultBBox(result);
    result.ok = true;
    result.hasElevation = false; // @Jin RFC 7946 三维坐标 P0 仍按贴地处理，后续如需要再接入逐顶点高程。
    LOGI("[LiteGeoJson] 读取完成 features=%zu bbox=[%.6f,%.6f,%.6f,%.6f] path=%s",
         result.features.size(), result.minLon, result.minLat, result.maxLon, result.maxLat,
         path.c_str());
    return result;
}

} // @Jin namespace globecore
