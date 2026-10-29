# 在线 GeoJSON 矢量数据源增强方案

> @Jin 本文档由侧边栏分析整理，供主聊天线程读取后实施。目标是在原生层增强 GlobeCore 的 `addVectorLayer` 能力，使其内部识别并加载 `http://` / `https://` 的 GeoJSON 地址；当前主线为 HarmonyOS NEXT HAR/NAPI，Android JNI 同步保留跨平台预埋。
>
> 状态：@Jin P0 已在 `feature/geojson-url-data` 分支实施并完成鸿蒙真机技术验收：Android/JNI 路径已完成编译检查，HarmonyOS NEXT LITE 已打通并验证 `ArkTS addVectorLayer(URL) → NAPI vectorCacheDir → 共享 C++ 下载缓存 → LITE 原生 GeoJSON reader → VectorBuilder/VBO`；首次下载、重启后缓存命中、4 省边界显示和 3D 地形下显示均已通过，等待用户最终人工确认后提交。  
> 日期：2026-10-28  
> 适用仓库：`E:\WorkBuddy\qt_ohos_map_sdk\vendor\GlobeCore`  
> 实施要求：实验性能力、可回退；新增/修改代码注释必须带 `@Jin`；定稿后同步更新 `README.md` 与 Android API HTML 文档。

---

## 1. 背景与结论

当前 Android 公开入口 `NativeMapView.addVectorLayer(path, style, ...)` 的 `path` 语义主要是本地文件路径。native 层最终在 `VectorReader.cpp` 中直接执行：

```cpp
GDALDataset *ds = (GDALDataset *) GDALOpenEx(
    path.c_str(),
    GDAL_OF_VECTOR | GDAL_OF_READONLY,
    nullptr,
    nullptr,
    nullptr
);
```

现有工程虽然静态链接了 `libcurl`，但 curl 主要服务于 XYZ 栅格瓦片和 Cesium terrain Provider，不代表 GDAL/OGR 已启用 `/vsicurl/`。GDAL 构建脚本中未显式看到 `GDAL_USE_CURL=ON` 等配置，因此不能承诺 `GDALOpenEx("http://...")` 或 `GDALOpenEx("/vsicurl/https://...")` 可跨平台稳定工作。

**推荐结论：**

> 借鉴 MapLibre Native 的在线 GeoJSON Source 生命周期，但不要直接依赖 GDAL `/vsicurl/`。在 GlobeCore native 层先识别 HTTP/HTTPS URL，通过现有 `HttpClient` 后台下载到本地缓存文件，再复用现有 `readVectorFile(localCachePath) → buildGeometry → Renderer VBO` 管线。

第一期只支持公开 GeoJSON URL；后续再扩展 headers、token、ETag、强制刷新、进度回调等能力。

---

## 2. MapLibre Native 参考机制

参考仓库：<https://github.com/maplibre/maplibre-native>

关键文件：

- `include/mln/style/sources/geojson_source.hpp`
- `src/mln/style/sources/geojson_source.cpp`
- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/style/sources/GeoJsonSource.kt`

### 2.1 明确区分内联数据与 URL 数据

MapLibre Android 中，内联 JSON 字符串不能以 `http` 开头；在线地址走独立的 `setUri`：

```kotlin
constructor(id: String?, geoJson: String?) : super() {
    require(!(geoJson == null || geoJson.startsWith("http"))) {
        "Expected a raw json body"
    }
    initialize(id, null)
    setGeoJson(geoJson)
}

fun setUri(uri: String?) {
    checkThread()
    nativeSetUrl(uri)
}
```

支持的 URI 类型包括：

- `http://`
- `https://`
- `file://`
- `asset://`

### 2.2 Core 层异步请求、后台解析、完成后替换

MapLibre Core C++ 的 `GeoJSONSource` 内部维护：

```cpp
std::optional<std::string> url;
std::unique_ptr<AsyncRequest> req;
std::atomic<uint64_t> requestGeneration{0};
```

核心流程：

