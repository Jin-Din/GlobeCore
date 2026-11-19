# @Jin 专用 GeoJSON 图层接口（addGeoJSONLayer）设计与任务分解

> 状态：Harmony 真机回归通过（T0–T14 完成），T15 Android full 构建回归待补。实验性/可回退。
> 日期：2026-11-18。
> 评审定稿：平台 API 采用 `addGeoJSONLayer(url, data, style, ...)` 双参互斥方案（不引入来源描述类）。
> 编译验证：OHOS clang 语法检查全过；hvigor `globecore:assembleHar` 与 `entry:assembleHap` 构建成功；
> Android NDK clang JNI 单元语法通过、`compileDebugKotlin` 通过（native 链接缺预构建 libcurl.a，属环境缺件）。
> 真机验证（2026-11-19，设备 `5KLBB25911200313`，demo 工程 `demo/GlobeCoreDemo` 签名 HAP）：
> 三来源全部通过，详见 `docs/jin-modification-records.md` 第十二节。
> 关联文档：`online-geojson-vector-proposal.md`（P0 在线 GeoJSON，已交付）、`upstream-sync-assessment.md`（2026-11-13 上游同步评估）。
> 目标分支：从最新 `main` 切 `feature/geojson-layer-api`（`feature/geojson-url-data` 已并入 main，不在旧基点开发）。

## 1. 背景与目标

### 1.1 现状问题

- `addVectorLayer` 是上游原生「本地 OGR 矢量文件」入口，P0 在线 GeoJSON 改造直接把 URL 识别、下载缓存、LITE 专属 reader 路由塞进了这条通路，导致：
  - 数据源职责混杂：定位符（本地路径 / http(s) URL）与即将新增的内存内容（GeoJSON 字符串）没有清晰边界；字符串若继续塞进 `path` 只能做前缀嗅探，判定脆弱、错误信息含糊。
  - 上游同步冲突面大：`VectorLoader`、`GlobeEngine`、JNI/NAPI 门面中的 @Jin 改造与上游同名通路耦合（参见上游同步评估中的 12 文件 16 冲突块）。
  - LITE 构建下 `lite_gdal_stubs.cpp` 按文件后缀路由 GeoJSON，`addVectorLayer` 的实际能力与方法名/文档语义不符。

### 1.2 目标

- 新增专用接口 `addGeoJSONLayer`，明确承载三种 GeoJSON 来源：本地 `.geojson/.json` 文件、在线 `http(s)://` URL、GeoJSON 结构字符串。
- 底层复用现有矢量图层容器与渲染管线（VBO/拾取/贴地/标注/extent 重载），不新增并行渲染体系。
- `addVectorLayer` 回退为上游原生语义：仅本地 OGR 文件（full 构建多格式；LITE 构建返回中文错误、不崩溃）。URL 能力经一个版本的弃用周期后移除。
- 缩小与上游 `z-gis/GlobeCore` 的同步差异面。

### 1.3 非目标（本期不做）

- 不做 P1/P2/P3 网络能力（headers/token、ETag/304、强制刷新、进度回调、stale 兜底、流式解析、GeoJSON-VT 聚合），仅在新接口上预留演进位置。
- 不改 `Renderer`/`VectorBuilder`/earcut/地形贴地等格式无关代码。
- 不改 `addOverlayLayer`（宿主自解析内存几何）与 `pickVector` 等索引共享 API 的对外行为。

## 2. 现状路由梳理（改造基线）

### 2.1 数据源接入层（本次要迁移的部分）

