# GlobeCore

本项目是一套**独立实现**的 **C++ 共享渲染引擎，四平台产物**：

| 平台 | 宿主 | 桥接 | 产物 |
|------|------|------|------|
| **Android** | Kotlin + `GLSurfaceView` | JNI | AAR（`com.zys:globecore`） |
| **HarmonyOS NEXT** | ArkTS + `XComponent` | NAPI | HAR（`@zys/globecore`） |
| **Windows** | Qt5 `QOpenGLWidget` | 直接 C++ 调用（无 JNI/NAPI 编解码层） | 可执行 exe |
| **Linux**（含麒麟/UOS/龙芯） | Qt5 `QOpenGLWidget` | 同上 | 可执行二进制 |

门面层（`NativeMapView` 等）只做接口转发，相机、瓦片网格、取瓦片与缓存、矢量读取/三角剖分、
OpenGL ES 2.0 绘制全部在 native 完成，并内联编译了 GDAL/OGR、PROJ、libcurl 全套第三方静态库能力。

当前版本 **0.1.4.0**：GeoPackage 工程读写（`geopackage_io`，七表结构 + JSON 桥接 + 原子写 + SetFID 记录 id 持久化）、着色器精度修复（顶点阶段归一化法向/视线方向，修复真机 3D 光照无过渡问题）、瓦片缓存 MBTiles 单文件后端（含 `added_at` 入库时间列）。tag 后追加（未重新打版）：鸿蒙 HAR 构建补齐引擎源清单（修复 so 链接缺符号）、photos 表新增 `photo_data` BLOB（主照片随工程文件入库）。

### @Jin 实验性在线 GeoJSON Source（P0）

- @Jin `NativeMapView.addVectorLayer(path, ...)` 的 `path` 在 Android 与 HarmonyOS NEXT HAR 两端除本地文件外，实验性支持公开 `http://` / `https://` GeoJSON URL。native 不依赖 GDAL `/vsicurl/`，而是在后台通过现有 `HttpClient` 下载、做基础 JSON/HTML/空响应/80MB 上限校验，并原子写入应用私有目录 `filesDir/globecore/vector-cache/url/<hash>.geojson`，随后复用现有 `buildGeometry → Renderer VBO/拾取/贴地` 管线。
- @Jin HarmonyOS NEXT 当前为 `GLOBECORE_LITE=ON`，没有链接 GDAL/OGR；LITE 包新增 `ohos/lite_geojson_reader.cpp`，在缓存下载完成后用无 GDAL 依赖的原生 GeoJSON reader 支持常见 Point/MultiPoint、LineString/MultiLineString、Polygon/MultiPolygon、GeometryCollection、Feature/FeatureCollection。shp/kml/tif 等格式仍需非 LITE/GDAL 构建。
- @Jin 同一 URL 首次成功下载后，后续重新打开图层或调用 `updateVectorExtent()` 只读取本地缓存，不因视口范围变化重复下载；析构/销毁图层会通过 abort 中断下载。
- @Jin 回退方式：将 `globecore/src/main/cpp/vector/RemoteVectorCache.h` 中 `RemoteVectorCache::kExperimentalRemoteGeoJsonSource` 改为 `false`，即恢复为纯本地路径语义；本次 Kotlin 公开方法签名保持不变。
- @Jin P0 暂不支持自定义 headers、token、ETag/Last-Modified、强制刷新、下载进度和 stale cache 网络失败兜底，这些能力保留在 `docs/design/online-geojson-vector-proposal.md` 的 P1/P2/P3。
- @Jin 内网 `http://` 地址在 Android 9+ 真机上还需宿主按调试策略允许 cleartext HTTP；正式环境建议使用 HTTPS。
- @Jin Android demo 的 `03 · 矢量图层与拾取` 与 HarmonyOS NEXT Demo 的“省级GeoJSON”按钮均直接加载 `http://210.74.129.84:8520/app-pub/web-data/geojson/district/province.json`，不再由 App 预下载、JSON.parse 或转动态 overlay；Android demo Manifest 仅为该内网 HTTP 调试地址开启 cleartext HTTP。
- @Jin 鸿蒙真机 P0 记录（2026-10-28，设备 `5KLBB25911200313`）：首次点击省级在线图层时 native 下载 `province.json` 669,489 bytes，日志依次出现 `[RemoteGeoJson] downloaded`、`[LiteGeoJson] 读取完成 features=4`、`[VecReload] VectorLoader 就绪`，画面显示河南/湖北/四川/陕西四省黄色边界；force-stop 后重启并再次加载，同 URL 命中 `[RemoteGeoJson] cache hit ... elapsed=0ms`，读取与构建总耗时约 268ms，未再出现 ArkTS `province geojson downloaded/overlay` 兜底日志。
- @Jin 鸿蒙真机同时在 3D 地形开启状态下缩放到四省范围，黄色省级边界仍作为普通 native VectorLayer 叠加显示；在线源读取的是下载后的本地缓存文件，因此 LITE GeoJSON reader 对“远程 URL 落缓存后的本地 .geojson”路径已完成真机验证。本地 rawfile `sample.geojson` 入口仍走同一 `readLiteGeoJsonFile()` 路由，桌面冒烟已通过，后续可再做一次人工 UI 回归。

### @Jin Android API 帮助文档

- @Jin 新增 `docs/api/android-api-guide.html`，面向 Android/Kotlin/Java 宿主开发人员，分类整理 `NativeMapView`、`Camera`、`Position`、`VectorStyle`、`VectorExtent`、`FeatureGeometry`、`NativeSrs`、`NativeVector`、`NativeLayerInfo` 等公开 API，补充参数/返回值、生命周期、XYZ 瓦片、三维地形 Provider、动态 overlay、拾取反算、坐标转换、GDAL/OGR 文件 IO 与可复制示例。
- @Jin 维护约定：以后新增、修改、废弃或删除 Android 公开 API，必须在同一次定稿改动中同步更新该 HTML 文档；实验性 API 必须明确标注“实验性/可回退”。

## 它是干什么的

一句话：**给手机 App 和桌面应用一个能显示 2D 地图 / 3D 地球、能直接读 shp·tif·kml、能离线用的地图渲染内核**。
瓦片下载与缓存、坐标系转换、几何三角剖分、GL 着色器都不用自己写——`NativeMapView` 摆进布局，调几个
方法就有地图；GDAL/OGR、PROJ、libcurl 全部静态链进产物，宿主**无需 NDK/CMake、不接任何在线地图厂商
SDK**。

### 能力清单

| 分类 | 具体能力 | 入口 |
|---|---|---|
| 视图 | 2D 平面墨卡托（正交）/ 3D WGS84 球体（透视，可旋转、俯仰、地平裁剪），两模式共用一套相机，切换保持视角连续 | `setViewMode` / `setCamera` / `getCamera` |
| 底图 | 任意 XYZ 瓦片图源（`{z}/{x}/{y}` URL 模板）多层叠加；注记层透明混合置顶；超出图源最大级别拉伸末级纹理（无限放大不出 404 占位块）；URL 传空即纯离线读缓存 | `addTileLayer` |
| 缓存 | 瓦片缓存双后端：`cacheDir` 以 `.mbtiles` 结尾走 SQLite **MBTiles 单文件**（WAL、标准 tiles/metadata 表、TMS y 翻转、扩展列 `added_at` 入库时间，老库自动 ALTER 补列），否则按 `<cacheDir>/<z>/<x>_<y>.tile` 目录落盘；2D/3D 纹理缓存同键复用；取瓦片走 native 后台线程池，就绪异步上屏 | 同上（`cacheDir`） |
| 本地影像 | tif/img 等 GDAL 可打开格式，双通路：`addRasterLayer` native 重投影后按全球墨卡托即时切片并回写缓存复用（级别由栅格分辨率自动推算）；`addRasterImageLayer` 整幅直读单张贴图按 WGS84 四至贴面（UI 线程零 GDAL 立即返回，超 GL 纹理/内存帽沿 overviews 降采样，全幅 sidecar 磁盘缓存二次打开秒级就绪） | `addRasterLayer` / `addRasterImageLayer` |
| 三维地形 | quantized-mesh v1.0 双通路：底层保留 `setTerrainTile/clearTerrain` 注入接口；共享 C++ `CesiumTerrainProvider` 已由 native 直接读取相机视口、中心优先生成 DEM 任务，并完成 `layer.json` 解析、EPSG:4326/TMS 与墨卡托 bbox 求交、磁盘缓存、并发下载、重试、去重和流式注入；Harmony NAPI/HAR 与 Android JNI/Kotlin 均暴露同名门面。宿主只注入 `baseUrl/cacheDir/maxZoom/concurrency/tileRadius/headers`，不绑定服务地址或 token；native 按真实 bbox 全局共享覆盖多块 DEM，索引按 high-watermark 解码，DEM 逐帧削峰、地形瓦片静态 VBO（LRU），仅 3D 球体生效；Android/Harmony/Qt 统一生命周期事件（元数据、开始、首块、进度、完成、错误、开关）由平台桥接转投宿主线程 | `configureTerrainProvider` / `setTerrainProviderEnabled` / `refreshTerrainProvider` / `terrainProviderStatus` / `setOnTerrainProviderEventListener` / Qt `terrainProviderEvent` |
| 瓦片调度 | 后台线程池取瓦片（先盘后网、原子写盘、失败冷却退避），GL 线程仅异步上传并按帧限额；P0-2 exp1f 后视口代际（view-generation）只推进代际、不再立即清空队列/ready，同 key 排队或在途任务可原地升级，已解码待上传瓦片用 ready key 去重；所有瓦片层共享每帧 4 张 / 1MiB 上传预算，当前视口优先、最近旧视口随后补齐；在线图源先投 z-2/z-1 祖先粗瓦，再以屏幕中心优先替换叶子，3D 叶瓦片请求限制为交互 96 块、空闲 192 块；交互期增加 LOD 迟滞，优先保持祖先粗图连续覆盖，大倾角近景按相机三维距离排队并保证全部可见叶至少具备 z-2 粗祖先覆盖，显著压低无效请求量与“先黑块再出图/近景黑面”的观感；3D 地理四叉树帧间缓存同时校验眼点、离地高度尺度、地形安全高程与相机 forward/up，倾斜、旋转、缩放后立即重建可见叶集；exp1h 起地平线/视锥/AABB/SSE 使用 DEM 安全壳，3D 相机目标点落在真实地形表面并按附近山脊保留最小净空，避免复用旧视锥或眼点穿入山体造成黑区 | `addTileLayer` |
| 矢量 | Android 非 LITE 支持 shp/kml/kmz/dwg/dxf 等 OGR 格式直读；HarmonyOS NEXT LITE 无 GDAL 依赖，P0 内置常见 GeoJSON 文件/在线 URL 读取。统一重投影到 WGS84 GeoJSON 口径后进入 earcut 三角剖分、面填充/描边/线/点/图标 billboard/文字标注（字形图集，可复用系统 CJK 字体） | `addVectorLayer` / `VectorStyle` / `setFontPath` / `vectorCacheDir` |
| 大数据矢量 | 按屏幕范围增量重载（Swap-on-ready，重载期无空窗）、单次要素预算、加载中状态轮询、按级别隐藏、样式变更就地换层不闪图 | `updateVectorExtent` / `hasVectorLoading` / `setVectorMinLevel` / `setVectorLayerVisible` / `removeVectorLayer` |
| 拾取与属性 | 单击命中 `[layerIndex, fid]`；回取要素 WGS84 几何用于选中高亮；屏幕点→经纬度（采集/测量加点）；范围过滤 + 字段白名单 + 几何简化读取、按 FID 全字段回取、属性 SQL 查询、要素计数、按 FID 写回源文件 | `pickVector` / `featureGeometry` / `screenToGeo` / `NativeVector` |
| 业务叠加层 | 运行时内存几何层（点/线/面覆盖式推送）：测量、轨迹、拍照标识、样地等业务图形；瞬态高亮层可退出拾取竞争 | `addOverlayLayer` / `updateOverlay*` / `setOverlayNoPick` |
| 定位标记 | 蓝点或自定义罗盘图标 + 方位角箭头，屏幕固定尺寸不随缩放，可由 1Hz 定位回调驱动 | `setLocationMarker` / `setLocationMarkerIcon` |
| 坐标转换 | PROJ 任意 EPSG / PROJ 文本互转；`proj.db` 与格网改正数随模块分发（Android assets / 鸿蒙 rawfile），宿主一行初始化 | `NativeSrs.initProjData` / `convert` |
| 数据元信息 | 文件四至（非 WGS84 自动重投影）、坐标系描述、字段名清单、GDAL 版本与已注册驱动清单 | `NativeLayerInfo` |
| 交互手势 | 平移 / 捏合缩放（聚焦缩放）/ 单击取点 / 松手惯性滑行；双指旋转与双指俯仰（tilt，钳 [0,80]）仅 3D 生效，识别器可摘出复用；多指→单指切换/依次抬指不产生跳变（ArkTS 单指基准+宽限窗口钳制，native `panByPixels` 单帧阈值兜底，2D/3D 通用） | `MapGestures` / `setOnTapListener` |
| 运行开销 | OpenGL ES 2.0；按需渲染（静止不重绘，省电）；门面方法只做 native 状态更新，可任意线程调用（含 Surface 创建前） | — |
| 多平台 | 同一套 C++ 引擎源四侧共用（鸿蒙/桌面经 `GLOBECORE_ENGINE_ROOT` 直接引用，不复制不搬迁）：Android JNI/AAR，HarmonyOS NEXT NAPI/HAR，Windows/Linux Qt5 Widgets 可执行程序 | — |