```cpp
req = fileSource.request(Resource::source(*url), [this](const Response& res) {
    if (res.error) {
        observer->onSourceError(...);
    } else if (res.notModified) {
        return;
    } else if (res.noContent) {
        observer->onSourceError(...);
    } else {
        Scheduler::GetBackground()->scheduleAndReplyValue(
            ...,
            [data = res.data]() {
                // 后台解析 GeoJSON，创建 GeoJSONData
                conversion::convertJSON<GeoJSON>(*data, error)
                GeoJSONData::create(*geoJSON, ...)
                return makeMutable<Impl>(...);
            },
            [capturedReqGeneration = ++requestGeneration](newImpl) {
                // 只接受最新请求的结果
                if (capturedReqGeneration == requestGeneration) {
                    baseImpl = std::move(newImpl);
                    loaded = true;
                    observer->onSourceLoaded(*this);
                }
            });
    }
});
```

### 2.3 应借鉴的设计点

1. URL 是数据源描述，不等同于 GeoJSON 内容。
2. 网络获取与解析/构建解耦。
3. 网络请求在后台执行，不阻塞渲染线程。
4. 下载完成后在后台解析和构建数据。
5. 成功后一次性替换旧数据。
6. 失败返回明确错误。
7. 用 generation / latest-wins 丢弃过期响应。
8. 内联数据、在线 URL、内存对象是不同输入路径。

GlobeCore 第一阶段不需要照搬 MapLibre 的 GeoJSON-VT、cluster、feature state 等高级能力；只借鉴在线 Source 的获取和生命周期模型。

---

## 3. GlobeCore 现状链路

当前文件矢量链路：

```text
NativeMapView.addVectorLayer(path, style, extent, maxFeatures, ...)
    ↓ JNI
GlobeEngine::addVectorLayer(...)
    ↓
VectorLayer(path, style, extent...)
    ↓
VectorLoader::start()
    ↓ 后台线程
VectorLoader::run()
    ↓
readVectorFile(path_, ...)
    ↓
GDALOpenEx(path)
    ↓
OGR 读取、过滤、重投影、属性/标注处理
    ↓
buildGeometry(r, style, &stale_)
    ↓
VectorGeometry Ready
    ↓ GL 线程
Renderer::drainReady / 上传 VBO / 拾取 / 贴地
```

`VectorLoader` 已具备可复用能力：

- 后台线程；
- `Idle / Loading / Ready / Failed / Consumed` 状态；
- `abort_` 析构中断；
- `stale_` 范围更新抢占；
- Swap-on-ready；
- 旧几何在新几何 Ready 前继续显示；
- `error_` 保存可展示错误；
- Renderer 通过 `hasPendingOrReady()` 持续重绘直到上传完成。

因此最合适的改造位置是：**在 `VectorLoader` 读取前增加远程源准备阶段**。

---

## 4. 不直接使用 GDAL `/vsicurl/` 的原因

### 4.1 GDAL curl 支持未明确启用

当前 GDAL 构建配置中显式启用了 SQLite/GPKG/DXF/LIBKML 等，但未看到：

```bash
-DGDAL_USE_CURL=ON
-DCURL_INCLUDE_DIR=...
-DCURL_LIBRARY=...
```

因此 `/vsicurl/https://...` 是否可用不能作为稳定 API 承诺。

### 4.2 请求头和 token 不灵活

未来政务/内网 GeoJSON 服务可能需要：

- `Authorization`
- `Token`
- `Referer`
- `User-Agent`

GlobeCore terrain Provider 已有 headers 机制，GeoJSON 在线源也应保持一致。

### 4.3 缓存和请求量不可控

项目已重点优化在线瓦片请求次数和缓存策略。GeoJSON 也应可控地：

- 首次下载；
- 后续读缓存；
- 离线可用；
- 可校验 ETag / Last-Modified；
- 可统计网络请求；
- 可避免 extent 高频变化导致重复下载。

### 4.4 大 GeoJSON 需要响应校验

需要在进入 GDAL 前识别：

- HTTP 状态码；
- 空响应；
- 登录页 HTML；
- 错误 JSON；
- Content-Length 超限；
- 非 GeoJSON 内容；
- gzip / 重定向等行为。

### 4.5 本地缓存路径可复用现有所有能力

下载成 `.geojson` 后，可继续复用：