| 文件 | 现状职责 | 处置 |
|---|---|---|
| `globecore/src/main/cpp/vector/VectorSourceResolver.{h,cpp}` | http(s) 前缀判定、FNV-1a 缓存键、日志脱敏 | 迁入 GeoJSON 专用通路，扩展 source kind |
| `globecore/src/main/cpp/vector/RemoteVectorCache.{h,cpp}` | URL 下载、80MB/20s 限制、原子落盘 | 迁入 GeoJSON 专用通路；字符串落盘复用其原子写工具 |
| `globecore/src/main/cpp/layer/VectorLoader.{h,cpp}` | `remoteSource_` 分支：下载→缓存→读文件 | 改为按显式 source kind 分发，移除 URL 嗅探隐式分支 |
| `globecore-harmonyos/.../ohos/lite_gdal_stubs.cpp` | `hasGeoJsonExtension()` 后缀路由到 `readLiteGeoJsonFile` | 路由判定上移到新通路；桩内只保留 reader 调用与中文报错 |
| `globecore-harmonyos/.../ohos/lite_geojson_reader.{h,cpp}` | LITE 无 GDAL 的 GeoJSON 解析 | 不动；新增字符串入口（见 T4） |
| `globecore/.../core/GlobeEngine.{h,cpp}` | `addVectorLayer(..., vectorCacheDir)` | 新增 `addGeoJSONLayer`；旧方法去掉 cacheDir 透传（弃用期保留重载） |
| `globecore/.../jni/GlobeEngineJni.cpp`、`ohos/napi_globe_engine.cpp` | 27 参数门面、cacheDir 透传 | 新增 `nativeAddGeoJSONLayer`；旧门面弃用期保留 |
| `globecore/.../net/HttpClient.{h,cpp}` | 下载能力 | 不动（通用网络组件，terrain 也在用） |

### 2.2 渲染与图层容器（本次不动、两接口共用）

- `render/Renderer.{h,cpp}`（VBO、拾取、draped 贴地、`603d531` 本地纯线修复）、`vector/VectorBuilder.cpp`、`vector/VectorGeometry.h`、`VectorStyle`。
- 图层 index 空间、`setVectorLayerVisible`、`setVectorMinLevel`、`updateVectorExtent`、`hasVectorLoading`、`removeVectorLayer`、`pickVector/pickVector3D` 对新接口原样生效。

## 3. API 设计

### 3.1 数据源参数（评审定稿：url/data 双参，互斥）

不引入来源描述类，`addGeoJSONLayer` 直接以两个数据源参数承载三种来源：

- `url`：定位符，支持本地文件绝对路径与 `http://`/`https://` 在线地址（native 内部仍以 http 前缀区分本地与联网）。
- `data`：GeoJSON 结构字符串（Feature/FeatureCollection/Geometry 对象文本）。
- 互斥规则：二者必须且只能传一个非空值；同时非空或同时为空直接返回 `-1` 并写中文错误，不做 `{` 前缀嗅探式猜测。
- native 内部仍以显式 `SourceKind{File,Url,Data}` 枚举分发，嗅探只作用于 `url` 单值的 http 前缀判定，`data` 通道独立。

### 3.2 新接口签名（两端同构）

```ts
addGeoJSONLayer(url: string, data: string, style: VectorStyle,
  iconArgb?: number[], iconW?: number, iconH?: number,
  extent?: VectorExtent, maxFeatures?: number): number
```

- 返回图层 index（0 起，与 `addVectorLayer`/`addOverlayLayer` 共享索引空间），失败 `-1`。
  样式、图标、extent、maxFeatures 语义与 `addVectorLayer` 完全一致，不重新定义。
- Harmony 侧沿用组件属性 `vectorCacheDir`（URL 下载与 DATA 落盘都写其下）；Android 沿用
  `filesDir/globecore/vector-cache` 自动推导，调用方无感。

### 3.3 三种来源的 native 分发

```text
url 非空
  ├─ http(s) 前缀 → RemoteVectorCache 下载 → <cacheDir>/url/geojson-<hash>.geojson → 读缓存（同 P0 现状）
  └─ 否则视为本地文件 → effectivePath = 原值，直接进 readVectorFile（LITE 经 lite reader；full 经 OGR GeoJSON 驱动）
data 非空
  └─ 原子写 <cacheDir>/inline/geojson-<FNV1a(text)>.geojson → 读缓存（同内容天然去重）
```

