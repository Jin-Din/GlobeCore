# globecore-desktop —— GlobeCore 引擎桌面宿主（Windows / Linux）

一套引擎源码的第四个宿主。口径与 `globecore-harmonyos/library` 完全一致：

```
globecore/src/main/cpp/          ← 引擎源（唯一真源，不复制不搬迁）
  ├── jni/            Android 宿主（GLSurfaceView + JNI）
  ├── (ohos/)         HarmonyOS 宿主（EglContext + XComponent + NAPI）
  └── (本目录)         桌面宿主（QOpenGLWidget + Qt Widgets）
```

引擎侧改动仅两处、均为「加分支」而非改写，移动端行为零影响：

| 文件 | 改动 |
|---|---|
| `util/Log.h` | 新增 `GLOBECORE_DESKTOP` 分支（stderr printf） |
| `bridge/{srs,gdal_info,layer_info,vector_io}.cpp` | JNI 导出段守卫由 `!__OHOS__` 改为 `!__OHOS__ && !GLOBECORE_DESKTOP` |

桌面直接调用 `gcbridge` C++ 核心，**不需要 JNI/NAPI 那种编解码薄封装层**。

## 地形 Provider

`MapWidget` 与 Android / Harmony 使用同一套地形 Provider 门面：

```cpp
widget->configureTerrainProvider(baseUrl, cacheDir, 15, 4, 3, headers);
QObject::connect(widget, &MapWidget::terrainProviderEvent, this, [](const QString &payload) {
    // @Jin payload 为单行 JSON，可在 GUI 线程直接解析 event / stats
});
widget->prefetchTerrainProviderMetadata();
widget->setTerrainProviderEnabled(true);
```

共享 C++ Provider 的 worker 线程只产生事件；`MapWidget` 通过 `Qt::QueuedConnection` 转投 GUI 线程后发出 `terrainProviderEvent(QString)`。

## 目录

```
globecore-desktop/
├── CMakeLists.txt          依赖定位 + GL 后端选择 + 引擎源引用（对齐鸿蒙脚本）
├── glshim/GLES2/gl2.h      仅 glew 后端用的 GLES2→桌面GL 兼容头
└── src/
    ├── host/MapWidget.*    GL 宿主：GL 生命周期/鼠标手势 → GlobeEngine（对应 GLSurfaceView）
    ├── host/RasterCache.h  全幅栅格 sidecar 缓存目录（按源路径 md5 出每源一 ASCII 子目录）
    └── app/
        ├── LayerPanel.*    图层停靠面板（瓦片/栅格/矢量 + 2D/3D）
        ├── MainWindow.*    QMainWindow + 菜单 + 状态栏 + 拾取属性弹窗
        └── main.cpp        表面格式(GLES2) / PROJ 数据路径 / CJK 字体探测
```

## GL 后端（关键决策）

引擎 `render/` 是纯 GLES2 指令 + 内嵌 GLSL ES 1.00 着色器（`attribute`/`varying`/`precision`）。

- **Windows**：必须 `GLOBECORE_DESKTOP_GL=gles2` + ANGLE（Qt6 不内置 ANGLE；`AA_UseOpenGLES` 让 Qt
  运行时加载 exe 旁的 `libEGL.dll`/`libGLESv2.dll`，来源见「Windows 构建」一节）。
  桌面 `opengl32` 不提供 `ARB_ES2_compatibility`，GLSL ES 着色器在原生 Windows GL 下**编译失败**，没有退路。
- **Linux**：`gles2`（Mesa `libGLESv2`+`libEGL`）或 `glew`（桌面 GL2.1 + `ARB_ES2_compatibility`，Mesa 支持）。
  龙芯/飞腾同理，GLES 走厂商 Mesa，不依赖任何 Windows 专有栈。

## 构建

### 0）前置工序（目前缺，必须先补）
`build-scripts/build-all.sh` 现仅 `--target android|ohos`（Linux 交叉编译）。桌面需要**与移动端同版本**的
GDAL/PROJ/curl/sqlite/kml 等静态库（引擎 `include/` 里的三方头按子目录形式包含且版本配套，混版本会 ABI 错配）。