- GDAL/OGR 读取；
- 空间过滤；
- 标注；
- KML 样式逻辑；
- 拾取；
- 属性查询；
- 后续 `NativeVector` 文件 API；
- 3D 贴地；
- 错误日志。

---

## 5. 推荐目标流程

### 5.1 总体流程

```text
addVectorLayer("https://example.com/a.geojson")
    ↓
native 识别 HTTP/HTTPS
    ↓
VectorLoader 进入远程源准备阶段
    ↓
计算 URL 对应本地缓存路径
    ↓
缓存存在且可用？
  ├─ 是：直接 readVectorFile(cachePath, extent)
  └─ 否：HttpClient::get(url)
          ↓
       校验响应、大小、JSON 嗅探
          ↓
       原子写入 cachePath
          ↓
       readVectorFile(cachePath, extent)
    ↓
buildGeometry
    ↓
Ready swap → Renderer 上传 VBO
```

### 5.2 extent 更新策略

在线 GeoJSON 文件下载和视口空间过滤必须解耦：

```text
第一次加载：URL → 下载 → cache.geojson → read(cache.geojson, extent1)

相机移动 / updateVectorExtent：
read(cache.geojson, extent2)
不得重新下载 URL
```

即：

- URL/source 变化才重新下载；
- extent 变化只重复本地 OGR 读取/构建；
- 大范围移动不能造成同一 GeoJSON URL 的网络风暴。

---

## 6. API 设计

### 6.1 第一阶段保持业务调用兼容

目标业务调用：

```kotlin
val index = map.addVectorLayer(
    path = "http://210.74.129.84:8520/app-pub/web-data/geojson/district/province.json",
    style = VectorStyle()
)
```

不要求业务先下载。

### 6.2 默认缓存目录

`NativeMapView` 已有 Android `Context`，可生成默认目录：

```kotlin
context.filesDir
    .resolve("globecore")
    .resolve("vector-cache")
    .absolutePath
```

native 内实际文件建议：

```text
<vector-cache>/url/<sha256>.geojson
<vector-cache>/url/<sha256>.meta.json
```

### 6.3 JNI 参数策略

当前 `nativeAddVectorLayer` 参数已经很多。为了第一阶段少改 API，可在 JNI 末尾追加默认缓存目录参数：

```cpp
jstring vectorCacheDir
```

但从长期维护看，更推荐逐步引入 options 结构体。可选扩展：

```kotlin
data class VectorLoadOptions(
    val extent: VectorExtent? = null,
    val maxFeatures: Int = 0,
    val networkCacheDir: String? = null,
    val networkHeaders: Map<String, String> = emptyMap(),
    val networkTimeoutMs: Long = 20_000,
    val useCache: Boolean = true,
    val refresh: Boolean = false,
    val maxNetworkBytes: Long = 80L * 1024L * 1024L,
    val experimentalRemoteSource: Boolean = true
)
```

考虑当前 API 兼容，第一阶段可先不改 Kotlin 方法签名，只内部传默认 cacheDir；第二阶段再增加 headers/timeout。

---

## 7. Native 模块设计

### 7.1 新增文件建议

新增：

```text
globecore/src/main/cpp/vector/VectorSourceResolver.h
globecore/src/main/cpp/vector/VectorSourceResolver.cpp
globecore/src/main/cpp/vector/RemoteVectorCache.h
globecore/src/main/cpp/vector/RemoteVectorCache.cpp
```

`CMakeLists.txt` 加入：

```cmake
vector/VectorSourceResolver.cpp
vector/RemoteVectorCache.cpp
```

### 7.2 VectorSourceResolver 职责

建议接口：

```cpp
namespace globecore {

struct ResolvedVectorSource {
    bool remote = false;
    std::string originalSource;
    std::string effectivePath;
    std::string cachePath;
    std::string cacheMetaPath;
    std::string safeLogName;
};

class VectorSourceResolver {
public:
    static bool isHttpUrl(const std::string &source);
    static ResolvedVectorSource resolve(
        const std::string &source,
        const std::string &vectorCacheDir
    );
    static std::string sha256OrFallback(const std::string &url);
    static std::string redactUrlForLog(const std::string &url);
};

} // namespace globecore
```