- DATA 首期走「落盘复用」路线：写完即与 FILE 同管线，LITE 后缀路由天然成立（文件名强制 `.geojson`）。
- 纯内存解析（lite reader 增 `readLiteGeoJsonString(const char*, len)`；full 用 GDAL `/vsimem/`）列入后续 P3，不在本期。
- `VectorLoader` 内部把 bool `remoteSource_` 升级为枚举 `sourceKind_{File,Url,Data}`；引擎/桥接层依据 url/data 互斥校验结果显式传 kind 与 value。

### 3.4 旧接口回退与弃用

- `addVectorLayer(path, style, ...)`：full 构建恢复仅本地 OGR 文件语义；LITE 构建保留符号，非 GeoJSON 返回「当前为 LITE 构建（未链接 GDAL/OGR），仅支持 GeoJSON 矢量文件，请改用 addGeoJSONLayer」。
- 弃用周期（一个版本）：检测到 `http(s)://` 前缀时仍走新通路并打一次 `LOGW` 迁移警告（含调用栈提示），KDoc/ArkTS 注释标 `@deprecated`；下一个版本删除该隐式分支，URL 传入按「本地文件不存在」报错。
- DATA 来源只在新接口提供，旧接口永不接收字符串内容。

### 3.5 行为矩阵

| 来源 | full 构建 | LITE 构建 |
|---|---|---|
| 本地 shp/kml/dwg/gpkg | `addVectorLayer` ✓ | 中文报错不崩溃 |
| 本地 .geojson/.json | 两接口均可，文档引导 `addGeoJSONLayer` | 仅 `addGeoJSONLayer` ✓ |
| http(s) GeoJSON | `addGeoJSONLayer` ✓（旧接口弃用期可用+警告） | 同左 |
| GeoJSON 字符串 | `addGeoJSONLayer` ✓ | `addGeoJSONLayer` ✓（落盘→lite reader） |

## 4. 任务分解（WBS）

> 粒度：每个任务可独立编译、独立提交；除 T0 外均带 `@Jin` 注释，实验开关守卫，可整体回退。

### 阶段 0：基线与分支

- T0 从最新 `main` 切 `feature/geojson-layer-api`；确认 LITE/full 两套构建当前可出包，记录基线 `GLOBECORE_BUILD_ID`。

### 阶段 1：native 数据源通路重构（C++，无 API 变化）

- T1 `VectorSourceResolver` 扩展：新增 `SourceKind{File,Url,Data}` 与 `resolve(kind, value, cacheDir)`；保留旧 `isHttpUrl` 供弃用分支调用；DATA 缓存键复用 FNV-1a，目录分 `url/` 与 `inline/`。
- T2 `RemoteVectorCache` 抽出原子写工具（下载落盘与 DATA 落盘共用），保持 80MB/20s 限制与中文错误文案。
- T3 `VectorLoader`：`remoteSource_` bool 改 `sourceKind_` 枚举；FILE 直通、URL 下载、DATA 先落盘再统一走 `readVectorFile`；`reload`/abort/状态机行为不变。
- T4 LITE：`lite_gdal_stubs.cpp` 后缀路由保持可用，新增 `lite_geojson_reader` 字符串解析的**预留声明**（本期可不实现，仅占位于 P3；若 T3 落盘路线验证顺利则本任务降为注释 TODO）。
- 验收：C++ 单测/桌面冒烟，三种 kind 在桌面构建（非 LITE）走通；`addVectorLayer` 旧调用零回归。

### 阶段 2：引擎与桥接层

- T5 `GlobeEngine` 新增 `addGeoJSONLayer(url, data, style, icon..., extent..., maxFeatures, cacheDir)`：入口处做互斥校验并映射为内部 `SourceKind{File,Url,Data}`，构造 loader 时显式传 kind。旧 `addVectorLayer` 签名保留，cacheDir 参数标弃用注释。
- T6 JNI（`GlobeEngineJni.cpp` + `NativeLib.kt`）新增 `nativeAddGeoJSONLayer`；NAPI（`napi_globe_engine.cpp` + `index.d.ts`）同构新增，门面参数为 url/data 两个字符串 + 现有样式/图标/extent 参数族，互斥非法时返回 -1。
- T7 弃用分支：旧 `nativeAddVectorLayer` 检测 URL 前缀 → 转调新通路 + 单次 `LOGW`；LITE 非 GeoJSON 本地文件中文报错核对。
- 验收：两端编译通过；符号表检查新接口存在、旧接口未删。