扩展它支持 `--target windows|linux`，产物落到：
```
build-scripts/out/windows/<arch>/lib{gdal,proj,curl,sqlite3,z,expat,minizip,uriparser,kml*}.a|.lib
build-scripts/out/linux/<arch>/lib*.a      # arch: x86_64 / aarch64 / loongarch64
```
或走 CMake 模式 B（vcpkg/系统包），但需自担头布局（`gdal/`、`proj/` 子目录）与版本匹配。

### Windows（2026-10-10 实测通过，已整理为项目内自包含布局）

Qt5.15.2（非 Qt6：Qt6 在 Windows 移除了 ANGLE，`AA_UseOpenGLES` 失效拿不到 GLES2 上下文）。
所有构建产物/脚本已从 `D:\` 根收进项目内，路径由脚本自身位置推导，搬迁项目即用：

```
globecore-desktop/
├── build/          CMake 二进制目录（原 D:\gcd-build）
├── thirdp/         gdal/proj/curl/sqlite 头 junction → vcpkg-2023（原 D:\gcd-3rdp）
│                   ⚠ junction 指向绝对 vcpkg 路径，换机需重建（见 tools 下说明）
├── tools/
│   ├── gcd-paths.sh    集中定义路径：项目内派生 + 共享工具链绝对
│   ├── vs2019-env.sh   导入 vcvars64 环境
│   ├── gcd-rebuild.sh  全新 configure + build（改 CMakeLists/搬迁后跑这个）
│   ├── gcd-build.sh    增量 build
│   ├── gcd-deploy.sh   windeployqt + 补 ANGLE/GDAL 运行 DLL + 校验 proj.db
│   ├── gen_geotiff.py  纯 stdlib 造 GeoTIFF（栅格冒烟，免 gdal-bin/sudo）；`--size WxH [--projected]`
│   ├── big-raster-test.sh  超大栅格压测（采样峰值RSS+真实退出码+崩溃标记）；`TIF/WINDOW/LOG` 可覆盖
│   ├── linux-smoke.sh  WSLg 冒烟封装：底图+3D+矢量+栅格 + 存活/崩溃标记判定
│   ├── linux-label-test.sh  验证地图标注字体（FontAtlas 命中 CJK 文件）
│   ├── egltest.c       WSLg EGL/GLES2 上下文自检
│   ├── sample.geojson  矢量冒烟测试数据
│   ├── KML_Samples.kml / ningde.kmz  真实矢量样例（OGR KML/KMZ 通路）
│   └── logs/           构建/运行日志归档
└── (Qt/vcpkg/ANGLE 仍在 D:\ 根，跨项目共享，未搬)
```

一键构建 → 部署 → 冒烟：
```bash
bash tools/gcd-rebuild.sh                 # configure+build 到 build/
bash tools/gcd-deploy.sh                  # 产物变独立可执行
# 冒烟钩子（环境变量，见 main.cpp）：
GLOBECORE_SMOKE_BASEMAP=1 GLOBECORE_SMOKE_3D=1 \
GLOBECORE_SMOKE_VECTOR="$(pwd)/tools/sample.geojson" QT_FORCE_STDERR_LOGGING=1 \
  ./build/globecore-desktop.exe