第一阶段如果没有 SHA256 工具，也可用已有 hash 或 URL 安全编码，但优先使用标准/已有库生成稳定 hash，避免 token 泄露和路径非法。

### 7.3 RemoteVectorCache 职责

建议接口：

```cpp
struct RemoteVectorFetchOptions {
    std::string cacheDir;
    std::vector<std::pair<std::string, std::string>> headers;
    long timeoutMs = 20000;
    size_t maxBytes = 80u * 1024u * 1024u;
    bool useCache = true;
    bool refresh = false;
};

struct RemoteVectorFetchResult {
    bool ok = false;
    bool cacheHit = false;
    std::string localPath;
    std::string error;
    size_t bytes = 0;
    long long elapsedMs = 0;
};

class RemoteVectorCache {
public:
    static RemoteVectorFetchResult prepareGeoJson(
        const std::string &url,
        const RemoteVectorFetchOptions &options,
        const std::atomic<bool> &abortFlag
    );
};
```

### 7.4 缓存文件写入

必须原子写，避免半文件被后续读取：

```text
下载到：<name>.geojson.tmp.<threadid>
写完并 flush/close 后 rename：<name>.geojson
```

伪代码：

```cpp
const std::string tmp = cachePath + ".tmp." + std::to_string(gettid());
writeFile(tmp, bytes);
rename(tmp, cachePath);
```

### 7.5 JSON 内容嗅探

第一阶段可做轻量检查：

1. 跳过 UTF-8 BOM；
2. 跳过 ASCII 空白；
3. 首字符必须是 `{` 或 `[`；
4. 必须包含 `"type"`；
5. FeatureCollection 可进一步检查 `"FeatureCollection"`、`"Feature"`、`"geometry"`。

不要把 Content-Type 作为唯一依据，很多内网服务可能返回：

```text
application/octet-stream
text/plain
```

但如果首段是 `<html`、`<!DOCTYPE HTML`，应直接报错。

---

## 8. VectorLoader 改造方案

### 8.1 字段扩展

`VectorLoader` 需要区分：

```cpp
std::string sourcePath_;   // 原始本地路径或 URL
std::string effectivePath_; // 远程下载完成后的本地缓存路径；本地源即 sourcePath_
std::string vectorCacheDir_;
bool remoteSource_ = false;
```

现有 `path_` 可逐步替换命名，避免误用。

### 8.2 状态扩展

当前：

```cpp
enum class State { Idle, Loading, Ready, Failed, Consumed };
```

建议改成：

```cpp
enum class State {
    Idle,
    Downloading,
    Reading,
    Building,
    Ready,
    Failed,
    Consumed
};
```

如果不想影响 Renderer，对 `hasPendingOrReady()` 来说，Downloading/Reading/Building 都算 pending。

### 8.3 run 流程

伪代码：

```cpp
void VectorLoader::run() {
    for (;;) {
        snapshotRequest();

        std::string readPath = sourcePath_;

        if (remoteSource_) {
            if (effectivePath_.empty() || !fileExists(effectivePath_)) {
                state_ = State::Downloading;
                auto fetched = RemoteVectorCache::prepareGeoJson(
                    sourcePath_,
                    fetchOptions,
                    abort_
                );

                if (abort_ || sourceInvalid_) return;

                if (!fetched.ok) {
                    error_ = fetched.error;
                    state_ = State::Failed;
                    return;
                }

                effectivePath_ = fetched.localPath;
            }
            readPath = effectivePath_;
        }

        state_ = State::Reading;
        VectorReadResult r = readVectorFile(readPath, ...);

        if (!r.ok) {
            state_ = State::Failed;
            error_ = r.error;
            return;
        }

        if (stale_) {
            // 下载结果可保留；只读/构建过期，使用最新 extent 重读本地缓存
            continue;
        }

        state_ = State::Building;
        VectorGeometry g = buildGeometry(r, style_, &stale_);

        if (stale_) {
            continue;
        }

        publishReady(g);
        if (rerunRequested_) continue;
        return;
    }
}
```

### 8.4 abort / stale 区分