### 适合这些场景

- **离线可用的移动端 GIS**（外业调查、样地、国土/林业巡检）：瓦片缓存 + 本地 shp/tif 直读，无网也能出图
- **离线可用的桌面 GIS**（内网数据浏览、现场核查、大屏展示）：同一引擎跑 Windows/Linux，无需额外开发
- 已有自建 WMTS / 高德 / OSM 等**任意 XYZ 图源**，只想快速塞进 App、不想写渲染层与缓存层
- 需要**读 GIS 文件并做属性交互**：点选要素、展示字段、SQL 查询、属性改写回写
- 需要 2D 地图与 3D 地球在**同一视图、同一相机**之间来回切换
- **HarmonyOS NEXT（纯血鸿蒙，无 AOSP 兼容层）** 需要一套原生 GLES 地图内核（0.1.1.0 起）

### 不包含

- 不带业务 UI（图层树、底图切换面板、工具条等由宿主自绘；仓库内两端 demo 仅作调用示例）
- 不内置地理编码、路径规划、POI 检索；几何编辑需宿主经叠加层自行驱动（库侧只到属性写回）
- 不提供 iOS 产物，也不做服务端矢量切片（瓦片要么在线取、要么由本地栅格现切）
- 不依赖、未链接、未衍生任何第三方地球渲染引擎源码（详见文末「许可与第三方声明」）

> 看完这段决定要用 → 直接跳「API 概览与快速上手」（Kotlin 最小用例）或「鸿蒙 HarmonyOS NEXT
> 集成」（ArkTS 最小用例）或「桌面宿主」（Qt5 Widgets）；想先跑起来看效果 → 两端 demo 见「功能演示 App」与「演示工程
> globecore-harmonyos」，桌面 demo 见 `globecore-desktop/`。

## 仓库结构

```
GlobeCore/
├── globecore/                    # Android Library 模块（Kotlin 门面 + cpp 渲染内核；引擎源在此）
│   └── src/main/cpp/
│       ├── jniLibs/<abi>/           # 第三方预编译 .a（编译产物，不入仓，见「0 → 1 从零构建」）
│       └── include/                 # 三方头（gdal/proj/curl/openssl/sqlite/zlib 由构建生成，不入仓）
│                                    #   stb/ 与 earcut.cpp 为仓库自带 vendored 源，随仓提供
├── globecore-android/               # 功能演示 App（MainActivity 菜单逐项体验，project 依赖库模块）
├── globecore-harmonyos/             # HarmonyOS NEXT 工程（DevEco Studio 打开；library HAR + entry 演示 HAP）
│   └── library/src/main/cpp/        #   鸿蒙专属源（EGL/XComponent/NAPI）+ 经 GLOBECORE_ENGINE_ROOT 引用引擎源
├── globecore-desktop/               # 桌面宿主工程（Windows/Linux，Qt5 Widgets）
│   ├── CMakeLists.txt               #   依赖定位 + GL 后端选择 + 引擎源引用
│   └── src/                         #   QOpenGLWidget 宿主 + 图层面板 + 主窗口
├── build-scripts/                   # 第三方静态库编译脚本（WSL/Linux；--target android|ohos|windows|linux）
└── gradle/ + 根构建脚本             # 独立 Gradle 工程（Gradle 9.4.1 / AGP 9.2.1）
```

> 若干本地文件**不入仓**：`操作说明.md`、`版本迭代.md`、`HAR发布指南.md`（HAR ohpm 发布
> 操作手册）与 `build-har.ps1` / `publish-har.ps1`（HAR 本地构建/发布脚本，涉及维护者本机
> ohpm 私钥环境）仅存于维护者工作区，克隆本仓库不会得到；上述排除不影响「0 → 1 从零构建」
> 与各端集成流程。

## 0 → 1 从零构建

新克隆本仓库**不含**下列编译产物，需要先运行 `build-scripts` 一键生成，之后 Gradle / Android
Studio 才能编译通过：

| 产物 | 位置 | 是否入仓 | 生成方式 |
|---|---|---|---|
| 第三方 `.a`（每 ABI 14 个） | `globecore/src/main/cpp/jniLibs/{x86_64,arm64-v8a}/` | ❌ | `build-all.sh` |
| 三方头文件 | `globecore/src/main/cpp/include/{gdal,proj,curl,openssl,sqlite}`、`zlib.h`/`zconf.h` | ❌ | `build-all.sh`（install_headers，双平台共用） |
| PROJ 运行期数据 | `globecore/src/main/assets/proj/`（proj.db 等） | ❌ | `build-all.sh`（install_proj_data） |
| 鸿蒙第三方 `.a`（每 ABI 14 个） | `build-scripts/out/ohos/{arm64-v8a,x86_64}/` | ❌ | `build-all.sh --target ohos` |
| 鸿蒙 PROJ 运行期数据 | `globecore-harmonyos/library/src/main/resources/rawfile/proj/` | ❌ | 同上（install_proj_data 按平台落位） |

native 构建需要每 ABI 14 个 `.a`（约 450MB/ABI，其中 libgdal.a 约 330MB），体积过大不随仓库分发：

```
libgdal.a  libproj.a  libsqlite3.a  libkmlbase.a  libkmldom.a  libkmlengine.a
libkmlxsd.a  libkmlconvenience.a  libkmlregionator.a  liburiparser.a
libminizip.a  libexpat.a  libz.a  libcurl.a
```

> ⚠️ `jniLibs/` 的 ABI 目录名必须与 `ANDROID_ABI` 完全一致。
> `libcurl.a` 内部已静态打包 OpenSSL 1.1.1w + zlib，无需再单独链
> libssl/libcrypto。

### 路径 A：下载现成产物（免编译，推荐快速上手）