```

CMake 变量（脚本已填好，手工调用时参考）：`-DGLOBECORE_DESKTOP_GL=gles2
-DGLOBECORE_ANGLE_DIR=D:/angle-msvc -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/msvc2019_64;D:/vcpkg-2023/installed/x64-windows"
-DGLOBECORE_3RDP_INC=<项目>/thirdp`。

已验证通路：XYZ 在线底图（2D）✓ · GDAL GeoJSON 矢量 ✓ · 3D 球面瓦片+深度 ✓ ·
MSVC 编译（`/FIwin_host_compat.h` 规避 wingdi `ABSOLUTE`/windef `near`/`far` 宏冲突）✓。

已知格式缺口：vcpkg GDAL 3.7.0 未编 optional OGR 驱动，`shp/geojson/json` 可读，
`kml/kmz`(需 LIBKML)/`gpx`/`gpkg` 暂缺 —— 属打包差异非代码缺陷，补驱动需重编 GDAL 或走 build-scripts。

ANGLE 就位（2026-10-09 实测，vcpkg 构建 angle 被 googlesource 网络阻断后的替代路线）：
- `D:\angle-msvc\include\{EGL,GLES2,KHR}` ← vcpkg `egl-registry`/`opengl-registry` 端口（纯头文件）
- `D:\angle-msvc\lib\{libEGL.lib,libGLESv2.lib}`、`D:\angle-msvc\bin\*.dll` ← Qt 5.15.2 msvc2019_64
  自带 ANGLE 预编译（Qt6 起不再捆绑）；运行时把 bin 下两个 DLL 放到 exe 旁

### Linux / WSLg（2026-10-10 实测通过）
本机在 **WSL2 Ubuntu-24.04 + WSLg** 下把 Linux 通路完整跑通（Mesa llvmpipe 提供 GLES2/EGL，
`GL_MAX_TEXTURE_SIZE=16384`），产物在**原生 ext4** 树 `/home/dtalp/globecore-linux/GlobeCore/globecore-desktop/build-linux`
（勿放 `/mnt/d`：drvfs 慢且软链不稳）。真实 麒麟/UOS 同构，只是走厂商真 GLES 驱动。

依赖（apt）：
```bash
sudo apt install cmake ninja-build qtbase5-dev libqt5opengl5-dev \
     libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
     libgdal-dev libproj-dev libcurl4-openssl-dev zlib1g-dev libsqlite3-dev
```
configure（系统包模式 B，走 `find_package`）：
```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j8
```

与 Windows 不同的三处关键处理（均已在 CMakeLists/代码内解决）：
1. **三方头影子 `linuxinc`**：引擎自带 `include/` 的 GDAL/PROJ/curl 头是移动端配置版（GDAL 3.7.0/PROJ 9.0），
   链系统新版（GDAL 3.8.4/PROJ 9.4）会 **C++ vtable 跨 minor 漂移 → readVectorFile 段错误**。CMake 在
   `build-linux/linuxinc` 下软链 `gdal→/usr/include/gdal`、`proj→/usr/include`、`curl→/usr/include/x86_64-linux-gnu/curl`，
   置于 `ENGINE_ROOT/include` 之前，令 `"gdal/…"`、`"proj/proj.h"`、`"curl/curl.h"` 命中新版；其余（stb/zlib/openssl）回落 bundle。
2. **proj.db**：`WIN32 AND EXISTS` 守卫 —— Linux **跳过拷贝**，走系统 `libproj` 内置的 `/usr/share/proj/proj.db`
   （与所链 libproj 严格同版本）。带 Android 那份低 minor 版会触发 `DATABASE.LAYOUT.VERSION.MINOR` 不匹配并让坐标变换崩。
3. **SQLite 目标名**：vcpkg 导出 `SQLite3::SQLite3`、CMake `FindSQLite3` 导出 `SQLite::SQLite3`，取存在者。

冒烟（免交互，`tools/` 下）：
```bash
# 造一张带地理范围的小 GeoTIFF（纯 stdlib，免装 gdal-bin/sudo）
python3 tools/gen_geotiff.py tools/sample.tif
# 2D 底图 + 3D 球面 + 矢量 + 栅格 端到端
DISPLAY=:0 QT_FORCE_STDERR_LOGGING=1 GLOBECORE_SMOKE_BASEMAP=1 GLOBECORE_SMOKE_3D=1 \
  GLOBECORE_SMOKE_VECTOR=<项目>/tools/sample.geojson \
  GLOBECORE_SMOKE_RASTER=<项目>/tools/sample.tif \
  timeout 12 ./build-linux/globecore-desktop