需要区分三类中断：

1. `abort_`：图层销毁，下载和读取都应停止；
2. `sourceInvalid_`：URL/source 已切换，旧下载结果丢弃；
3. `stale_`：extent 过期，下载好的缓存仍有价值，但本次读取/构建结果丢弃并用最新 extent 重来。

**不要因为 extent 变化而取消 URL 下载。**

---

## 9. 错误处理建议

错误信息应细分，便于真机排查：

| 阶段 | 建议错误 |
|---|---|
| URL 非 http/https | `在线矢量地址协议不支持` |
| URL 无 host | `在线矢量地址无效` |
| 网络失败 | `在线 GeoJSON 下载失败` |
| HTTP 非 2xx | `在线 GeoJSON 请求失败：HTTP 404` |
| 空响应 | `在线 GeoJSON 内容为空` |
| 超过大小限制 | `在线 GeoJSON 超过大小限制` |
| 返回 HTML | `在线地址返回的不是 GeoJSON，可能是登录页或错误页` |
| JSON 嗅探失败 | `在线数据不是有效 GeoJSON` |
| 写缓存失败 | `在线 GeoJSON 缓存写入失败` |
| GDAL 读取失败 | 沿用 `打开矢量数据失败`，日志带本地缓存路径 |

日志中 URL query 必须脱敏，避免 token 泄露：

```text
https://host/path/province.json?token=***
```

---

## 10. 实验开关与回退

第一阶段作为实验性能力，必须可回退。

可先在 native 加常量：

```cpp
// @Jin 实验开关：在线 GeoJSON Source 首版默认开启；如出现兼容问题可快速关闭回退到纯本地路径语义。
static constexpr bool kExperimentalRemoteGeoJsonSource = true;
```

开关关闭时：

- HTTP URL 不做下载；
- 保持旧行为，交给 `GDALOpenEx`，大概率失败；或直接返回“不支持在线地址”；
- 本地 `.geojson/.shp/.kml/...` 行为完全不变。

更理想是后续由 Kotlin options 控制：

```kotlin
experimentalRemoteSource: Boolean = true
```

---

## 11. 分阶段实施清单

### P0：最小可用闭环（建议先做）

- [x] @Jin 新增 `VectorSourceResolver`，识别 `http://` / `https://`。
- [x] @Jin 新增 `RemoteVectorCache`，复用 `HttpClient::get` 下载公开 URL。
- [x] @Jin 默认缓存目录由宿主应用私有目录生成：Android `NativeMapView.context.filesDir` 自动通过 JNI 传入；HarmonyOS NEXT 由 Demo/宿主通过 `NativeMapView.vectorCacheDir` 经 NAPI 传入。
- [x] @Jin `VectorLoader` 保存原始 source 和下载后的 effective local path。
- [x] @Jin 第一次远程加载下载到本地 `.geojson` 缓存。
- [x] @Jin 后续 `updateVectorExtent()` 只读缓存，不重复联网；缓存命中使用首尾轻量校验，避免每次重载整文件读取。
- [x] @Jin 原子写缓存文件。
- [x] @Jin 增加大小限制、空响应、HTML/JSON 基础嗅探。
- [x] @Jin 析构时中断下载。
- [x] @Jin 加入实验开关。
- [x] @Jin 所有新增/修改代码注释带 `@Jin`。
- [x] @Jin 更新 `README.md`，记录实验性能力和回退方式。
- [x] @Jin 更新 `docs/api/android-api-guide.html`。
- [ ] @Jin Android native 真机验证公开 province.json：当前电脑无 Android ADB 设备且缺少 Android 预编译 `.a` 依赖，尚未闭环。
- [x] @Jin HarmonyOS NEXT NAPI 第 27 个参数增加 `vectorCacheDir`，ArkTS `NativeMapView` 组件由宿主注入应用私有缓存目录。
- [x] @Jin HarmonyOS NEXT LITE 新增无 GDAL 依赖的 `ohos/lite_geojson_reader.cpp`，支持常见 GeoJSON 几何并复用普通 VectorBuilder/VBO/拾取/贴地管线。
- [x] @Jin Harmony Demo 的“省级GeoJSON”改为直接调用 `map.addVectorLayer(PROVINCE_GEOJSON_URL, style)`，删除 ArkTS 下载、JSON.parse 扁平化和 overlay 兜底路径。
- [x] @Jin 鸿蒙真机验证首次下载、二次缓存、`[RemoteGeoJson]`、`[LiteGeoJson]`、`[VecReload]` 日志和 4 省边界显示。
  - @Jin 2026-10-28 设备 `5KLBB25911200313`：首次 native 下载 669,489 bytes，解析 4 个 MultiPolygon；force-stop 重启后同 URL `[RemoteGeoJson] cache hit ... elapsed=0ms`，`read=140ms build=128ms total=268ms`；截图确认四省黄色边界。
  - @Jin 3D 地形开启并缩放到四省范围时，省级在线图层仍保持显示；当前验收未覆盖断网启动、404/HTML/超限等错误场景，保留到 P1/专项测试。

