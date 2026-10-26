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
| 瓦片调度 | 后台线程池取瓦片（先盘后网、原子写盘、失败冷却退避），GL 线程仅异步上传并按帧限额；P0-2 exp1f 后视口代际（view-generation）只推进代际、不再立即清空队列/ready，同 key 排队或在途任务可原地升级，已解码待上传瓦片用 ready key 去重；所有瓦片层共享每帧 4 张 / 1MiB 上传预算，当前视口优先、最近旧视口随后补齐；在线图源先投 z-2/z-1 祖先粗瓦，再以屏幕中心优先替换叶子，3D 叶瓦片请求限制为交互 96 块、空闲 192 块；交互期增加 LOD 迟滞，优先保持祖先粗图连续覆盖，大倾角近景按相机三维距离排队并保证全部可见叶至少具备 z-2 粗祖先覆盖，显著压低无效请求量与“先黑块再出图/近景黑面”的观感；3D 地理四叉树帧间缓存同时校验眼点、离地高度尺度与相机 forward/up，倾斜、旋转、缩放后立即重建可见叶集，避免复用旧视锥造成周边黑区 | `addTileLayer` |
| 矢量 | shp/kml/kmz/dwg/dxf 等 OGR 可打开格式**直读文件**→ 重投影 WGS84 → earcut 三角剖分；面填充/描边/线/点/图标 billboard/文字标注（字形图集，可复用系统 CJK 字体，零 APK 增量） | `addVectorLayer` / `VectorStyle` / `setFontPath` |
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
      onReady: (v: NativeMapView): void => {
        this.initMap(v);
      },
    })
      .width('100%').height('100%')
  }
}
```

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

其余 API（矢量/栅格/叠加层/拾取/定位标记/手势）与 Kotlin 侧逐一对应，参「API 概览与快速上手」；
生命周期无需手动 onPause/onResume：native EGL 线程随 XComponent 加载/卸载自动 attach/detach。

### 实验性瓦片/地形加载平滑调度（P0-2 exp1e）

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

### 实验性交互 LOD 迟滞与低分辨率地形后备（P0-2 exp1f）

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

### 实验性大倾角近景黑面修复（P0-2 exp1g）

> 实验状态：本轮对应构建 ID `v0.1.3-20261026-loadsmooth-exp1g`。用户在 3D、地形开启、倾角较大且距离较近时反馈屏幕下方近景影像呈黑色。初验在 Harmony 真机 `5KLBB25911200313` 通过；仍按实验提交管理，若近景清晰度/请求量不符合预期，可整体回退本提交。

- 根因：exp1f 后四叉树已经画出大倾角可见叶，但高清叶请求预算只有前 192 个；旧排序先按 z、再按屏幕中心经纬度距离，屏幕下方离相机最近但略偏离中心的叶子可能排在预算外，其 z-1/z-2 祖先也没有入队。静止时没有 pending/inflight，占位球面被持续绘制（真机日志约 `placeholder=16920~17202/s`，约 280 次占位/帧）。
- 近景优先：3D 可见叶排序改为按瓦片地形表面中心到相机的三维距离；距离近似相同时再保留“高级别优先、屏幕中心优先”的高空正视策略。距离和索引通过 scratch 数组每帧预计算，避免排序比较器重复扫描 DEM。
- 覆盖率兜底：高清叶仍只请求前 192 个近景瓦片，但底图最粗的 z-2 祖先请求覆盖全部可见叶；z-1 仍受近景预算约束。这样预算外瓦片至少由粗祖先拉伸覆盖，不再永久走深色 placeholder。
- VBO LRU：大倾角可见叶可达 700+，旧的 512 个地形网格缓存会在近景持续 LRU 抖动并重建；上限提高到 1024（24×24 地形 VBO 最坏约 24MiB）。
- 真机结果：复现“太白山 → 地形 → tilt=55° → 短放大到正常近景大倾角”后，连续帧和静止帧未见近景黑面，`placeholder=0`；热缓存地形 `182/182` 磁盘命中、`networkRequests=0`、`failures=0`。
- 边界：自动测试中持续放大到相机贴入/穿入山体时，画面仍可能因地形遮挡呈黑色，此时 `placeholder=0`，不是影像缺瓦；后续应单独增加“相机相对地形表面的最小安全高度/有效倾角钳制”。

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
  ⑪ **P0-2 exp1e：瓦片/地形加载期交互平滑实验（实验性）**：新增 180ms 交互窗口；所有瓦片层共享 4 张/1MiB 单帧上传预算；TileLoader 不再随相邻视口立即清空队列和 ready，pending/inflight 同 key 可升级代际，新增 ready key 去重；2D 中心优先、3D 交互/空闲分别只请求 96/192 块叶瓦片并保持祖先粗瓦先覆盖；quantized-mesh 解码移到 Provider worker，GL 线程按 2/4 块应用 DEM，地形网格改按局部 DEM signature + revision 精确失效并按 2/4 个预算重建；新增 TILEPIPE 与 HTTP 连接复用诊断。真机冷地形 182/182、失败 0、首块 58ms、总耗时 834ms，DEM 到达秒 frameMaxMs=14.59ms，连续拖动 frameMaxMs=23.58ms。本轮为实验提交，效果不佳可整笔 revert，详见“实验性瓦片/地形加载平滑调度”。
  ⑫ **P0-2 exp1f：交互 LOD 迟滞与低分辨率地形后备（实验性）**：3D 手势中使用最近非交互帧的稳定瓦片 LOD 做细化迟滞，放大时不追逐瞬态高清叶、缩小时允许父级粗图覆盖，避免自身高清纹理与祖先纹理短暂同时不可用时绘制深色占位；迟滞状态按图层 `maxLevel` 隔离，并在 surface 重建、纹理清空、图源切换和 release 时复位。地形网格预算不足时仍采样已有 DEM，只从 24×24 降为 8×8，不再生成零高程平面；新增 `placeholder/ancestor/leaf/fallbackMesh` 诊断。真机太白山地形开启双指放大/缩小及关闭地形仅影像三组缩放均记录 `placeholder=0`；冷启动全球首秒在根瓦到达前仍有占位，后续需通过低级别瓦预取/内置或底色过渡单独优化。详见“实验性交互 LOD 迟滞与低分辨率地形后备”。
  ⑬ **P0-2 exp1g：大倾角近景黑面修复（实验性）**：3D 可见叶改为按地形表面中心到相机的三维距离排序，保证屏幕下方最近区域优先获得高清叶；底图 z-2 粗祖先请求覆盖全部可见叶，预算外近景瓦片不再因“自身高清纹理和祖先纹理均缺失”而永久绘制深色占位；排序距离通过 scratch 数组每帧预计算，地形 VBO LRU 从 512 提到 1024，避免 700+ 可见叶时网格缓存反复淘汰重建。真机太白山正常近景大倾角视角下 `placeholder=0`、近景无黑面；相机穿入山体的过近机位另列地形安全高度任务。详见“实验性大倾角近景黑面修复”。
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