bash tools/linux-smoke.sh    # 封装上面 + 存活/崩溃标记判定（exit 124=存活正常）
```
已验证通路（真实数据 `D:\Android\MobileMap\示例数据`）：XYZ 在线底图 ✓ · 3D 球面贴瓦+深度 ✓ ·
GDAL/OGR 3.8.4 GeoJSON ✓ · **KML**（20 要素、三角化、VBO）✓ · **KMZ**（`/vsizip`+内层 `doc.kml`、KmlStyle 解析）✓ ·
**栅格 GeoTIFF 通路**（`GdalBootstrap`→`addRasterImageLayer`→`GetRasterEntry`(VRT)→`renderFullImage`(`RasterIO`)→纹理上传→`RasterImage takeover`）✓，实测细节：
- **双帽降采样确证**：地理版 5000×5000（25M px > 像素帽≈24M）→ 日志 `renderFullImage src=5000x5000 dst=4899x4899`（≈23.99M），随后 `RasterImageLoader ready`→上传渲染完成。边长帽=`GL_MAX_TEXTURE_SIZE`（WSLg llvmpipe 报 16384）。
- **投影栅格→VRT 重投影确证**（真实 `帽帽山.tif`）：`GetRasterEntry: VRT 就绪 src=24819x37202 srcProj=CGCS2000 / 3-degree Gauss-Kruger CM 111E … vrt=30341x34465` —— 引擎正确识别投影 CRS、取到经纬度四至并现场建 VRT。
- **超大栅格内存有界**（1.3GB `帽帽山.tif`，9.2 亿像素，无内部金字塔）：整幅直读峰值 RSS 仅 ~1.26GB 且不随文件增大失控、**无 OOM/段错误**（`timeout` 窗口内 `EXIT=124` 存活），读取在后台线程、GUI 全程可交互。双帽把最终贴图像素封顶在 ≈24M(RGBA≈96MB)。
- **吞吐特性 + sidecar 缓存**：无金字塔的 1.3GB 投影图，现场 VRT warp（亿级像素）+ 抽稀整幅读首开 >4 min。桌面宿主已启用引擎的**全幅位图 sidecar**（`renderFullImage` 命中免 GDAL 免锁免解码）：`addRasterImageLayer` 传「按源绝对路径 md5 出的每源一 ASCII 子目录」`<dataRoot>/raster/<md5>`（见 `src/host/RasterCache.h`），一源一目录避免同尺寸不同源互相覆盖（引擎 sidecar 名 `full_WxH.rgba` 不含源标识），二次打开同一图即秒级命中、并随源 mtime/size 自动失效。**实测**：地理版 5000×5000 冷启动 warp→回写 `full_4898x4899.rgba`(95,981,208B)，热启动日志 `renderFullImage sidecar 命中 4898x4899`（不再 warp）。
- **修复：全幅 sidecar 永久未命中（共享引擎 `RasterRenderer.cpp`）**：`dstW/dstH` 由 `sqrt` 使积恰等 `maxPixels` 后再 `lround` 逐维进位，可令 `dstW*dstH` 略超帽（4899²=24,000,201>24,000,000）；而 `TryLoadFullSidecar` 判据「`w*h>maxPixels` 即未命中」（本为跨设备帽安全）会把本设备刚回写的 sidecar 永久拒绝 → 抽稀到帽附近的栅格缓存全平台失效。修法：miss 路径逐维回退硬保证 `dstW*dstH ≤ maxPixels`（现落 4898×4899）。Android/鸿蒙同受益（其全幅栅格缓存此前对降采样图从不命中）。
- 生产仍建议交付前 `gdaladdo` 预建 overview 令首开也秒级。
- 复现工具：`tools/big-raster-test.sh`（`TIF=`/`WINDOW=`/`LOG=` 环境变量可覆盖，默认压 `帽帽山.tif`；采样峰值 RSS + 真实退出码 + 崩溃标记）；`tools/gen_geotiff.py` 支持 `--size WxH [--projected]`（`--projected`=EPSG:32650 米制，走投影→VRT 码路；地理度数版走抽稀码路，秒级生成免 gdal-bin/sudo）。

CJK 中文（WSLg 曾控件乱码，已修）：
- 根因是裸 WSL/精简 Linux **没装任何中文字体**（`fc-list :lang=zh` 空）→ Qt 控件成豆腐块。真实麒麟/UOS 通常自带 Noto/文泉驿，无需干预。
- `main.cpp::applyWidgetCjkFont()`：Linux 下在已装字体族里命中首个 CJK 族（Noto Sans CJK SC / Source Han Sans SC / WenQuanYi Micro Hei / Microsoft YaHei / SimHei）就 `QApplication::setFont`；Windows 分支直接跳过（系统默认即含中文）。
- `main.cpp::detectCjkFont()`：确定性**目录扫描**（随包 `fonts/` → `~/.local/share/fonts` → `/usr/share/fonts`/`/usr/local/share/fonts` 递归，按文件名关键字匹配）取一个含中文字形的字体文件喂引擎 `FontAtlas`（地图标注）。**不用 `fc-match`**（实测其 `:lang=zh` 会回落无中文的 DejaVu）。
- 本机即时修法（用户目录装字体，免 sudo）：拷 `msyh.ttc`/`simhei.ttf` 到 `~/.local/share/fonts` + `fc-cache -f`。
  产品化建议：给产物 `fonts/` 随包一个 OFL 中文字体（如 Noto Sans SC），代码已优先扫 exe 旁 `fonts/`，裸 Linux 也自足。

### 龙芯 loongarch64 交叉
```bash
cmake -S globecore-desktop -B build-loong -DCMAKE_TOOLCHAIN_FILE=<loongarch toolchain> \
  -DGLOBECORE_ARCH=loongarch64 -DGLOBECORE_DESKTOP_GL=gles2