本仓库 [Releases](https://github.com/z-gis/GlobeCore/releases) 提供与头文件版本配套的预编译
产物包 **`globecore-prebuilt-libs-v0.1.0.0.zip`**（约 106MB，含双 ABI 共 28 个 `.a` + 三方头 +
`assets/proj` 数据，共 290 个文件）。压缩包保持仓库相对路径（顶层 `globecore/...`），在**仓库根
目录**解压即全部落位，无需本地编译工具链：

```bash
# 于仓库根目录执行
curl -LO https://github.com/z-gis/GlobeCore/releases/download/v0.1.0.0/globecore-prebuilt-libs-v0.1.0.0.zip
unzip globecore-prebuilt-libs-v0.1.0.0.zip   # 产物落入 cpp/jniLibs、cpp/include、assets/proj
```

> 直链：<https://github.com/z-gis/GlobeCore/releases/download/v0.1.0.0/globecore-prebuilt-libs-v0.1.0.0.zip>
> 该包由路径 B 的 `build-all.sh` 产出后打包，若你更新了三方库版本，请用路径 B 重编并同步更新 Release。
> 0.1.1.0 未变更任何三方库版本，该包对 Android 0.1.1.0 依然有效；鸿蒙 `.a` 需另跑路径 B 的 `--target ohos`。

### 路径 B：用 build-scripts 一键自行编译（推荐）

脚本位于 **`build-scripts/`**，在 **WSL2 / Linux** 下运行（Windows 直接跑不通）。一次执行即
**同时产出 x86_64 与 arm64-v8a 两套共 28 个 `.a`**，并自动刷新 `include/` 三方头与
`assets/proj/` 数据，全部落回仓库对应目录。

依赖（脚本会自动探测 / 下载，一般无需手动准备）：
- Android NDK **r23b**（编译期工具链；与模块自身 Gradle 构建用的 NDK 28.x 不冲突，静态库 ABI 兼容）
- cmake ≥ 3.22（Linux 版）
- 网络可达 github / osgeo / sqlite 等源码站（首次会下载各 tarball）

只需一条命令：

```bash
cd build-scripts
bash build-all.sh
```

要点：
- 无需手改 ABI / MAIN_DIR。脚本内置双 ABI 循环，按依赖顺序构建
  （expat → minizip/zlib → uriparser → openssl → curl → boost 头 → libkml → sqlite3 → proj → gdal）。
- **WSL 下若脚本在 Windows 挂载盘（`/mnt/<盘>`，9p/drvfs）**：NDK/cmake 解压出的符号链接与可执行位
  无法保留，脚本会自动把构建工作区重定向到原生目录 `~/android-jni-build`（ext4），产物仍按仓库路径
  落回挂载盘上的 `jniLibs/`、`include/`、`assets/proj/`。
- 可离线复用：把已下载的 `*.tar.gz`/`*.zip` 与 `boost/` 头放进 `build-scripts/third-party/`
  （`PKG_CACHE`），脚本会 `cp -n` 种入工作区，避免重复下载。
- **鸿蒙产物**：把 `--target ohos` 追加到任一命令即可（`bash build-all.sh --target ohos`），
  ABI 换为 `arm64-v8a`（真机）+ `x86_64`（DevEco 模拟器），工具链改用 OHOS NDK。
  华为侧 NDK 需登录账号下载，脚本只探测不自动下载：解压 Command Line Tools SDK 里的
  `.../sdk/default/openharmony/native` 后 `export OHOS_NDK_ROOT=<路径>/openharmony/native`
  （或放入 `build-scripts/ndk/` 下），探测口径 `llvm/bin/clang` + `build/cmake/ohos.toolchain.cmake`。
  产物落 `build-scripts/out/ohos/<abi>/`，三方头与 `assets/proj` ↔ `rawfile/proj` 按平台自动分流，
  与 Android 产物目录完全隔离、可重复交叉构建。

各源码包版本（须与生成头文件配套）：

| 源码包 | 版本 | 产物 |
|---|---|---|
| gdal | 3.7.0 | libgdal.a |
| proj | 9.0.0 | libproj.a + assets/proj 数据 |
| sqlite-autoconf | 3370200 | libsqlite3.a |
| libkml | 1.3.0 | libkml{base,dom,engine,xsd,convenience,regionator}.a |
| expat | 2.4.8 | libexpat.a |
| zlib | 1.2.13 | libz.a + libminizip.a |
| uriparser | 0.9.8 | liburiparser.a |
| openssl | 1.1.1w | 并入 libcurl.a |
| curl | 8.4.0 | libcurl.a |
| boost | （仅头文件） | libkml 编译依赖 |

## 构建 AAR

1. 完成上面「0 → 1」拿到 `.a` + 头 + PROJ 数据。
2. 构建：

   ```bat
   gradlew.bat :globecore:assembleRelease
   ```

   产物：`globecore/build/outputs/aar/globecore-release.aar`（另自动产出同名带版本号副本
   `outputs/aar-versioned/globecore-<version>-release.aar`，便于宿主按版本落 `libs/` 管理）。
   首次构建会由 CMake 全量编译 native 内核并静态链接 GDAL 链。

构建环境：JDK 21+（wrapper 自动解析）、Android SDK（compileSdk 37）、NDK `28.2.13676358`、
CMake `3.22.1`。验证链接是否缺库：报 `cannot find -lxxx` / `Unable to find library` 即 `.a`
缺文件或目录名不对。

## 引用方式：Maven 坐标（推荐，Gradle/IDE 自动下载）

发布产物托管在本仓库的 **GitHub Pages Maven 仓**，宿主**无需手动下载 AAR**——在 `settings.gradle.kts`
的 `dependencyResolutionManagement.repositories` 里加一行仓库，再按坐标引用：

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("https://z-gis.github.io/GlobeCore/maven") } // globecore
    }
}
```

```kotlin
// app/build.gradle.kts
android {
    defaultConfig {
        minSdk = 24
        ndk { abiFilters += listOf("x86_64", "arm64-v8a") }
    }
}
dependencies {
    implementation("com.zys:globecore:0.1.4.0")   // 自动下载；传递依赖（androidx core-ktx）随之带入
}
```

> 注意：仓库地址要加在 **`dependencyResolutionManagement.repositories`**（解析 `implementation` 依赖用）；
> 只加进 `pluginManagement.repositories` 不对——那处只管 Gradle 插件，坐标会报
> `Could not find com.zys:globecore:x.y.z`。本仓库的 `globecore-android` 演示模块同样按此配置，
> 把 `implementation(project(":globecore"))` 换成上面的 Maven 坐标即可验证“外部消费者”体验。
>
> 与下方手动 files() 方式的区别：Maven 坐标会自动解析传递依赖（无需再手写 `core-ktx`）。
> 其余（`NativeSrs.initProjData` 初始化、宿主自行声明 `INTERNET` 权限、`NativeMapView` 用法）与
> 「AAR 三步曲」的第二~四步完全一致。
>
> 发布方式：**本地手工部署**（仓库 CI workflow 已移除，推 `v*` tag 不会触发自动发布）——本地编 AAR
> 后 overlay 部署到 `gh-pages:/maven` 分支（保留历史版本目录，Gradle `publish` 产物拷入 gh-pages
> worktree 提交推送）。

## 备选：手动引入 AAR 文件（无网络仓库时的“三步曲”）

globecore 以 **AAR 文件**方式集成，无需引入源码模块。

**第一步：放入 AAR** —— 把 `globecore-release.aar` 复制到宿主工程 `app/libs/`（宿主无需安装
NDK/CMake，渲染内核已编译进 AAR 的 `jni/*.so`）。

**第二步：声明依赖** —— `app/build.gradle.kts`：

```kotlin
dependencies {
    // files() 方式不解析传递依赖，宿主需自带 androidx core-ktx
    implementation(files("libs/globecore-release.aar"))
    implementation("androidx.core:core-ktx:1.10.1")
}

android {
    defaultConfig {
        minSdk = 24
        ndk {
            // AAR 仅打包这两个 ABI，宿主保持一致避免运行期找不到 .so
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
    }
}
```

AAR 内置的 `proguard.txt`（consumer rules）会自动生效，宿主开混淆也无需额外配置。

**第三步：启动初始化** —— PROJ 数据（proj.db 与格网改正数文件）随 AAR assets 分发、合入宿主
APK。宿主启动时（首次坐标转换 / 建图层之前）调用一次：

```kotlin
// 解压 assets/proj 到 filesDir/proj 并设置 PROJ 搜索路径；已存在的文件跳过，幂等可重复调
com.zys.globecore.NativeSrs.initProjData(context)
```

未初始化时坐标转换按既有口径返回 `null`（不崩溃），调用方需自行回退。验证集成：

```kotlin
NativeSrs.convert(116.4, 39.9, "EPSG:4326", "EPSG:3857")  // 非 null 即成功
```

集成常见问题：
- **`UnsatisfiedLinkError: libglobecore.so`**：宿主 abiFilters 未包含目标 ABI，或 APK 里同名
  .so 被其它依赖覆盖。
- **坐标转换全部返回 null**：漏了第三步 `initProjData`。
- **升级 AAR 后 assets 旧数据残留**：解压逻辑按「文件已存在即跳过」，升级 PROJ 数据需先清应用数据
  （或自行删除 `filesDir/proj`）。

## API 概览与快速上手

公开 API 均为 Kotlin 门面类（`com.zys.globecore` 包），JNI 细节封装在 internal 的
`NativeLib` 中，宿主不需要直接接触。各方法语义以源码 KDoc 为准，本节只给地图接入最小路径。

门面类一览：

| 类 | 职责 |
|---|---|
| `NativeMapView` | 地图视图（继承 GLSurfaceView）：相机、视图模式、瓦片/栅格/矢量/叠加层、拾取、定位标记 |
| `NativeSrs` | PROJ 初始化（`initProjData`）与任意坐标系互转（`convert`）、PROJ 版本 |
| `NativeLayerInfo` | 矢量/栅格文件元信息：四至、坐标系、字段名（建图层前探测用） |
| `NativeVector` | 矢量读写与 SQL 查询桥接（GDAL/OGR 能力入口） |
| `Camera` / `Position` / `VectorExtent` / `VectorStyle` / `FeatureGeometry` | 值对象 |
| `MapGestures` | 手势识别（已内联进 `NativeMapView`，自定义视图时复用） |

最小用例：显示在线瓦片底图：

```kotlin
class MapActivity : AppCompatActivity() {

    private lateinit var map: NativeMapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 一次性：解压 AAR 内置 proj 数据并配置 PROJ 搜索路径（见上文第三步）
        NativeSrs.initProjData(this)

        map = NativeMapView(this)
        setContentView(map)

        // 瓦片层：磁盘缓存目录 + URL 模板（{z}/{x}/{y} 占位）+ 图源最大级别
        map.addTileLayer(
            cacheDir = File(filesDir, "tiles/img").absolutePath,
            urlTemplate = "https://.../wmts?...&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}",
            maxLevel = 18,
        )
        // 相机：经纬度（度）+ 高度（米）
        map.setCamera(Camera(latitude = 39.9, longitude = 116.4, altitude = 3_000_000.0))
    }

    // GLSurfaceView 生命周期要求
    override fun onResume() { super.onResume(); map.onResume() }
    override fun onPause() { super.onPause(); map.onPause() }
    override fun onDestroy() { map.destroy(); super.onDestroy() }  // 释放 native 实例
}
```

3D 球体：

```kotlin
map.setViewMode(NativeMapView.ViewMode.THREE_D)  // 与 2D 共用相机状态，切换保持视角连续
```

矢量图层与拾取：

```kotlin
// OGR 可打开的任意格式；样式颜色为 #AARRGGBB 打包 Int；异步读取+三角剖分，就绪自动重绘
// （后续参数：图标像素/尺寸、加载范围、要素上限，均有默认值）
val idx = map.addVectorLayer(path, style /* VectorStyle */)