### 阶段 3：平台 API 层

- T8 Harmony：`NativeMapView.addGeoJSONLayer(url, data, ...)` 与互斥校验；`Index.ets` 导出；`vectorCacheDir` 注入说明更新。
- T9 Android：`NativeMapView.addGeoJSONLayer(url, data, ...)` 同构实现；缓存目录自动推导复用现有逻辑。
- T10 两端 demo 增加三个入口（FILE/URL/DATA），DATA 用内联 FeatureCollection 小样例；旧「省级GeoJSON」按钮切到新接口 URL 入口。
- 验收：demo 三来源均出图；旧 demo 路径（弃用期）URL 仍可用且有警告日志。

### 阶段 4：文档与发布物

- T11 更新 `docs/api/harmony-api-guide.html`、`docs/api/android-api-guide.html`：新接口、url/data 互斥规则、参数表、错误码、弃用说明。
- T12 更新 `README.md`：能力速查、@Jin 改造记录、实验性/可回退说明与回退方式（关闭源码内 `kExperimentalGeoJsonLayerApi` 守卫即恢复纯旧通路）；本设计文档状态改「已实现」。
- T13 出包核对：lite/full 命名按全局约定带 `-lite`/`-full`；`llvm-nm/readelf` 验证 AArch64 与 lite 符号边界（无 GDAL/OGR/PROJ 真符号、含 `readLiteGeoJsonFile`）；包内 `README-integration.md` 补新接口说明。

### 阶段 5：验证与收尾

- T14 真机（鸿蒙设备 `5KLBB25911200313`，2026-11-19 已完成）回归矩阵：
  三来源 × LITE、缓存命中/二次加载、显隐、3D 下显示均已通过（数据见改造记录第十二节）。
  未覆盖项（沿用 P1 清单，不阻塞本期）：extent 重载真机、拾取高亮、`setVectorMinLevel`、断网/404/HTML 错误页/超 80MB 错误文案，
  这些路径与 `addVectorLayer` 共用未改的既有代码，逻辑零差异。
  **封存专项**：3D 地形下面状（Polygon/MultiPolygon）要素贴地遮挡仍未根治（用户 2026-11-19 拍板本期不解决，
  与 README ⑱ 遗留项合并为后续专项）；纯线 `603d531` 通路与内联小面显示正常。
- T15 Android 模拟器/真机同矩阵（full 构建）+ 旧接口 OGR 多格式回归（shp/kml）：待补（本机缺预构建 libcurl.a，
  native 链接无法在当前环境完成；Kotlin/JNI 已通过编译与语法检查）。
- T16 提交：中文提交信息、改造内容标 @Jin；评审后合并 main、推远端；本分支归档。

## 5. 风险与回退

- 风险 1：loader 枚举化改动影响现有在线图层状态机。缓解：T3 先做等价替换（FILE=旧本地、URL=旧 remote 两条路径行为逐行对照），DATA 作为第三条增量路径。
- 风险 2：DATA 大字符串落盘卡顿。缓解：后台线程执行（与下载同线程模型），UI 不阻塞；P3 再做 `/vsimem/` 纯内存。
- 风险 3：弃用期双入口行为漂移。缓解：两入口在引擎层收敛到同一个 `addGeoJSONLayer` 内部方法，旧门面只做前缀判定+转发。
- 回退：编译期守卫 `kExperimentalGeoJsonLayerApi`（位于 `VectorSourceResolver.h`，默认 ON）；OFF 时新门面返回 `-1` 并打中文错误，全部源码回到 P0 后的旧通路，无数据格式迁移、无持久化结构变化。

## 6. 工作量预估

- C++ 核心（T1–T3、T5）：2–3 天；桥接与平台层（T6–T9）：2 天；demo 与文档（T10–T13）：1.5 天；真机/双端回归（T14–T15）：1.5 天。合计约 7–8 人日，不含 P1–P3 后续能力。