### P1：可靠性增强

- [ ] 增加下载耗时、字节数、缓存命中、GDAL 读取、构建耗时日志。
- [ ] 网络失败时 stale cache 兜底。
- [ ] 写 `.meta.json`，记录 URL、HTTP 状态、ETag、Last-Modified、下载时间、字节数。
- [ ] Android Kotlin 与 Harmony ArkTS API 增加 `networkTimeoutMs`、`maxNetworkBytes`。
- [ ] 增加 source generation，切换 URL 时丢弃旧响应。
- [ ] 对错误页 HTML、JSON error、空 FeatureCollection 做更细提示。

### P2：请求头与缓存校验

- [ ] Android Kotlin 与 Harmony ArkTS API 增加 `networkHeaders`。
- [ ] JNI/NAPI flatten headers，和 terrain Provider 风格一致。
- [ ] 支持 `refresh=true`。
- [ ] 支持 ETag / If-None-Match。
- [ ] 支持 Last-Modified / If-Modified-Since。
- [ ] 304 Not Modified 时复用本地缓存。
- [ ] 统计在线 GeoJSON 网络请求次数。

### P3：高级能力

- [ ] 下载进度回调。
- [ ] 内存 GeoJSON 字符串 / FeatureCollection 直接建层。
- [ ] `/vsimem/` 模式，减少磁盘占用。
- [ ] 大 GeoJSON 流式解析和分块上传。
- [ ] 参考 MapLibre GeoJSON-VT 做索引、聚合和动态数据更新。

---

## 12. 需要改动的文件

预计代码文件：

```text
globecore/src/main/cpp/vector/VectorSourceResolver.h
globecore/src/main/cpp/vector/VectorSourceResolver.cpp
globecore/src/main/cpp/vector/RemoteVectorCache.h
globecore/src/main/cpp/vector/RemoteVectorCache.cpp
globecore/src/main/cpp/layer/VectorLoader.h
globecore/src/main/cpp/layer/VectorLoader.cpp
globecore/src/main/cpp/layer/VectorLayer.h
globecore/src/main/cpp/core/GlobeEngine.h
globecore/src/main/cpp/core/GlobeEngine.cpp
globecore/src/main/cpp/jni/GlobeEngineJni.cpp
globecore/src/main/java/com/zys/globecore/NativeLib.kt
globecore/src/main/java/com/zys/globecore/NativeMapView.kt
globecore/src/main/cpp/CMakeLists.txt
```

文档文件：

```text
README.md
docs/api/android-api-guide.html
```

可选 demo 验证：

```text
globecore-android/... 地图页面中“省级 GeoJSON”按钮改为直接传在线 URL
```

---

## 13. 真机验证建议

### 13.1 首次在线加载

1. 清除应用数据或清空 vector-cache。
2. 打开 demo。
3. 点击省级 GeoJSON，URL 使用：

```text
http://210.74.129.84:8520/app-pub/web-data/geojson/district/province.json
```

4. 观察日志：
   - 识别为 remote；
   - HTTP 200；
   - bytes；
   - 写缓存；
   - GDAL/OGR 或 Harmony LITE GeoJSON features；
   - buildGeometry；
   - 图层显示。

### 13.2 二次缓存命中