map.setOnTapListener { x, y ->
    val hit = map.pickVector(x, y)                       // [layerIndex, fid]，未命中 null
    if (hit != null) { val geo = map.featureGeometry(hit[0].toInt(), hit[1]) }
    val pos = map.screenToGeo(x, y)                       // 单击转经纬度（采集/测量加点）
}
```

大数据集用 `updateVectorExtent(index, extent, maxFeatures)` 按屏幕范围增量重载（Swap-on-ready
无空窗），`hasVectorLoading()` 轮询驱动「加载中」提示。

业务叠加层（测量/轨迹/拍照标识）：`addOverlayLayer` 建运行时内存几何层，经
`updateOverlayPoints/Lines/Polygons` 覆盖式推送，`removeOverlayLayer` 移除；瞬态高亮层可
`setOverlayNoPick` 退出拾取竞争。

线程约定：
- 所有 `NativeMapView` 公开方法只做 native 状态更新 + `requestRender`，不涉及 GL 调用，可在任意
  线程调用（含 Surface 创建前）；
- 矢量读取、瓦片下载在 native 后台线程池完成，就绪后回调 `requestRender` 异步上屏。

## 功能演示 App（globecore-android 模块）

仓库内置一个演示应用，逐项展示库能力：

1. 瓦片底图（在线高德影像，**仅 3D**）+ 放大/缩小/旋转/仰角按钮 + 单击取点
2. 2D / 3D 视图切换 + 倾斜视角 + 放大/缩小
3. 矢量图层（内置 GeoJSON，默认 2D）+ 要素拾取高亮 + 放大/缩小
4. 动态叠加层（默认 2D）：测量口径——顶点标记层即时出点 + 几何层连线/成面（双图层）+ 放大/缩小
5. 定位标记蓝点 + 方向箭头（模拟 1Hz 回调）

运行：先完成「0 → 1」拿到 `.a`/头/PROJ 数据，Android Studio 打开本仓库并 **Sync**，选择
`globecore-android` 配置直接装机；或

```bat
gradlew.bat :globecore-android:assembleDebug
```

底图默认用高德在线影像（免 token，需联网），可在 `TileSources.kt` 换成自有图源。
0.1.1.1 起 Android demo **移除内置离线世界图基图**、默认高德影像，并按演示项配相机姿态按钮。

## 鸿蒙 HarmonyOS NEXT 集成（0.1.1.0 新增）

### 架构与对应关系

HarmonyOS NEXT（API 12 / 5.0.0，纯血鸿蒙，无 AOSP 兼容层）下引擎同一套 C++ 源直接参与编译，
平台差异收敛为三类适配点：

| 职责 | Android | HarmonyOS NEXT |
|---|---|---|
| 引擎源 | `globecore/src/main/cpp/*` | 经 CMake 变量 `GLOBECORE_ENGINE_ROOT` 直接引用同一目录，不复制不搬迁 |
| 日志 | `android/log` | hilog（`Log.h` 按 `__OHOS__` 分派，引擎其余代码零平台 `#ifdef`） |
| GL 宿主 | Kotlin `GLSurfaceView` 转发 surface 生命周期 | `XComponent(SURFACE)` + native EGL 渲染线程（`ohos/EglContext`、`ohos/XComponentBridge`），渲染后端仍为 GLES（向下兼容 ES 2.0 绘制指令，`render/` 层零重写） |
| 桥接 | JNI（`GlobeEngineJni.cpp`） | NAPI（`ohos/napi_*.cpp`，导出清单与 JNI 一一对应） |
| 门面 | Kotlin `com.zys.globecore` | ArkTS HAR `@zys/globecore`（`globecore-harmonyos/library`，类名/方法名/参数序完整对齐） |
| 资源 | AAR `assets/proj` | HAR `resources/rawfile/proj`（均经 `NativeSrs.initProjData` 解压到沙箱 `filesDir/proj`） |
| 产物 | AAR（`com.zys:globecore`） | HAR（`@zys/globecore`，so 名 `libglobecore.so`） |

### 集成三步曲（对齐 Android 侧口径）

**第一步：引入 HAR** —— 包名 **`@zys/globecore`**。ohpm 仓已发布（当前 0.1.3），宿主 `oh-package.json5` 直接按版本引用：

```json5
// oh-package.json5
{
  "dependencies": {
    "@zys/globecore": "0.1.3"
  }
}
```

无 ohpm 环境时备选：把 DevEco `Build Module → Make Module HAR` 产出的
`@zys-globecore-<版本>.har` 放入宿主后 `file:` 引用；源码仓库直接引用时 `file:` 指向
`library` 模块（仓库内演示工程 entry 即 `"@zys/globecore": "file:../library"`）。

**第二步：声明权限** —— 取瓦片走 native libcurl，宿主 `module.json5` 自行声明（HAR 不携权限）：

```json5
// module.json5（module 节内）
{
  "requestPermissions": [{ "name": "ohos.permission.INTERNET" }]
}
```

**第三步：启动初始化** —— Ability `onCreate` 里调一次（幂等；首次坐标转换/栅格重投影之前）：

```typescript
import { NativeSrs } from '@zys/globecore';

// 解压 HAR rawfile/proj 到 filesDir/proj 并配置 PROJ 搜索路径（this.context 为 UIAbilityContext）
NativeSrs.initProjData(this.context);
```

未初始化时坐标转换按既有口径返回 `null`（不崩溃）；验证集成：

```typescript
NativeSrs.convert(116.4, 39.9, 'EPSG:4326', 'EPSG:3857')  // 非 null 即成功
```

### 地图接入最小路径

`NativeMapView` 是 `@Component` struct（**不可 `new`**）：声明在 `build()` 里，组件引用经 `onReady`
回调挂载后再驱动 API（相机/图层可在 XComponent 就绪前设置，native 句柄在组件
`aboutToAppear` 已创建）：

```typescript
import { NativeMapView, Camera, ViewMode } from '@zys/globecore';

@Entry
@Component
struct MapPage {
  private mapView: NativeMapView | undefined = undefined;

  private initMap(view: NativeMapView): void {
    this.mapView = view;
    view.addTileLayer(`${getContext(this).filesDir}/tiles/osm`,
      'https://tile.openstreetmap.org/{z}/{x}/{y}.png', 19);
    view.setCamera(new Camera(39.9, 116.4, 3000000.0));
    view.setViewMode(ViewMode.THREE_D);
  }

  build() {
    NativeMapView({
      density: 1.0,
      // @Jin 实验性在线 GeoJSON 需要宿主传入应用私有缓存目录；本地文件矢量可留空。
      vectorCacheDir: `${getContext(this).filesDir}/globecore/vector-cache`,
      onReady: (v: NativeMapView): void => {
        this.initMap(v);
      },
    })
      .width('100%').height('100%')
  }
}
```

### @Jin 实验性在线 GeoJSON（HarmonyOS NEXT）

LITE HAR 中 `addVectorLayer` 的 `path` 可直接传入公开 GeoJSON URL；组件必须显式注入宿主私有缓存目录，HAR 不假设业务 App 的沙箱路径：

```typescript
const provinceUrl = 'http://210.74.129.84:8520/app-pub/web-data/geojson/district/province.json';

NativeMapView({
  density: 2.38,
  vectorCacheDir: `${this.context.filesDir}/globecore/vector-cache`,
  onReady: (view: NativeMapView): void => {
    const style = new VectorStyle(0x44F2C94C, 0xFFFFD54F, 2.0);
    const index = view.addVectorLayer(provinceUrl, style);
  },
})
```

- 首次加载：native `HttpClient` 下载到 `<vectorCacheDir>/url/geojson-<hash>.geojson`，LITE GeoJSON reader 解析后进入普通矢量 VBO 管线。
- 二次加载/视口重载：同一 URL 只读本地缓存，不重复网络下载。
- 日志关键字：`[RemoteGeoJson] downloaded/cache hit`、`[LiteGeoJson] 读取完成`、`[VecReload] VectorLoader 就绪`。
- P0 边界：仅公开 URL、GeoJSON WGS84、80MB/20s 默认限制；暂不支持 headers/token、ETag/304、强制刷新、下载进度、stale cache 网络失败兜底。
- LITE 边界：只内置常见 GeoJSON 读取；shp/kml/kmz/dwg/dxf/tif 等仍需非 LITE 的 OHOS GDAL/PROJ 全套依赖。
- 回退：将 `RemoteVectorCache::kExperimentalRemoteGeoJsonSource` 改为 `false` 可关闭 URL 下载；宿主不传 `vectorCacheDir` 时远程 URL 会返回“缓存目录未配置”，本地文件行为不受影响。

### 三维地形 C++ Provider（P2-1 阶段 C）

阶段 C 起，Cesium quantized-mesh 的视口调度、下载缓存、重试去重与引擎注入全部在 `globecore/src/main/cpp/terrain/CesiumTerrainProvider` 中完成。Harmony 宿主只配置数据源；仓库中的旧 ArkTS `CesiumTerrainProvider` 暂时保留用于兼容和阶段 D 参考，但 Demo 正常路径不再调用。

```typescript
const cacheDir = `${getContext(this).filesDir}/map-cache/terrain`;
view.configureTerrainProvider(
  'http://example.com/terrain/cesium',
  cacheDir, 15, 4, 3,
  { 'User-Agent': 'GlobeCoreDemo/1.0' }
);
view.prefetchTerrainProviderMetadata(); // 空闲时先取 layer.json
view.setViewMode(ViewMode.THREE_D);
view.setTerrainProviderEnabled(true);
view.refreshTerrainProvider();
console.info(view.terrainProviderStatus());
```

- 缓存布局：`<cacheDir>/layer.json`、`<cacheDir>/<z>/<x>_<y>.terrain`；元数据和瓦片均使用临时文件 + rename 原子写入。
- 调度策略：相机中心优先，默认 `tileRadius=3`（7×7 引擎瓦片窗口）；固定并发 worker 先查磁盘，未命中才联网，同一服务 DEM 在单轮及跨刷新 LRU 中去重。
- 可靠性：单请求失败重试 1 次；相机连续变化只保留最后一次刷新；视口代际变化会终止旧任务并丢弃迟到结果；失败回调不会污染已交付 LRU。
- 状态 JSON：`engineZoom/requestedEngineTiles/requestedServiceTiles/deliveredTiles/diskHits/networkRequests/retries/cacheWrites/duplicateSkips/failures/firstTileMs/elapsedMs`，可直接用于 hilog 和页面诊断。
- 真机数据（设备 `5KLBB25911200313`，太白山 tilt=55°）：热缓存 `182` 块全部磁盘命中、`networkRequests=0`、`firstTileMs=8ms`、`elapsedMs=101ms`；清数据冷缓存 `182` 个唯一 DEM、`182` 次网络、`firstTileMs=106ms`、`elapsedMs=3003ms`，去重 `84`、失败 `0`。

### @Jin 地形 Provider 相机自动跟随（实验性，方案 B）

> @Jin 实验状态：能力位于 GlobeCore C++ 层，默认 **关闭**，旧宿主不调用新 API 时保持原作者行为；当前 Harmony Demo 在地形 Provider 配置成功后显式开启。若真机体验不符合预期，宿主删除 `setTerrainProviderAutoFollow(true)` 即可回退到“手动刷新地形”模式。

- @Jin 根因：native 手势直接修改 `Navigator` 并请求渲染，原先不会通知 `CesiumTerrainProvider`；ArkTS 层只能覆盖按钮 `setCamera()`，无法可靠感知 C++ 手势结束，所以平移/缩放到新区域后必须手动点“刷新地形”。
- @Jin C++ API：`GlobeEngine::setTerrainProviderAutoFollow(bool)`。开启后，`drawFrame()` 在 GL 线程读取最新相机并计算地形任务签名；签名变化后等待约 **700ms** 防抖，再调用 `terrainProvider_->requestRefresh()`，连续手势只保留最后一次视野。
- @Jin 当前签名：只包含 3D 模式、四舍五入后的引擎地形 z、以及中心经纬度对应的 Web Mercator 瓦片 `x/y`。这与当前 `buildTasks()` 的“中心瓦片 + 固定半径 3（7×7）”选块逻辑一致；纯 tilt/heading 改变不会触发刷新，因为固定半径选块当前不消费这些参数。
- @Jin 平台 API：Harmony 为 `NativeMapView.setTerrainProviderAutoFollow(enabled)` / NAPI `nativeSetTerrainProviderAutoFollow`；Android 为同名 Kotlin API 和 JNI 方法。
- @Jin 生命周期：新增 `terrainProviderMtx_` 保护 Provider 创建、销毁、开关、刷新、状态查询和帧循环访问；Provider worker 回调 `setTerrainTile()` 不反向获取该锁，避免下载线程与 GL 线程死锁。
- @Jin 首批保护：打开地形或重新开启自动跟随后，首帧只建立签名基线，不重复打断 `setEnabled(true)` 已发起的首批 DEM 下载；2D 时暂停跟随，回到 3D 后强制记录新视野并允许一次刷新。
- @Jin Harmony 真机 `5KLBB25911200313` 初验（2026-10-27，Demo 显式开启）：HAP 构建成功；太白山首批 `168/168`、网络 168、`elapsed=5597ms`；大幅平移后无需点按钮自动刷新，`24/24`、`elapsed=712ms`；连续按钮缩放自动刷新 z12→z13→z14；2D→3D 触发 `serviceTasks=0` 的空刷新且无网络；关闭再开启地形 `182/182` 全部磁盘命中、`elapsed=239ms`；Mars3D→210 切回后 `182/182` 全部磁盘命中、`elapsed=214ms`。
- @Jin 已知边界：本能力只解决“视野变化后自动发起刷新”，没有把 7×7 固定窗口升级为真实视锥选块。若大倾角、极近景边缘仍缺 DEM，但 hilog 已出现新的 `refresh start`，后续应扩大半径或实现按 tilt/heading/FOV 的非对称视锥选块；签名也要同步纳入这些参数。

其余 API（矢量/栅格/叠加层/拾取/定位标记/手势）与 Kotlin 侧逐一对应，参「API 概览与快速上手」；
生命周期无需手动 onPause/onResume：native EGL 线程随 XComponent 加载/卸载自动 attach/detach。

> @Jin 维护约定：以下 exp1e~exp1h 真机验证、性能实验和黑面修复记录均为后续任务追加；后续新定稿记录继续带 `@Jin` 标识。

### @Jin 实验性瓦片/地形加载平滑调度（P0-2 exp1e）

> 实验状态：本轮对应构建 ID `v0.1.3-20261026-loadsmooth-exp1e`，用于验证“影像/DEM 瓦片加载期间操作地图卡顿”的削峰方案。若真机主观体验不佳，应整体 `git revert` 本轮 experiment 提交；仅把 `kExperimentalAdaptiveUploadBudget` 改为 `false` 只能关闭交互/空闲预算切换，不能回退 ready 去重、DEM 后台解码和地形局部签名等全部改动。

- 交互检测：`Navigator::isInteracting()` 在 pan/zoom/rotate/tilt 后保留 180ms 交互窗口，Renderer 据此区分手势中与抬手后的空闲补齐。
- 影像请求队列：`TileLoader::beginView()` 只推进视口代际，不再立即清空排队项和已解码结果；同 z/x/y 的 pending/inflight 任务会原地升级到新视口，减少连续手势中的重复读盘、下载和解码。
- Ready 去重与预算：新增 `readyKeys_`，已解码但尚未上传的瓦片不会在下一帧重复处理；`drainReady(maxCount,maxBytes)` 先交付当前视口，再用剩余预算接收最近旧视口结果。底图和注记层共享单帧 **4 张 / 1MiB RGBA** 上传预算。
- 3D 请求收敛：可见叶先按 z 降序、同级按视口中心距离排序；交互期只请求 96 块叶瓦片，空闲期 192 块。祖先粗瓦仍先入队，用于先形成完整低清覆盖，再细化中心高清叶。
- DEM 后台解码：quantized-mesh 解码和高度栅格化从 GL 线程移到 `setTerrainTile()` 调用线程（Provider worker）。GL 线程每帧只应用已解码 DEM，预算为交互期 2 块、空闲期 4 块；`terrainPendingKeys_` 防止同一服务 DEM 被重复解码，`terrainGeneration_` 丢弃 `clearTerrain()` 前后的迟到结果。
- 地形 VBO 局部失效：每个渲染瓦片按相交 DEM key 计算 `terrainSignature`，并以 `terrainCacheRevision_` 跳过稳态扫描；只有覆盖 DEM 集合变化的瓦片重建。单帧网格重建预算为交互 2 个、空闲 4 个；预算不足的新瓦片先建低分辨率球面后备网格，后续帧升级 24×24 地形网格，避免首批 DEM 触发全场景 VBO 重传。
- 网络与诊断：HttpClient 增加请求、连接复用/新建、HTTP/1.1/HTTP2 计数，并启用 TCP keep-alive 与 300s DNS 缓存；Renderer 每秒输出 `TILEPIPE`，字段包括交互状态、上传峰值、帧峰值、cache/network、pending/inflight/ready、HTTP 连接复用、DEM 应用和网格重建数。

真机验证（Harmony 设备 `5KLBB25911200313`，Demo 图源 ArcGIS，太白山 tilt 视角）：

- 清应用数据后冷加载 DEM：`deliveredTiles=182/182`、`networkRequests=182`、`failures=0`、`firstTileMs=58`、`elapsedMs=834`。
- DEM 集中到达秒：`terrainApplied=182`、`meshRebuilds=318`、`frameMaxMs=14.59`；截图确认真实山脊起伏，无黑屏。
- 加载后连续拖动/斜视角浏览：交互秒上传峰值 3 张 / 786432B，`frameMaxMs=23.58`，未见失败，首波后多数统计秒 `newConn=0`。
- 锁顺序修正后的安装冒烟：构建 ID 正确，热缓存 `processed=126 cache=126 network=0 ready=0`，无 native 崩溃；启动首秒 `frameMaxMs=56.47` 仍来自着色器、首批纹理和球面 VBO 初始化，保留为后续启动优化项。

### @Jin 实验性交互 LOD 迟滞与低分辨率地形后备（P0-2 exp1f）

> 实验状态：本轮对应构建 ID `v0.1.3-20261026-loadsmooth-exp1f`，目标是修复 3D 倾斜视角缩放时“先黑色块、再替换影像”的观感。初验在 Harmony 真机 `5KLBB25911200313` 通过；仍保留为实验策略，若人工主观反馈认为模糊时间过长，可整体回退本提交。

- 3D 影像 LOD 迟滞：非交互帧记录最近稳定 `targetLevel`；180ms 交互窗口内，四叉树层级使用 `min(targetLevel, lastIdleLevel)`。放大时不追逐瞬态高清叶，缩小时允许切到更粗父级，使画面优先由已上传祖先粗瓦连续覆盖，抬手后再细化高清叶。
- 状态边界：迟滞状态只复用于相同 `maxLevel` 的图层；surface 重建、清空瓦片纹理、图源切换丢弃纹理和 release 时均复位，避免新上下文或新图源沿用旧 LOD。
- 地形后备网格：网格预算不足时不再生成 `sampleTerrain=false` 的 8×8 零高程平面，而是仍采样已到达 DEM，只把细分降为 8×8；后续帧再升级为 24×24 完整地形网格，减少低分辨率后备与相邻真实山体之间的高度裂缝。
- 诊断计数：`TILEPIPE` 新增 `placeholder/ancestor/leaf/fallbackMesh`。其中 `ancestor` 与 `leaf` 是每秒 draw call 次数，不是瓦片块数；`placeholder` 表示没有自身纹理且没有祖先纹理的深色占位绘制，`fallbackMesh` 表示新增 8×8 地形后备网格。

真机验证（Demo 图源 ArcGIS，太白山 tilt=55°）：

- 热缓存 + 地形开 + 双指放大 5.2s：各统计秒 `placeholder=0`，交互帧峰值约 19.98~25.85ms，抬手后高清补齐出现一帧 `frameMaxMs=73.77ms`；`fallbackMesh` 在主要交互秒为 262/100/24/15，随后归零。
- 热缓存 + 地形开 + 双指缩小 5.2s：各统计秒 `placeholder=0`，交互帧峰值约 15.27~22.14ms，`fallbackMesh` 最高 123/s；连续截图未见黑色空窗。
- 地形关闭、仅影像双指放大：各统计秒 `placeholder=0`，证明黑块消除来自影像祖先覆盖/LOD 策略，而不是地形层遮挡；抬手后有一帧 `frameMaxMs=89.52ms` 的高清补齐尖峰。
- 清应用数据冷启动：低级别祖先瓦尚未到达的全球首秒仍有 `placeholder=4180/44`，定位太白山后祖先瓦已建立，冷加载地形与随后缩放期间 `placeholder=0`；冷 DEM `182/182` 全部网络获取、`failures=0`、`firstTileMs=31ms`、`elapsedMs=899ms`。
- 结论：缩放/浏览中的黑色块路径已被压住；冷启动全球第一秒的根瓦空窗仍存在，彻底消除需进一步预取或内置 z0~z2 低级别瓦，或做底色/雾化过渡。

### @Jin 实验性大倾角近景黑面修复（P0-2 exp1g）

> 实验状态：本轮对应构建 ID `v0.1.3-20261026-loadsmooth-exp1g`。用户在 3D、地形开启、倾角较大且距离较近时反馈屏幕下方近景影像呈黑色。初验在 Harmony 真机 `5KLBB25911200313` 通过；仍按实验提交管理，若近景清晰度/请求量不符合预期，可整体回退本提交。

- 根因：exp1f 后四叉树已经画出大倾角可见叶，但高清叶请求预算只有前 192 个；旧排序先按 z、再按屏幕中心经纬度距离，屏幕下方离相机最近但略偏离中心的叶子可能排在预算外，其 z-1/z-2 祖先也没有入队。静止时没有 pending/inflight，占位球面被持续绘制（真机日志约 `placeholder=16920~17202/s`，约 280 次占位/帧）。
- 近景优先：3D 可见叶排序改为按瓦片地形表面中心到相机的三维距离；距离近似相同时再保留“高级别优先、屏幕中心优先”的高空正视策略。距离和索引通过 scratch 数组每帧预计算，避免排序比较器重复扫描 DEM。
- 覆盖率兜底：高清叶仍只请求前 192 个近景瓦片，但底图最粗的 z-2 祖先请求覆盖全部可见叶；z-1 仍受近景预算约束。这样预算外瓦片至少由粗祖先拉伸覆盖，不再永久走深色 placeholder。
- VBO LRU：大倾角可见叶可达 700+，旧的 512 个地形网格缓存会在近景持续 LRU 抖动并重建；上限提高到 1024（24×24 地形 VBO 最坏约 24MiB）。
- 真机结果：复现“太白山 → 地形 → tilt=55° → 短放大到正常近景大倾角”后，连续帧和静止帧未见近景黑面，`placeholder=0`；热缓存地形 `182/182` 磁盘命中、`networkRequests=0`、`failures=0`。
- 边界：自动测试中持续放大到相机贴入/穿入山体时，画面仍可能因地形遮挡呈黑色，此时 `placeholder=0`，不是影像缺瓦；后续应单独增加“相机相对地形表面的最小安全高度/有效倾角钳制”。

### @Jin 实验性地形安全剔除与相机安全高度（P0-2 exp1h）

> 实验状态：本轮对应构建 ID `v0.1.3-20261026-loadsmooth-exp1h`。exp1g 解决了影像 placeholder 黑块，但真机继续在“3D + 地形 + 大倾角近景”看到黑面；新日志确认为 `placeholder=0`，根因转为地形几何剔除和相机安全高度。仍按实验提交管理，若近景最小观察距离或请求量不符合预期，可整体回退本提交。

- 根因一：`GeographicQuadtree` 的地平线、视锥 AABB、最近点和 SSE 均按海拔 0 采样；太白山 DEM 最高约 2.7km，近景大倾角时真实山脊会越过椭球面包围盒，导致覆盖屏幕下缘的地形/影像叶瓦片在选择阶段被误剔除。
- 地形安全壳：Renderer 从当前 `terrainCache_` 求最大 DEM 高程并附加 300m 余量（9000m 安全帽），通过新增 `terrainCullMarginM` 参数传给四叉树；最近点、3×3 AABB、极冠补点、子瓦片 DFS 距离和帧间缓存失效均使用该安全高程。宁可保守多保留瓦片交给深度测试，也不误删近景地形。
- 根因二：连续放大后眼点可能进入或贴近附近山脊，此时影像仍已绘制（`placeholder=0`），但视野会被背光山体占满而呈整屏暗面。Renderer 每帧在生成 3D 相机前把目标点 DEM 高度和当前缓存安全高同步给 Navigator。
- 相机安全高度：3D 目标点改为真实 DEM 表面，`altitude` 表示相对目标地形表面的高度；倾斜时若全局地形安全壳高于目标点，则按至少 150m 净空增加斜距，防止眼点穿入山脊。near/far 同步改用实际眼点—目标斜距，避免近距离地形被近裁剪面切掉。
- 真机结果：冷启动确认构建 ID 后执行“秦岭太白山 → 加载地形 → tilt=55° → 放大 2 档”，地形 `182/182`、`failures=0`、`placeholder=0`，屏幕下缘近景连续；按钮重排后不遮挡观察区域。自动连续放大到地形 z15 时，服务端会出现一批 182 个越界/限流失败，属于后续 Provider 可用级别与请求治理任务，不影响本次正常近景机位结论。

### @Jin 瓦片 LOD 诊断、运行时偏移与高分辨率密度修正

> 实验状态：默认仍使用标准屏幕误差 LOD；运行时偏移和“瓦片级=地图级”只在宿主显式调用时生效，可随时恢复，不持久化。

- 背景：真机 `MLR-AL10` 物理分辨率为 2560×1600，ArkUI 实测 `densityPixels=2.38`。Harmony Demo 此前固定传入 `density=1.0`，而 C++ 的瓦片细分判据按物理像素工作，导致高分辨率屏幕上标准 LOD 比预期多选约一级，注记文字视觉偏小。
- 密度修正：宿主应把真实 DPR 传入 `NativeMapView.density`；Demo 现使用 `display.getDefaultDisplaySync().densityPixels`。同一全球视角下，固定 `density=1.0` 时目标瓦片为 z4，传入 2.38 后标准目标瓦片为 z3，与此前人工确认字体较正常的“LOD-1”观测一致。
- 新增只读诊断：`GlobeEngine::currentTileLevel(maxLevel)` 返回当前图层目标瓦片 LOD；NAPI/ArkTS 门面提供 `tileZoomLevel(maxLevel)`，供宿主同屏显示“地图级别 / 瓦片级别”。3D 四叉树仍会按距离和 SSE 在目标 LOD 附近混入更粗叶。
- 新增运行时实验：`GlobeEngine::setTileLodOffset(-3..2)` 在标准屏幕误差结果上做整数偏移；负数使用更粗父瓦，可减少请求量并增大注记视觉字号，0 恢复标准。
- 新增同级实验：`GlobeEngine::setTileLodAlignDisplay(true)` 强制目标瓦片 LOD 等于仅由相机高度推导的 `displayLevel()`，用于测试“地图级=瓦片级”。该模式忽略视口高、FOV 与 DPR 的屏幕误差补偿，3D 全球大视口下会明显偏粗，仅建议作为对照，不建议直接作为默认策略。
- Harmony 门面：`NativeMapView.tileZoomLevel()`、`NativeMapView.setTileLodOffset()`、`NativeMapView.setTileLodAlignDisplay()`；NAPI 名称为 `nativeGetTileZoomLevel`、`nativeSetTileLodOffset`、`nativeSetTileLodAlignDisplay`。
- 缓存与限流：`TileLoader` 仍严格执行“磁盘缓存有效图像 → 未命中才联网”；HTTP 非 2xx（含天地图 429）不会写入缓存。若缓存中有历史瓦片，即使 token 被限流也应先显示缓存；卸载应用会删除沙箱缓存，冷启动后只能联网。



### @Jin C++ flyTo 相机飞行与动态叠加层 DEM 贴地实验

> 实验状态：`setCamera()` 旧行为保持即时跳转；新增 `flyToCamera()` 由宿主显式调用。动态叠加层 DEM 贴地仅作用于 `isOverlay=true` 的内存矢量层，文件矢量层仍走旧路径，必要时可只回退 Renderer 中的 overlay 分支。

- @Jin 相机能力下沉：C++ `GlobeEngine::flyToCamera(camera, durationMs)` 在 GL 帧循环中用 smoothstep 插值经纬度、高度、heading、tilt、roll 和 FOV；heading 走最短角方向，动画结束帧精确落到目标相机。手势平移、缩放、旋转、俯仰以及新的 `setCamera()` 会取消旧动画，避免飞行和手指操作互相抢相机。
- @Jin 双平台 API：Harmony NAPI/ArkTS 新增 `nativeFlyToCamera` 与 `NativeMapView.flyToCamera()`；Android JNI/Kotlin 同步新增同名能力。Demo 的秦岭太白山、西岳华山、喜马拉雅·珠峰快捷定位已改为 1.8s 平滑飞行。
- @Jin LOD Demo 入口收敛：Demo 只保留“标准”和“-1”两档；C++ SDK 的 `-2/-3/同级` API 保留，供后续专项实验直接调用。
- @Jin 动态矢量贴地根因：旧 3D overlay VBO 按海拔 0 球面一次性烘焙，再通过 `uVecAlt` 对所有顶点统一抬高，只能避免 z-fighting，不能让线、面随 DEM 山脊/沟谷起伏，因此斜视时会被山体遮盖或裁剪。
- @Jin 实验修复：Renderer 新增按经纬度采样 DEM 的 `sampleTerrainHeightLonLat()`；动态 overlay 的 fill/line/outline/point/icon 在 3D 上传时逐顶点采样真实高程，并加 1.5m 微小表面偏移。线沿线加密，面三角形按最长边递归细分到约 90m（最多 5 层），减少大三角形内部穿坡。
- @Jin DEM 到达后自动重建：`VectorLayer` 记录 3D overlay VBO 使用的 `terrainCacheRevision`；DEM 新增或淘汰后，下一帧删除旧 globe VBO 并按新高程重建，解决“第一次上传时地形未到，之后线面一直停留在海拔 0”的问题。
- @Jin 当前边界：本轮贴地实验只改动态叠加层，文件矢量和拾取几何尚未全部改为 DEM 贴合；超大面内部细分仍偏保守，后续若业务叠加大数据量，需要再做空间索引和视相关预算。
- @Jin 真机排查到叠加层“只见点、线面不显示”的根因（2026-10-27，Harmony `5KLBB25911200313`）：该设备 NAPI 的 `napi_get_typedarray_info()` 返回的 `length` 是**字节数而非元素个数**（Float64Array 6 个 double 返回 48）。旧 `napi_util.h::NapiGetFloat64Array/NapiGetInt32Array` 直接把它当元素数，导致 3 个点被读成 24 个、坐标尾部补 0；线/面顶点数被放大 8 倍且后半坐标为 0，墨卡托转换异常后几何被丢弃，图标 Int32 像素也会越界读 4 倍。
- @Jin 修复：`NapiGetFloat64Array`/`NapiGetInt32Array` 用底层 ArrayBuffer 容量（`napi_get_arraybuffer_info` 的字节数减 offset）反推元素数做归一——`length` 超过按元素计算的容量时判定为字节长度并除以元素字节数，兼容标准元素口径与 subarray；标准 NAPI（返回元素数）行为不变。修复后真机日志点层 `pts=3/1`、线 `hits=44/44`、面 `hits=991/991`、图标 1 点，全部命中 2542~3625m 真实高程。
- @Jin 修复贴地层每帧无谓重建：旧 `overlayDrapedTerrainSignature()` 直接遍历 `terrainCache_`（`unordered_map`）做 FNV 哈希，瓦片到达/淘汰后桶遍历顺序变化，使同一批 DEM 每帧算出不同签名，叠加层被反复 reset+重建。改为先收集命中 DEM 的稳定 word、`std::sort` 后再哈希，签名对同一组 DEM 顺序无关且稳定；真机静止 4 秒重建次数由“每帧都重建”降为 0。
- @Jin 图标锚点：3D overlay icon 顶点由“图片中心对地理点”改为“PNG 底边中心（针尖）对 DEM 采样点、图标向上展开”，`poi.png` 水滴针尖扎在地形点。当前为全局 overlay icon 行为，后续若需兼容中心锚点图标，应在 `VectorStyle` 增加 anchor 配置。
- @Jin 修复 3D 贴地折线/描边整条不可见（2026-10-27）：draped 的 stroke 三角带在 `uploadGlobeOverlayDrapedChunks()` 里误存了**绝对 ECEF**（量级约 6.37e6 米），而顶点着色器还要再加 `uOffset = anchor − eye`，位置被双重叠加后整条被视锥剔除，现象是“线完全看不见/像陷到地下”。已改为与 fill/point 同口径存“相对图层锚点的 RTC 坐标”（AABB 仍用绝对 ECEF），修复后折线完整跨山脊显示。
- @Jin 修复贴地面“破碎、一块显示一块被地形遮挡”：密采沟谷（已把细分由 90m/5 层加密到约 20m/7 层，面采样 991→3934、线 44→140）的叠加矢量与离散影像地形网格（8×8/24×24）并非同一曲面，fill 原本开 `GL_DEPTH_TEST` 会把顺谷片元按粗地形三角面剔除。实验性方案（可回退）：对 `isOverlay` 的 fill/stroke/point/icon 四个 pass 在逐层绘制时 `glDisable(GL_DEPTH_TEST)`，叠加层本身 `depthMask=FALSE` 走画家序，故稳定盖在地表且不破坏彼此顺序，pass 结束立即 `glEnable(GL_DEPTH_TEST)` 恢复；文件矢量仍保留原深度测试。
- @Jin 修复 3D 开启地形后加载省级 GeoJSON 卡死/被杀（2026-10-27）：上一轮为太白山小区域把贴地细分固定为约 20m、递归 7 层，该策略对省级 MultiPolygon（真机约 8.1 万原始 fill 顶点、覆盖约 1081km 跨度）会递归放大到 18 万+输出、单轮 82 万次 DEM 采样并在 GL 线程长阻塞，3D 下卡死、进程被系统回收；2D 不走 draped 路径所以正常。
- @Jin 大矢量贴地保护策略（实验性、可回退）：细分密度按叠加层 bbox 跨度自适应，目标边长 = clamp(跨度/224, 20m, 32km)；当原始 fill 顶点超过 2 万（线/描边角点超过 1.2 万）时视为“已足够密的大层”，直接对原始三角网/原始角点逐顶点采 DEM，不再计算边长、不再递归细分或插点；另设 fill 18 万顶点、stroke 12 万角点硬预算兜底。真机省级层输出降回 80,952 fill 顶点、DEM 采样 82.8 万→10.8 万，3D+地形下地图可正常拖动且进程稳定；太白山小样例仍走 20m/7 层细分，保持贴地精度。

### @Jin 本地纯线 DEM 贴地与高等级视角地形遮挡修复（drape36e）

> @Jin 实验状态：对应构建核对 ID `v0.1.3-20261029-local-line-drape36e`。本轮只接管 HarmonyOS NEXT Demo 中由陕西 Polygon/MultiPolygon 外环和洞环转换得到的**本地文件纯 `MultiLineString` 层**，不改变在线大面、内存 overlay、点、图标和面填充的既有路径；实验开关为 `Renderer::isExperimentalLocalDrapedLineLayer()` 内的 `kExperimentalLocalVectorLineTerrainDrape`，置 `false` 可整体回退。

- @Jin 问题背景：陕西面转线在 3D 地形开启后，地形瓦片加载过程中会反复埋住平面线 VBO；低高度时还会出现线条陷入沟谷、没有跟随 DEM 起伏的问题。
- @Jin 低空真实贴地：本地纯线复用大图层 `uploadGlobeRemoteDrapedChunks()` 的 DEM 通路，等待精细 DEM 覆盖后沿线加密、逐点采高，按每个 MultiLineString ring 独立生成 chunk，并在地形修订后通过稳定签名防抖重建；flyTo 和手势操作期间延迟重建，避免为全省线同步采高造成卡顿。
- @Jin 地形加载过渡期可见性：最终 DEM 线 chunk 与过渡平面线 chunk 均逐 chunk 标记 `terrainDraped`，`drawStripChunks()` 内按 chunk 切换 `GL_DEPTH_TEST`，避免一批粗地形到达时省界线短暂掉到地形下面。
- @Jin 高等级视角策略：z3~z6 等高空视角不为全省线条执行全量 DEM 烘焙，只要 `terrainCache_` 已存在就临时关闭该纯线层对地形的深度测试；低空仍恢复真实 DEM 采高，无地形时 `terrainCache_` 为空并保持普通深度路径。
- @Jin 真机记录（2026-10-29，设备 `5KLBB25911200313`）：自动场景直接设置相机高度，HUD 实测 z3 为 3000km / `地图 z3 / 瓦片 z6`、z4 为 1700km / `地图 z4 / 瓦片 z7`、z5 为 850km / `地图 z5 / 瓦片 z8`，三档品红陕西省界均连续可见；地形全部本地缓存命中（z3 为 `80/80`、`network=0`、`failures=0`）。用户人工复核确认 z4/z5/z6 已无遮挡问题。
- @Jin 回归：`shaanxi-line-3d-off` 无地形纯线正常，`taibai-samples` 的 overlay 点/线/面/POI 正常，`province-3d-taibai` 在线省级大面场景仍可加载；本轮 HAP 编译通过、`git diff --check` 通过。
- @Jin 已知边界：高空方案优先保证线条可见，不在全球视角对全省每个顶点做真实地表贴合；若以后接近地平线旋转时观察到穿透、背面可见或浮线，需要继续做视口范围深度策略或真实地表 ribbon。
- @Jin **未解决待办**：
  - [ ] @Jin 陕西省级 Polygon/MultiPolygon“大面”在 3D 地形下仍存在被地形遮挡的问题，且不只发生在 z4/z5/z6；该问题与本轮已修复的“本地纯 MultiLineString 省界线”不是同一条路径，后续需单独优化大面 fill/outline 的 DEM 覆盖判定、分 chunk 深度策略、透明填充零几何构建以及边界 ribbon 贴合。

### 演示工程 globecore-harmonyos

DevEco Studio（5.0+，API 12 SDK）打开 `globecore-harmonyos/`，`ohpm install` 后直接 run `entry`：
清单页 `Index` 五项（底图 / 3D / 矢量 / 叠加层 / 定位标记）路由到综合演示页
`MapDemoPage`，能力口径对齐 `globecore-android`。矢量/栅格样例文件经 hdc 推送到应用沙箱
`filesDir` 即自动被演示页发现：

```bat
hdc file send D:\data\sample.shp /data/app/el2/100/base/com.zys.globecore.harmonyos/files/sample.shp
```

（需开发者模式 + root / `hdc shell mount -o remount,rw`；无样例文件时底图功能不受影响，仅 toast 提示。）

编译前提：先完成「0 → 1」鸿蒙分支（`build-all.sh --target ohos` 产出 `build-scripts/out/ohos`），
否则 CMake 报找不到 `.a`。常见问题：
- **`cannot find -lxxx` / IMPORTED 库不存在**：鸿蒙 `.a` 未产出，或 ABI 目录名与 `OHOS_ARCH` 不一致。
- **`dlopen failed: libglobecore.so`**：HAR 打包未含目标 ABI 的 so；真机 `arm64-v8a`，模拟器 `x86_64`。
- **坐标转换全部返回 null**：漏了第三步 `initProjData`；升级 PROJ 数据需先清应用数据（解压按
  「文件已存在即跳过」）。
- **日志查看**：`hdc shell hilog | findstr GlobeCore`（引擎 `Log.h` 鸿蒙分支统一走 hilog，TAG=globecore）。

## 桌面宿主（globecore-desktop）

Windows / Linux（含麒麟/UOS/龙芯）第四宿主：Qt5 Widgets + `QOpenGLWidget` **直接调用引擎 C++ 核心**
（无 JNI/NAPI 编解码薄层）。引擎侧仅「加分支」适配：`Log.h` 加 `GLOBECORE_DESKTOP` 分支（stderr）、
部分 JNI 导出段加 `!GLOBECORE_DESKTOP` 守卫，移动端行为零影响。GL 后端：Windows 必走 **ANGLE**
（原生桌面 GL 无 `ARB_ES2_compatibility`，GLSL ES 着色器编不过）；Linux 走 Mesa GLES2 或 glew。

```bash
# Windows（Git-Bash；VS2019 + Qt 5.15.2 + ANGLE + vcpkg 三方，项目内自包含布局）
cd globecore-desktop && bash tools/gcd-rebuild.sh && bash tools/gcd-deploy.sh

# Linux / WSLg / 麒麟 / UOS（系统包 GDAL/PROJ/curl/sqlite；勿在 /mnt 挂载盘构建）
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux -j8
```

两端均已实测跑通 XYZ 底图、3D 球面、GDAL 矢量（含 KML/KMZ）、栅格 GeoTIFF（含投影重投影 VRT、
超大栅格 sidecar 缓存）与中文标注。目录结构、GL 后端决策、三方库来源、冒烟工具与已知格式缺口
详见 **`globecore-desktop/README.md`**。

## 版本说明

- **0.1.5.0（开发中，分支 `feature/terrain-pre`，未发版）**：三维地形、手势稳定性与瓦片调度专项——
  ① **三维地形（quantized-mesh v1.0）**：新增宿主侧 DEM 注入通路 `setTerrainTile/clearTerrain`，引擎按服务瓦片真实 bbox 全局共享并采样多块 DEM（墨卡托渲染瓦片与 EPSG:4326 地理网格边界不重合，改单块粗 DEM 外推为四角/边中点收集全部相交服务瓦片）；`QuantizedMesh` 索引改按规范 **high-watermark** 解码（`index=highest-code; code==0 则 highest++`），修正此前山脊/沟谷整体错位；地形细分提升（24 分格/瓦片），DEM 解码在 GL 线程逐帧削峰（每帧 3 块、余留回队并驱动 WHEN_DIRTY 连续出帧），地形瓦片改 **静态 VBO + LRU512**、着色器以 `uRtcOffset/uTileCenter` 取代逐顶点 `uEye`，稳态浏览零顶点重建零重传；
  ② **双指缩放抬手跳变修复**：根因为多指→单指切换时残余孤立的单帧巨幅 panBy（真机实测最大约 1643px）。ArkTS 识别器层（`MapGestures`）改为 Up 只删除抬起手指 id 而不全量重建手指表、单指独立平移基准、多指转单指 4 帧宽限窗口（60vp）与常规 100vp 单步钳制、fling 速度钳 6000vp/s 与 dt 钳 32ms；native `Navigator::panByPixels` 增加单帧 >200px 静默丢弃作 2D/3D 通用兜底；
  ③ **瓦片请求与初始加载优化**：`TileLoader` 新增视口代际 `beginView(signature)`（2D 用可见范围签名、3D 用排序后可见瓦片集签名），视口集合不变的补帧保留队列、集合变化即丢弃未开工排队项与旧就绪字节，在途同 key 若仍被新视口需要则升级代际复用，`drainReady` 只接收当前代际、单帧 32 张上传预算不被旧视口抢占，`hasPendingOrReady` 计入在途任务；在线瓦片层 worker 调为 6、祖先链深度收敛为 2，并增加 cache/network/failed 聚合统计日志。**首屏粗瓦优先**：2D/3D 请求投递顺序改为先 z-2、再 z-1，最后可见叶，在线 XYZ 源 worker 出队同步采用低 z 优先（本地 GDAL 栅格仍高 z 优先，避免慢粗瓦堵死），冷启动时先以少量祖先瓦完整低清覆盖再由叶子替换变清。真机清缓存日志确认首批处理顺序为 z11 → z12 → z13；早期版本单层瓦片由约 965 块（横跨 z8~13）降至 204 块（z13 叶 + 两级祖先），二次启动全部命中磁盘缓存（network=0）；
  ④ **鸿蒙 LITE 构建**：新增 `ohos/lite_gdal_stubs.cpp`，在 `GLOBECORE_LITE=ON` 下以最小桩替代 GDAL/PROJ/KML 依赖链，并补齐对应 CMake 源清单、NAPI 桥接与 `index.d.ts` 声明；
  ⑤ **3D 倾斜/缩放黑区修复**：`GeographicQuadtree` 帧间缓存原先只比较眼点位移、FOV 与级别参数，且位移阈值误用地心距 1%（50 km 高度时约 64 km）；两指倾斜或缩放后高度/朝向已变化却继续复用旧叶集，真机出现中心影像带之外大片黑区。现缓存同时校验相机 `forward/up`（0.1°）并把位移阈值改为离地高度的 0.25%（下限 20 m），眼点相同但纯旋转也会立即重建；真机以 tilt 55°→约 80°、随后双指缩小复测，左右/下方黑带消失。
  ⑥ **P2-1 阶段 A：地形 Provider 下沉 Harmony HAR**：原 Demo 内 `CesiumTerrainProvider` 迁入 `@zys/globecore`，公开 `TerrainSourceOptions`（`baseUrl/cacheDir/maxZoom/concurrency/headers`）和网格工具函数；移除 HAR 内硬编码地形地址，`layer.json` 的 `tiles/maxzoom/available` 动态生效，保留并发下载、磁盘缓存、EPSG:4326/TMS bbox 精确求交与流式交付。Demo 改为从 HAR 导入并只注入 210 数据源配置；真机太白山热缓存复测 `DEM=182、engineTiles=49、fetchMs=64、skip=0、fail=0`。
  ⑦ **P2-1 阶段 B：地形元数据与网格核心下沉 C++**：新增共享 `terrain/CesiumTerrain`，统一解析 `layer.json` 的 `tiles/maxzoom/available`，提供 Geographic/TMS 与 Web Mercator bbox 求交、available 判断及服务瓦片四至计算；Harmony NAPI 新增 `nativeCesiumTerrainCreate/Destroy/MaxZoom/TileTemplate/EngineToServiceTiles/TileBounds/IsAvailable` 薄桥接，HAR Provider 正常路径不再用 `JSON.parse` 解析元数据，并新增幂等 `dispose()` 释放 C++ 句柄。HAP 编译通过；真机日志确认 `CesiumTerrainCore: projection=EPSG:4326 scheme=tms maxzoom=15 availableLevels=16` 与 ArkTS `core=cpp`，太白山热缓存复测仍为 `DEM=182、engineTiles=49、fetchMs=64、skip=0、fail=0`。
  ⑧ **P2-1 阶段 C：C++ 地形 Provider 与视口调度**：新增平台无关 `terrain/CesiumTerrainProvider`，由 native 直接读取 navigator 相机视口并按中心优先生成 7×7 引擎瓦片任务；内置 `layer.json` 元数据预取、磁盘缓存（`layer.json` 与 `<z>/<x>_<y>.terrain` 均临时文件 + rename）、固定并发、失败重试、视口代际取消、单轮/跨刷新 LRU 去重，以及 `GlobeEngine::setTerrainTile` 直接流式注入。Harmony NAPI/HAR 新增 `configureTerrainProvider/setTerrainProviderEnabled/refreshTerrainProvider/prefetchTerrainProviderMetadata/terrainProviderStatus`，宿主不再负责 DEM 网格换算、下载和逐块调度；`clearTerrain` 会同步失效 Provider 交付 LRU。真机太白山热缓存 `182/182` 磁盘命中、`network=0`、首块 `8ms`、总耗时 `101ms`；清数据冷缓存 `182` 个唯一 DEM、`182` 次网络、首块 `106ms`、总耗时 `3003ms`、去重 `84`、失败 `0`。
  ⑨ **P2-1 阶段 D1：Android 地形 Provider 门面对齐**：Android `NativeLib/NativeMapView` 新增与 Harmony 同名的 `configureTerrainProvider/setTerrainProviderEnabled/refreshTerrainProvider/prefetchTerrainProviderMetadata/terrainProviderStatus`；JNI 将 Kotlin `Map` 请求头按 `[name0,value0,...]` 展平后还原为 native 键值对，所有视口调度、下载、缓存、重试和注入仍复用共享 C++ Provider。`:globecore:compileDebugKotlin` 与 JNI 单目标 C++ 编译通过；完整 AAR 本机因仓库未放置第三方 `jniLibs/*.a` 前置库暂无法链接。
  ⑩ **P2-1 阶段 D2/D3：统一地形事件与 Qt Provider 门面**：C++ `CesiumTerrainProvider` 新增单行 JSON 事件（`metadata_started/metadata_ready/refresh_started/first_tile/progress/refresh_completed/error/enabled/disabled`），每条携带完整诊断快照，进度事件每轮最多约 10 次；Android JNI 按需附加 JVM 并调用 `NativeMapView.onTerrainProviderEvent` 后 `post` 到主线程，Harmony NAPI 使用容量 32 的 TSFN 非阻塞投递，Qt `MapWidget` 新增同名配置/开关/刷新/状态门面与 `terrainProviderEvent(QString)` signal，并用 `Qt::QueuedConnection` 转投 GUI 线程。Demo 已移除 900/3200/6000ms 轮询，改为事件驱动状态栏。Harmony HAP 编译与真机验证通过：热缓存事件链完整，`delivered=2/2`、`diskHits=2`、`networkRequests=0`、`firstTileMs=2`、`elapsedMs=7`、`failures=0`；关闭地形收到 `disabled`。Android Kotlin 与 JNI/C++ 三个目标编译通过；Qt 桌面端源码已补齐，但本机缺少 GDAL/Qt 桌面依赖，CMake 配置未完成。
  ⑪ @Jin **P0-2 exp1e：瓦片/地形加载期交互平滑实验（实验性）**：新增 180ms 交互窗口；所有瓦片层共享 4 张/1MiB 单帧上传预算；TileLoader 不再随相邻视口立即清空队列和 ready，pending/inflight 同 key 可升级代际，新增 ready key 去重；2D 中心优先、3D 交互/空闲分别只请求 96/192 块叶瓦片并保持祖先粗瓦先覆盖；quantized-mesh 解码移到 Provider worker，GL 线程按 2/4 块应用 DEM，地形网格改按局部 DEM signature + revision 精确失效并按 2/4 个预算重建；新增 TILEPIPE 与 HTTP 连接复用诊断。真机冷地形 182/182、失败 0、首块 58ms、总耗时 834ms，DEM 到达秒 frameMaxMs=14.59ms，连续拖动 frameMaxMs=23.58ms。本轮为实验提交，效果不佳可整笔 revert，详见“实验性瓦片/地形加载平滑调度”。
  ⑫ @Jin **P0-2 exp1f：交互 LOD 迟滞与低分辨率地形后备（实验性）**：3D 手势中使用最近非交互帧的稳定瓦片 LOD 做细化迟滞，放大时不追逐瞬态高清叶、缩小时允许父级粗图覆盖，避免自身高清纹理与祖先纹理短暂同时不可用时绘制深色占位；迟滞状态按图层 `maxLevel` 隔离，并在 surface 重建、纹理清空、图源切换和 release 时复位。地形网格预算不足时仍采样已有 DEM，只从 24×24 降为 8×8，不再生成零高程平面；新增 `placeholder/ancestor/leaf/fallbackMesh` 诊断。真机太白山地形开启双指放大/缩小及关闭地形仅影像三组缩放均记录 `placeholder=0`；冷启动全球首秒在根瓦到达前仍有占位，后续需通过低级别瓦预取/内置或底色过渡单独优化。详见“实验性交互 LOD 迟滞与低分辨率地形后备”。
  ⑬ @Jin **P0-2 exp1g：大倾角近景黑面修复（实验性）**：3D 可见叶改为按地形表面中心到相机的三维距离排序，保证屏幕下方最近区域优先获得高清叶；底图 z-2 粗祖先请求覆盖全部可见叶，预算外近景瓦片不再因“自身高清纹理和祖先纹理均缺失”而永久绘制深色占位；排序距离通过 scratch 数组每帧预计算，地形 VBO LRU 从 512 提到 1024，避免 700+ 可见叶时网格缓存反复淘汰重建。真机太白山正常近景大倾角视角下 `placeholder=0`、近景无黑面；相机穿入山体的过近机位另列地形安全高度任务。详见“实验性大倾角近景黑面修复”。
  ⑭ @Jin **P0-2 exp1h：地形安全剔除与相机安全高度（实验性）**：四叉树地平线/视锥/AABB/SSE 接收 DEM 安全高程，地形抬高后的近景山脊不再按海拔 0 被误剔除；Renderer 每帧向 Navigator 同步目标点地形高和全局安全高，3D 目标点落在 DEM 表面，倾斜时保持至少 150m 净空，near/far 改用实际斜距。真机太白山地形开启、55° 倾角、近景放大时 `182/182、failures=0、placeholder=0`，屏幕下缘连续无黑面。详见“实验性地形安全剔除与相机安全高度”。
  ⑮ @Jin **3D 动态叠加层贴地 stroke 飞线修复**：`Renderer::uploadGlobeOverlayDrapedChunks()` 在线/面描边沿 DEM 加密时，曾以 `const DrapeVertex &prev = corners.back()` 持有上一角点，随后继续向同一 `std::vector` 插入约 20m 加密点；vector 扩容后该引用悬空，插值经纬度可能读到垃圾内存（真机日志出现异常大经度），再烘焙为 ECEF 后表现为青色线和黄色面描边从太白山样例区域辐射到屏幕边缘，容易被误判为 GeoJSON 解析错乱。现改为先复制上一角点值再插点；临时真机诊断同时确认修复后小范围面层 DEM 命中 `3992/3992`、线层 `159/159`，省级 GeoJSON 大层保护路径仍可加载和缩放；动态叠加面入口同时允许宿主只传真实角点（四边形传 4 点），内部自动补回首点让面描边闭合。构建核对 ID：`v0.1.3-20261027-drape-strokefix2`。
  ⑯ @Jin **HarmonyOS NEXT 在线 GeoJSON P0 真机验收**：`feature/geojson-url-data` 分支在鸿蒙 HAR LITE 构建中打通 `ArkTS addVectorLayer(httpUrl) → NAPI vectorCacheDir → C++ RemoteVectorCache 原子下载缓存 → lite_geojson_reader → VectorBuilder/VBO/拾取/贴地`。真机首次下载 669,489 bytes、解析 4 个 MultiPolygon 要素；重启后二次加载 `cache hit elapsed=0ms`、read/build 共约 268ms，且不再走 ArkTS 下载或 overlay 兜底。3D 地形开启后四省边界仍可显示；能力由 `RemoteVectorCache::kExperimentalRemoteGeoJsonSource` 守卫，可一键回退。
  ⑰ @Jin **在线 GeoJSON 岛洞解析、远程大面贴地与动态 overlay 边界修正（实验性，构建核对 `v0.1.3-20261029-remote-vector-drape35`）**：在 P0 URL Source 基础上继续修复 HarmonyOS NEXT LITE GeoJSON 对 Polygon/MultiPolygon 岛洞环的解析与三角剖分回归，陕西省级 `province.json` 真机解析为 4 个 MultiPolygon 要素，其中陕西外环 2434 点、3 个洞、洞点 51 个，未再出现洞间多余连线或几何错乱；远程 GeoJSON 与内存 overlay 的 3D VBO 路径拆分，在线大面使用独立 `uploadGlobeRemoteDrapedChunks()` 实验通路，按当前 DEM 覆盖细分、逐 chunk 标记 `terrainDraped`，运动/flyTo 中延迟重烘焙，停顿后再生成贴地 VBO，避免浏览过程中为整省大面同步采高造成卡顿。内存 overlay 的面填充在 DEM 烘焙后按 `isOverlay` 整层关闭对地形的深度测试，修复太白山小面被粗地形三角面局部遮挡造成的“破碎”；面外边界新增实验性 conformal outline ribbon：不再用屏幕空间横向扩边的 stroke shader，而是将中心线沿 DEM 加密、左右边分别偏移并重新采高，生成随坡面起伏的 `GL_TRIANGLES` 地表窄三角带（10km 测试面真机日志 `outline=0、conformalOutlineRanges=1、ribbonVerts=4476、ribbonHalfM=27.3`）。该边界方案以固定世界宽度换取旋转时不“飘”，尚未做到恒定像素宽度，后续可继续优化；开关和代码均集中在 `Renderer.cpp`，可整体回退到旧 stroke。透明填充样式（fill alpha=0）已由 Demo 验证 2D/3D 仅显示省界，但当前仍会构建 fill 几何，后续需在 `fillAlpha<=0` 时跳过 earcut/fill VBO/远程贴地烘焙。
  ⑱ @Jin **本地纯线 DEM 贴地与高等级视角遮挡修复（实验性，构建核对 `v0.1.3-20261029-local-line-drape36e`）**：新增仅针对本地文件、非 overlay、非在线 GeoJSON、无 fill/outline/point/icon 的纯 `MultiLineString` 实验通路；低空等待精细 DEM 后沿线加密采高并按 ring 分 chunk，手势/flyTo 中防抖延迟重建；地形加载过渡期和 z3~z6 等高等级视角逐 chunk 临时关闭线对地形的深度测试，避免粗地形瓦片把陕西省界埋住。真机 HUD 实测 z3/z4/z5 场景品红线连续可见，用户人工确认 z4/z5/z6 通过；无地形、太白山 overlay 样例和在线省级大面加载回归正常。遗留：陕西省级 Polygon/MultiPolygon 大面在 3D 地形下仍会被遮挡且不限级别，已列入后续未解决待办。
  > 维护约定：**每次改动定稿后同步更新本版本说明**（与代码提交同批），未发版工作累积在本条，发版时再改版本号与“当前发布”标记。
- **0.1.4.0（当前发布）**：三项合入——
  ① **GeoPackage 工程读写**：新增 `bridge/geopackage_io.cpp`，OGR GPKG 驱动实现工程文件（七表：tracks / measures / photos / media_groups / survey_plots / project_meta / layer_refs）整体读写，Kotlin/ArkTS 侧以 JSON 跨桥传输；写入整文件 tmp+rename **原子替换**，五张要素表 **SetFID 落盘**使记录 id 持久化（重写不重编号），读侧缺列宽容回退、写侧整文件重建自动升级 schema；
  ② **着色器精度修复**：顶点着色器中 `vNormal`/`vViewDir` 归一化移至顶点阶段，避免 `mediump` varying 插值大数值 ECEF 坐标丢精度，修复真机 3D 光照无明暗过渡问题（模拟器 SwiftShader 不受影响）；
  ③ **瓦片缓存 MBTiles 后端**：`TileCache` baseDir 以 `.mbtiles` 结尾即走 SQLite 单文件后端（WAL、TMS y 翻转、标准 tiles/metadata 表），tiles 表带扩展列 `added_at`（入库时间 Unix 秒，新建即带、老库打开自动 ALTER 补列、旧行 NULL=未知）；目录式 `<z>/<x>_<y>.tile` 旧布局保留可用，双后端按路径后缀分派。
  v0.1.4.0 tag 后追加（未重新打版）：鸿蒙 HAR CMake 补齐 11 个落后引擎源（修复 `libglobecore.so`
  链接缺符号）；`geopackage_io` photos 表新增 `photo_data` BLOB 列（主照片随工程文件入库，
  分享单文件自带照片，文件缺失时读侧回退库内 BLOB）。
- **0.1.3.x**：tif/img 栅格加载专项与稳定性修复——手写 PNG 编码器取代缺失的 GDAL PNG 驱动（栅格瓦片可生成可落盘）、投影栅格 LOCAL_CS 退化修复与四至单一口径（`raster/RasterSrsRepair.h`）、新增 `removeTileLayer` 删层墓碑回收、读带统一 WGS84 VRT 口径；修复图层容器 UI/GL 线程竞争崩溃（`GlobeEngine::layersMtx_`），瓦片加载改「级别最细优先 + 祖先链深度帽」排序（叶子先出图）；另含 RHI 抽象层（GLES2 默认，Vulkan 骨架经 `GLOBECORE_VULKAN_BACKEND` 编译守卫）与 terrain/gltf/MVT/KTX2 模块骨架。技术明细见仓库内 `版本迭代.md`（本地文件）。
- **0.1.2.0**：修复两处 KML/KMZ 渲染色问题——① 无名/非 styleUrl 型 KML/KMZ（ArcGIS、
  MapInfo 导出）按「几何前两个顶点坐标签名」兜底取色，避免要素全无名时按名映射恒空而不按原文件配色；
  ② 图层管理改「整层样式」生效：新增 `hasFillColor`/`hasLineColor` 按通道覆盖标志，仅在整层未显式设该
  通道时才用 KML 源色覆盖，使填充/描边颜色可被整层样式覆盖、线宽统一。改动落在 `vector/`（`KmlStyle`、
  `VectorReader`、`VectorBuilder`、`VectorGeometry`）、`GlobeEngine` 及 JNI/NAPI 桥接与 `VectorStyle` 门面，
  两端共用引擎源同步；Android `com.zys:globecore:0.1.2.0` 与鸿蒙 `@zys/globecore:0.1.2` 一并发版，
  API/ABI/产物兼容 0.1.1.0，三方库版本未变更。
- **0.1.1.1**：Android / HarmonyOS 两端 **demo** 调整（不改库 API/ABI/产物）——删除内置离线基图、默认改高德在线影像；
  演示项按 01 仅 3D（放大/缩小/旋转/仰角按钮）、02 2D/3D 切换、03/04 默认 2D 重排；鸿蒙矢量 demo
  随包 `rawfile/sample.geojson`（解压到 `filesDir`）免推送即可拾取；两端 demo 删除坐标转换演示页（PROJ demo 入口，库 `NativeSrs` 保留），测量叠加层改「顶点标记层 + 几何层」双图层
  （对齐 MobileMap-android），首点即出点给出即时点击反馈。引擎与桥接层零改动。
- **0.1.1.0**：新增 HarmonyOS NEXT 双平台支持（`globecore-harmonyos` 工程 + `build-scripts --target ohos`）；
  Android 侧仅 `Log.h` 平台分派适配，API/ABI/产物与 0.1.0.0 完全兼容；三方库版本未变更。
  GitHub Release tag `v0.1.1.0` 与 Maven `com.zys:globecore:0.1.1.0` 发布随本次改造一并打点
  （当时走 `.github/workflows/publish-maven.yml` 自动发布；该 workflow 现已移除，改为本地手工部署）。
- **0.1.0.0**：Android 单平台首个正式版（AAR + Maven 仓 + prebuilt-libs 产物包）。

## 许可与第三方声明

本项目为**独立实现**，仅参照同类开源地球渲染器的公开行为与概念进行设计，**未使用、未链接、未衍生**任何
第三方渲染引擎源码；本项目与任何第三方开源项目**无任何隶属或背书关系**。
本仓库源码按 MobileMap 项目许可发布。产物 AAR 静态链接以下第三方库，对外分发时须保留其版权声明：

| 库 | 版本 | 许可证 |
|---|---|---|
| GDAL | 3.7.0 | MIT |
| PROJ | 9.0.0 | MIT |
| libkml | 1.3.0 | BSD-3 |
| SQLite | 3.37.2 | public domain |
| expat | 2.4.8 | MIT |
| zlib | 1.2.13 | zlib |
| uriparser | 0.9.8 | MIT |
| minizip | (zlib 附带) | zlib |
| OpenSSL | 1.1.1w | Apache-style 2.0（并入 libcurl.a） |
| libcurl | 8.4.0 | curl |
| boost（仅头文件，libkml 依赖） | — | BSL-1.0 |