```
注：`build-scripts/build-all.sh` 尚未产桌面静态库（见「0）前置工序」）；龙芯/飞腾走系统包时，
`linuxinc` 影子逻辑同理复用（把三方头链到厂商 SDK 的 `gdal/`、`proj/` 目录即可）。

## 运行时资源
- **proj.db**：见上「Linux」第 2 点 —— Windows 拷到产物旁 `proj/` 并经 `gcbridge::initProjDataPath()` 注册；Linux 走系统 `/usr/share/proj`。
- **CJK 字体**：见上「CJK 中文」—— 控件走 `QApplication::setFont`，地图标注走 `engine->setFontPath()`（桌面没有 `/system/fonts`）。
- **CA 根**：`main.cpp::detectCaBundle()` 探测（exe 旁 `cacert.pem` → `/etc/ssl/certs/ca-certificates.crt`(Debian/麒麟)
  → `/etc/pki/tls/certs/ca-bundle.crt`(RHEL/UOS/openEuler) → `/etc/ssl/ca-bundle.pem`(openSUSE)），命中则
  `HttpClient::setCaBundle()`。**Windows 命中不到→不设置**，沿用 Schannel 系统证书库；**Linux 给出真实 bundle**，
  杜绝引擎在未设 CA 时回落 Android `/system/etc/...` 假路径 → OpenSSL 校验失败 → 静默降级为不校验的安全隐患。

## 本骨架状态与后续
- **已编译并运行验证**：Windows（MSVC14.29/Qt5.15.2/ANGLE，vcpkg 三方）与 Linux（WSLg/Qt5.15/Mesa，系统包）
  两端均跑通 XYZ 底图、3D 球面、GDAL 矢量（含真实 KML/KMZ）、栅格 GeoTIFF；中文（控件 + 地图标注）两端正常。
- 未接的引擎能力（接口都在，加对应 Widgets 即可）：叠加层测量/轨迹（`updateOverlay*`）、
  要素高亮（`featureGeometry`）、属性写回（`gcbridge::updateFeatureAttributes`）、SQL 查询、
  级别可见性、样式编辑面板。

## TODO（按优先级）
1. 扩展 `build-scripts` 产出桌面三方库静态产物（`--target windows|linux`），把 vcpkg/系统包的
   版本漂移风险收敛为与移动端同源同版本（前置，稳打包链路）。
2. 全幅栅格移除：引擎缺 `removeRasterImageLayer`，当前面板降级为「隐藏」，需引擎补接口。
3. 拾取→属性弹窗接样式编辑/高亮叠加层，对齐移动端交互。
4. （可选）Windows GDAL 驱动对齐：vcpkg GDAL 未编 LIBKML 等 optional 驱动，`kml/kmz/gpx/gpkg`
   在 Windows 暂缺（Linux 系统 GDAL 齐全，实测可用）；补驱动需重编 GDAL 或走 build-scripts。
5. （产品化）随包 OFL 中文字体到产物 `fonts/`，使裸 Linux 无系统中文字体时标注也自足。

> 已完成的桌面化项（原 TODO）：CA 根桌面化（`detectCaBundle` + `HttpClient::setCaBundle`，Windows 走
> Schannel、Linux 给真实 bundle，杜绝引擎不校验降级）· MSVC POSIX 兼容层（`/FIwin_compat.h`）·
> 桌面 CJK 字体（控件 `applyWidgetCjkFont` + 标注 `detectCjkFont`）· Linux 三方头影子 `linuxinc`。