1. 关闭页面或重启 App。
2. 再次加载同一 URL。
3. 期望：不发 GeoJSON 网络请求，直接读缓存。

### 13.3 extent 重载

1. 加载在线 GeoJSON 后移动、缩放地图。
2. 触发 `updateVectorExtent()`。
3. 期望：Android 只有本地 GDAL 读取/构建，Harmony LITE 只有本地 GeoJSON reader 读取/构建，不重复下载 URL。

### 13.4 离线

1. 首次成功加载后断网。
2. 重启 App 再加载同一 URL。
3. 期望：缓存命中并显示，错误不影响地图。

### 13.5 错误场景

验证：

- 404；
- 服务器返回 HTML；
- 空文件；
- 非 GeoJSON JSON；
- 超大文件；
- 加载中销毁 Activity；
- 快速连续切换两个 URL；
- 在线加载过程中移动地图。

### 13.6 3D 地形贴地

使用太白山示例点、线、面和省级 GeoJSON：

- 2D 正常；
- 3D 开启地形后动态 overlay 贴地逻辑不受影响；
- 大省级面不应卡死；
- 如顶点量大，沿用已有大 GeoJSON 顶点预算保护。

---

### 13.7 @Jin 2026-10-28 鸿蒙真机实测记录

- 设备：`5KLBB25911200313`，Demo 包：`com.example.globecoredemo`。
- 首次在线加载：`[RemoteGeoJson] downloaded ... bytes=669489 elapsed=1465ms`，随后 `[LiteGeoJson] 读取完成 features=4 bbox=[105.488260,31.706680,111.241870,39.585240]`，并进入 `[VecReload] VectorLoader 就绪`。
- 重启后缓存：force-stop 并重新启动 App，再次点击省级 GeoJSON 后日志为 `[RemoteGeoJson] cache hit ... bytes=669489 elapsed=0ms`，随后 LITE reader 读取缓存，`read=140ms build=128ms total=268ms`，未再触发 GeoJSON 网络下载。
- 视觉结果：全球视角可见中国中部四省黄色边界；加载 210 地形源并在 3D 下缩放后，省级在线图层仍显示，没有回退到 ArkTS overlay。
- 回归观察：太白山点、线、面、POI 图标动态 overlay 样例仍可显示在地形上；本地 rawfile `sample.geojson` 与远程缓存文件共用 LITE `.geojson/.json` 路由，桌面真实数据冒烟通过，UI 人工回归可作为提交前补充项。
- 未覆盖项：断网启动、错误 URL、HTML 登录页、超大文件、快速切换 URL、headers/token、ETag/304 和下载进度，仍按 P1/P2/P3 拆分。

---

## 14. 给主聊天线程的执行指令摘要

主聊天可按以下摘要开始：

```text
请读取 docs/design/online-geojson-vector-proposal.md，按 P0 最小可用闭环实施在线 GeoJSON 支持。
要求：
1. 在 native 层增强 addVectorLayer，识别 http/https URL；
2. 不直接依赖 GDAL /vsicurl，使用现有 HttpClient 下载到应用私有 vector-cache；
3. 下载后复用 readVectorFile → buildGeometry → Renderer 管线；
4. 同一 URL 只下载一次，后续 updateVectorExtent 只读本地缓存；
5. 作为实验性能力，加开关，保证可回退；
6. 处理 abort、stale、HTML/空响应/超限等错误；
7. 所有新增或修改代码注释加 @Jin；
8. 定稿后更新 README.md 与 docs/api/android-api-guide.html；
9. 完成后给出真机验证步骤和待办清单，不要自动提交，等待确认。
```

---

## 15. 最终建议

采用 MapLibre Native 的：

```text
URL Source → 异步网络请求 → 后台解析构建 → latest-wins → 成功后替换 → 错误回调
```

但 GlobeCore 第一阶段落地为：

```text
HTTP URL → HttpClient 下载 → 原子写本地缓存 → Android GDAL/OGR 或 Harmony LITE GeoJSON reader 读取缓存 → 现有 VectorGeometry/VBO 管线
```

这是当前风险最低、最贴合现有架构、最容易验证和回退的方案。
