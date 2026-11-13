# @Jin 本库改造记录详档

> 来源：本文件于 2026-11-11 由 README.md 的 @Jin 详细区段迁移整理；README 仅保留干条摘要，完整改造过程统一归档于此。
> 基线：上游 @zys/globecore 0.1.4（提交 7eb0e4a）。
> 维护规则：以后新增/增强/修复/实验的完整过程、真机数据、回退方式、未解决待办写入本文件并继续以 `@Jin` 标识；README 只同步干条摘要。

## 目录

- 一、新增能力（在线 GeoJSON、EPSG:4490 瓦片、地形 Provider 下沉与自动跟随、LOD 调试 API、峰顶标注、能力查询文档）
- 二、瓦片/地形加载性能与黑面治理实验（P0-2 exp1e–exp1h）
- 三、飞行相机与矢量地形贴地（drape 系列）
- 四、API 文档与工程约定
- 五、未解决待办与实验开关/回退索引
- 六、3D 低空手势改造（fix/3d-gesture-low-altitude，2026-11-11）：单位勘误、P1 低空参数化、P2 射线投影平移
- 七、图层初始化参数入参化 layer-init-options
- 八、3D 垂直视角高 zoom 黑面修复 T4（地平线椭球化+垂直眼点安全壳）（fix/3d-gesture-low-altitude，2026-11-11）：serviceZoomOffset/heightOffset 建层 options 一次性传入
- 九、低空周边黑块根治 T5/T8、缩放焦点地形锚点 T9 与捏合降单指窜动 T9-fix2（fix/3d-gesture-low-altitude，2026-11-12~13）：DEM 死锁兜底、侧视锥有界松弛、射线-地形迭代求交、native 触摸桥基准重建+grace
- 十、单图层 serviceZoomOffset 模糊核查 A/B 结论与标准化测试机位（2026-11-13，main）：新旧 LOD 路径同级实证、unsharp 实验回退、西安 z8/0° 测试机位
- 十一、天地图 3857/4490 服务差异与 4490 注记字小/模糊根治：geoSourceLevelBias 地理源选源层级偏置（2026-11-13，main）

## 六、3D 低空手势改造（fix/3d-gesture-low-altitude，2026-11-11）

> 背景：真机反馈 3D 模式倾角 0 时最大只能到约 17 级、带倾角（55°/-35°）大比例尺下不能继续缩放且平移困难。先分析后改造，全部为实验性、可回退，代码注释统一 @Jin。

### 0. 前置结论：手势单位口径勘误（零逻辑改动）

- 真机实测（2560×1600、DPR 2.38）：在超出 vp 视口（约 1076×672）的物理像素点 (1800,1000) 长按，2D 逆地理 marker 落点 (1790~1810,988~1010) 与触点逐像素重合，结合官方文档确认 `OH_NativeXComponent_TouchEvent.x/y` 单位为**物理像素 px**而非 vp。
- 修正 XComponentBridge/EglContext/napi 层把 px 误标为 vp 的注释与内部符号（kFling*VpPerSec→PxPerSec、xVp→xPx 等）；公开 API 无变化。MapGestures.ets 消费 ArkUI TouchEvent（真 vp），加注释标明两条通路差异。

### P1. 高度地板与地形净空参数化

- 旧约束：`Navigator.cpp` 编译期常量 `kMinCameraAltitudeMeters=38.2m`（相机离地地板）与 `kMinTerrainClearanceM=150m`（眼点相对视野最高地形的净空），山区低空被安全壳强制抬升，缩放无感。
- 改造：下沉为成员 `minCameraAltitudeMeters_`（钳 1~100000m，默认 38.2）、`terrainClearanceM_`（钳 0~10000m，默认 150），经 GlobeEngine→Harmony NAPI/ArkTS、Android JNI/Kotlin 双端暴露：
  - `setMinCameraAltitudeMeters(meters)` / `setTerrainClearanceMeters(meters)`；setter 后立即 `clampAltitude()` 生效。
- 真机数据（太白山、倾角 55°）：默认参数按钮缩放止于 displayLevel **z16**（另发现 demo `zoom()` 自带 500m 硬下限，已放宽到 5m 以暴露 SDK 真实地板）；切 5m 地板 + 0m 净空后同场景放大到 **z20**（瓦片仍 z18，图源 maxLevel=18 的数据天花板，之上为纹理拉伸，符合预期）。

### P2. 3D 射线投影平移

- 旧路径：`panByPixels` 3D 分支用正射口径 `mPerPx=2·alt·tan(fov/2)/视口高` 线性近似 + 屏幕基向量折算；低 alt（z20 时约 0.05m/px）且透视压缩下手感“拖不动、发飘”。
- 新路径（默认开）：手势 DOWN 调 `beginPanGesture(sx,sy)` 记录屏幕锚点的椭球射线交点；每帧把“锚点屏幕坐标+本帧位移”作为新屏幕点再次 `screenToGeo3DUnlocked` 求交，按两交点 ENU 差分平移中心并逐帧滚动锚点（支持长拖动）；射线落到地平线外失败时本帧自动回退线性近似、锚点保留。UP/CANCEL 调 `endPanGesture`；fling 不经会话走旧路径。
- 开关：`setRaycastPanEnabled(false)` 整体回退；Harmony 手势桥在 DOWN/UP 自动维护会话，Android 在 NativeMapView.onTouchEvent 按 action 维护。
- 真机数据（z20、倾角 55°，NCC 块匹配估计画面位移）：同 700px 手指拖动，旧线性路径画面仅响应 **16px**（NCC 0.92 几乎不动），射线路径响应 **≥280px**；300px 短拖响应 320px+，开关双向切换有效。

### 附带发现（未在本分支修复）

- 3D 长按拾取只与椭球求交（高程 0），overlay 点按 DEM 表面烘焙，倾角下 marker 与触点存在数百 px 屏幕偏差（2D 无此问题）；后续 P3 拟在拾取/缩放锚点增加 DEM 求交修正。
- 构建 ID：`v0.1.4-20261111-low-alt-gesture-p1p2`（Renderer.cpp GLOBECORE_BUILD_ID）。

## 七、图层初始化参数入参化 layer-init-options（2026-11-11）

> 背景：3D Tiles 的 `heightOffset` 与瓦片层的 `serviceZoomOffset` 引擎内部早已是逐层独立状态，但公开 API 只有“先建层、后补调 setter”一条路径，宿主首帧先按 0 渲染再跳变。方案存档：`docs/design/layer-init-options-proposal.md`。实验性、可回退，注释统一 @Jin。

### 1. 改造内容（双端共用 C++ 核心，Harmony 全链路已通，Android 待移植入口）

- C++ 核心：`TileLayerOptions` 新增 `int serviceZoomOffset = 0`，`GlobeEngine::addTileLayer` 建层即写入并沿用 `[-3,2]` 钳制；新增 `struct Tileset3DOptions { double heightOffset = 0.0; }` 与重载 `addTileset3D(url, const Tileset3DOptions*)`，旧单参签名保留并以 nullptr 委托，建层后立即 `setHeightOffset`（Renderer 下一帧 uniform 生效，无模型重建）。
- Harmony NAPI：新增 `nativeAddTileLayerWithLayerOptions(handle, cacheDir, url, maxLevel, overlay, options)` 与 `nativeAddTileset3DWithOptions(handle, url, options)`，options 为 JS 对象、字段全可选，缺省即旧默认值；旧 NAPI 函数全部保留。
- ArkTS：具名接口 `TileLayerInitOptions`/`Tileset3DInitOptions` 定义在 NativeLib.ets（ArkTS 禁止对象字面量作类型），NativeMapView 再导出保持 HAR 根出口不变；门面 `addTileLayerWithLayerOptions(...)`、`addTileset3DWithOptions(url, options)`。
- 语义：初始化值负责开箱即适配，运行时 setter（`setLayerZoomOffset`/`setTileset3DHeightOffset`）覆盖初值；默认 0/不传 options 时与改造前行为完全一致。
- Demo：四个建层点（4326/墨卡托各影像+注记）改用 options 一次性携带 `serviceZoomOffset`，删除建后 setter 重放块；qx-dyt 改 `addTileset3DWithOptions(url, {heightOffset:-20})`，步进面板仍走 setter。

### 2. 调试中发现并修复的空指针崩溃

- 首版真机启动即 SIGSEGV（SEGV_MAPERR @0x7c），符号化定位到 `GlobeEngine::addTileLayer`（GlobeEngine.cpp:259）：`layers_.push_back(std::move(layer))` 后 unique_ptr 已置空，新 LOGI 仍解引用 `layer->serviceZoomOffset`，0x7c 即该成员相对对象基址的偏移。
- 修复：move 前先把钳制结果落到本地 `appliedZoomOffset`，日志改读本地变量；`addTileset3D` 路径使用 move 前缓存的裸指针 `layerRaw`，无此问题。

### 3. 真机验证（MatePad HarmonyOS NEXT，BUILD_ID v0.1.5-20261111-layer-init-options-fix1）

- 默认档位切图源：hilog 两层均为 `addTileLayer ... zoomOffset=0`，且无 `setLayerZoomOffset` 重放日志。
- 调试坞影像/注记均置 -1 后切天地图 4490：两层建层日志 `crs=EPSG:4326 geo=1 zoomOffset=-1`，中文注记渲染正常；切 ArcGIS 后同态保留 `crs=EPSG:3857 zoomOffset=-1`。
- 恢复标准后天地图 3857/ArcGIS 建层日志回到 `zoomOffset=0`。
- 大雁塔（qx-dyt）建层日志 `addTileset3D ... initHeightOffset=-20.000`，面板初值即 -20m；步进 +5/复位经 setter 正常覆盖，无崩溃。

### 4. 回退方式

- 宿主不传 options 或恒传 0 即恢复旧行为；删除 NAPI/ArkTS 两个 WithOptions 接线与 C++ 重载即可完全回退，旧签名与 setter 全保留。

### 5. 遗留

- Android JNI/Kotlin 尚未提供 tileset3d 与 WithOptions 入口（C++ 核心已双端共用），列入待办；heightOffset 步进节流防抖待办不变。
## 八、3D 垂直视角高 zoom 黑面修复 T4（fix/3d-gesture-low-altitude，2026-11-12）

> 背景：真机反馈 3D 模式垂直视角（倾角≈0°）放大到 z15+ 后整屏无影像（纯黑），2D 同级正常；带倾角时部分场景也有周边黑块。全部为实验性、可回退，注释统一 @Jin。

### 1. 真机诊断数据（临时 DIAG3D/DIAGCULL 日志，验证后已删除）

- 黑面瞬间四叉树可见叶集 `leaves=0`（正常同场景应为 10+ 个 z18 叶），TILEPIPE `placeholder=0 leaf=0`，管线零请求零绘制。
- 相机状态：目标地形高 `tgtH≈2184m`、视口安全壳 `safeH≈2400m`、相机相对高度 `alt≈157m`。
- 根节点剔除原因：`DIAGCULL z0 horizon=0 rejectPlane=-1`——死在**地平线测试**，不是视锥平面。

### 2. 两个叠加根因

1. **垂直视角眼点不遵守视口安全壳**（`Navigator::camera3DUnlocked`）：tilt=0 分支眼点恒为 `目标点地形高 + altitude_`，高 zoom 时 altitude_ 仅几十~百余米；带倾角分支本有 `safety-target+clearance` 净空保护，垂直分支漏接，眼点钻进周边山脊安全壳之下。
2. **地平线判据被地形高程污染（决定性）**：T3g 把逐瓦片地形抬高量（nodeSurfaceMargin）同时用于视锥 AABB 与地平线采样。地平线测试 `dot(q,e)*|E| >= |q|²` 是“地球背面剔除”，只对球面点成立（Tessellator 一直用高 0 采样）；低空眼点半径小于邻近地形壳半径时，正前方瓦片也被判到地平线下，根节点直接剪枝 → 全黑。

### 3. 修复内容

- **T4-a 地平线采样回归椭球面**（`GeographicQuadtree.cpp`）：最近点新增 `nearestHorizon`（高 0）专供地平线判定；9 网格采样拆为地形壳点（只入视锥 AABB）与 `ph`（高 0，入地平线）；跨 180° 补点 `phW1/phW2`、极冠补点 `phN/phS` 同口径。地形壳对视锥的山脊近裁面保护完整保留。回退：高 0 采样改回 nodeSurfaceMargin 即恢复 T3g 行为。
- **T4-b 垂直眼点安全壳**（`Navigator.cpp`）：tilt=0 眼点高度改为 `max(目标高+altitude_, safeH+terrainClearanceM_)`；无地形（safeH=0）时与旧表达式逐位等价。回退：恢复单行旧写法（注释中已保留）。

### 4. 工程事故与重建（如实记录）

- 排查中误用 `git checkout -- GeographicQuadtree.cpp`，把该文件**未提交的 T3g/T3h 改动连同诊断代码一起回退**；.h 未受影响（ITileTerrainQuery、T3h 节流成员仍在）。
- 已依据会话内留存的完整 diff 逐块重建 .cpp：T3g 逐瓦片 `maxTerrainHeightForBounds+50m`、T3h 180ms 修订号节流、T3c 朝向 0.35°/位移 0.5%·40m 阈值，并在重建版上直接叠加 T4-a。ninja 编译通过，行为经真机复核。

### 5. 真机验证（BUILD_ID v0.1.5-20261112-fix-t4-horizon2，诊断已移除）

- 原黑面同场景（甘肃山区 z17/z20、倾角 0°）：`leaves=10 zRange=[18,18]`，TILEPIPE `placeholder=0 leaf≈1140`，影像完整无黑块；最低高度地板 38.2m 仍保持叶集。
- 高 zoom 单指平移：叶集连续（leaves 稳定 10）、目标高程平滑过渡，无黑面复发；2D 模式同级正常，切 2D/3D 无回归。
- 带倾角场景与本次同根（地平线剔除通路唯一），双指倾角手势需人工补充验证。

### 6. 回退方式

- T4-a：GeographicQuadtree.cpp 中高 0 地平线采样点（nearestHorizon/ph/phW/phN/phS）改回 nodeSurfaceMargin。
- T4-b：Navigator.cpp 删除 eyeHeightAboveEllipsoid 分支，恢复 `geographicToCartesian(..., terrainTargetHeight + altitude_, c.eye)`。

## 一、新增能力

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

### @Jin EPSG:4326/4490 地理切片栅格瓦片支持（feature/epsg-4490，P1 实验）

- @Jin 背景：天地图等服务的 img_c/cia_c 为 EPSG:4326 地理切片（根 2x1、纬度线性、TILEMATRIX 从 1 起、XYZ 行序），与引擎现用 Web 墨卡托（根 1x1、墨卡托反投影）不是同一套 TileMatrixSet；4490(CGCS2000) 与 4326(WGS84) 椭球差异亚米级，可视化按同一地理切片处理。方案与对标（Cesium/Mars3D）详见 docs/design/epsg-4490-raster-tile-proposal.md；改造思路、分层改动清单与重投影算法（相交地理瓦映射、等经纬拼贴、预乘 alpha、双线性采样、两级缓存、zlib 自研 PNG 编码）的完整实现详解见 docs/design/epsg-4490-raster-tile-implementation.md。
- @Jin 采用低风险“方案 A（图层级适配，可回退）”：引擎选瓦四叉树与 3D 几何完全不动；TileLoader 对地理源图层把每个引擎墨卡托瓦映射到相交的地理服务瓦（0 基服务级 s=z，cols=2^(s+1)/rows=2^s；早期曾用 s=z-1，因垂直 1/cos(lat) 拉伸已修正，见 2026-10-31 记录），原始地理瓦先读独立缓存 -geo-raw、未命中再下载，解码后等经纬拼贴并双线性重采样成该墨卡托瓦的 256x256 PNG，再复用既有结果缓存/去重/解码上传链路。二次浏览命中结果缓存，零重采样零网络。
- @Jin 新增 render/PngEncoder.{h,cpp}：仅依赖 zlib 的内存 RGBA 转 PNG 编码器（GDAL 无关），因此 GLOBECORE_LITE=ON 的鸿蒙包也可用；已登记进 Android 与 Harmony 两套 CMake 源清单。
- @Jin 公开 API（向后兼容，默认仍墨卡托）：C++ GlobeEngine::addTileLayer(..., const TileLayerOptions&)（TileLayerOptions{crs,tmsY,tileMatrixOffset}）；Harmony NativeMapView.addTileLayerWithOptions(...) 到 NAPI nativeAddTileLayerWithOptions。crs 传 EPSG:4326/EPSG:4490 启用，其余与旧版一致；Android/JNI 重载留作后续预埋（本次未改 Android 公开 API）。
- @Jin Demo：天地图影像/注记由 img_w/cia_w 改为 img_c/cia_c，切到天地图图源时以 crs=EPSG:4326、tmsY=false、tileMatrixOffset=1 加载，并使用独立缓存目录 map-cache/tiles/tianditu-c4326-{img,cia}（含 -geo-raw 原始瓦缓存），避免与墨卡托瓦串图。默认启动图源仍为 ArcGIS。
- @Jin Demo 图源菜单后扩展为三项（ArcGIS / 天地图4490 / 天地图3857）：4490 用 img_c/cia_c 走地理源重投影（独立缓存 tianditu-c4326-*）；3857 用 img_w/cia_w 走墨卡托直连（独立缓存 tianditu-w3857-*，实测 w 矩阵根为 2×2、等价标准 XYZ z1，{z} 直传无需偏移）。真机已分别验证 geo=1 与 geo=0 路径影像/中文注记均正常。
- @Jin 真机验证（2026-10-30，设备 5KLBB25911200313，LITE 包）：日志确认 geographic source on=1 maxGeoLevel0=17 tmsY=0 matrixOffset=1，影像/注记两层均出现 geo4490 reproject（首屏 online=1、随后缓存 online=0），无服务瓦获取失败/尺寸异常/映射超限；3D 全球中国轮廓、2D 放大到中国（地图 z4/瓦片 z7）城市与道路注记、以及太白山开地形飞行后的影像地形贴合均正确。
- @Jin 回退：不调用 addTileLayerWithOptions（或 Demo URL 改回 img_w/cia_w、缓存目录改回）即完全恢复旧墨卡托直连；地理源默认关闭。
- @Jin 2026-10-31 修复 4490 注记上下拉伸（两个叠加原因）：①主因——早期地理源按赤道经度 1:1 选服务级 s=z-1，但地理切片像元在地面是 cosφ(宽)×1(高) 的扁矩形，垂直方向源分辨率只有需求的 cosφ 倍（34°≈0.83），输出时垂直被整体放大约 1/cosφ≈1.20 倍，且与层级无关，表现为中文注记字整体拉高（影像因是自然纹理几乎无感）；改为 s=z，每个输出像元从源约 2(宽)×2cosφ(高) 像元重采样，恰好抵消源像元扁率，地面各向同性、与 3857 原生注记同比例。②次因——瓦内 256 行纬度原按角点线性排布，中纬度低层级另含 ±1%~3% 的瓦内不均匀；已改为逐行按真实墨卡托反投影取纬度（经度本就线性），瓦内误差归零，每瓦仅增 256 次 atan·sinh。结果缓存目录升级为 tianditu-c4326-v2-* 与旧结果隔离。
- @Jin 2026-10-31 遗留问题（暂不修，记录在案；详见 docs/design/epsg-4490-raster-tile-implementation.md §10）：天地图 4490 的中文**栅格注记 cia_c** 仍存在两个观感问题，二者同源、当前方案 A 无法同时消除——①**字偏小**：固定 s=z 时 c 套图在更细矩阵重新制图，注记更密更小（西安 z8 实测字高中位：cia_w≈12px，cia_c 经 s=z 重投影≈8.5px），全球小比例视角尤其明显；②**轻微上下拉伸**：c 瓦像元在地面是 cosφ(宽)×1(高) 的扁矩形，天地图把方块字栅格化在扁像元上，字本身在地面已被南北拉长 1/cosφ（33.7°≈1.20 倍），重投影/墨卡托保角只是如实呈现；合成实验证明无论服务级取 s=z / z-1 / z-2，结果高宽比恒约 1.20，改级差只能放大缩小字与改变模糊度，不能消除拉伸。曾实验每图层 serviceZoomOffset（对 cia_c 传 -1 取粗一级可把字调大到接近 cia_w，但纵向 1.2 倍拉伸随之更明显），经评估方案不妥，**已整体回退**为固定 s=z、接口恢复 8 参、缓存回到 c4326-v2。后续推荐方向：影像走 img_c、中文注记改用墨卡托 cia_w（每层本就支持独立方案），或注记改客户端矢量文字标注；如坚持 cia_c 只能在“字小但拉伸不明显（现状）”与“字大但拉长明显”间二选一。另有与之正交的待办：把全局瓦片 LOD 偏移改造成单图层参数（现 setTileLodOffset 为全局，影像/注记一起受影响）。影像层 img_c 无上述注记问题。

### @Jin 三维地形 CesiumTerrainProvider C++ 下沉（P2-1 阶段 C）

- @Jin quantized-mesh v1.0 的视口调度、下载缓存、重试去重与引擎注入全部下沉到 native（`globecore/src/main/cpp/terrain/CesiumTerrainProvider`），Android/Harmony/Qt 统一生命周期事件（元数据/开始/首块/进度/完成/错误/开关）；API 用法、缓存布局、调度策略与真机指标保留在 README.md「三维地形 C++ Provider（P2-1 阶段 C）」节，本文件不重复。

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

### @Jin 瓦片 LOD 诊断、运行时偏移与高分辨率密度修正

> 实验状态：默认仍使用标准屏幕误差 LOD；运行时偏移和“瓦片级=地图级”只在宿主显式调用时生效，可随时恢复，不持久化。

- 背景：真机 `MLR-AL10` 物理分辨率为 2560×1600，ArkUI 实测 `densityPixels=2.38`。Harmony Demo 此前固定传入 `density=1.0`，而 C++ 的瓦片细分判据按物理像素工作，导致高分辨率屏幕上标准 LOD 比预期多选约一级，注记文字视觉偏小。
- 密度修正：宿主应把真实 DPR 传入 `NativeMapView.density`；Demo 现使用 `display.getDefaultDisplaySync().densityPixels`。同一全球视角下，固定 `density=1.0` 时目标瓦片为 z4，传入 2.38 后标准目标瓦片为 z3，与此前人工确认字体较正常的“LOD-1”观测一致。
- 新增只读诊断：`GlobeEngine::currentTileLevel(maxLevel)` 返回当前图层目标瓦片 LOD；NAPI/ArkTS 门面提供 `tileZoomLevel(maxLevel)`，供宿主同屏显示“地图级别 / 瓦片级别”。3D 四叉树仍会按距离和 SSE 在目标 LOD 附近混入更粗叶。
- 新增运行时实验：`GlobeEngine::setTileLodOffset(-3..2)` 在标准屏幕误差结果上做整数偏移；负数使用更粗父瓦，可减少请求量并增大注记视觉字号，0 恢复标准。
- 新增同级实验：`GlobeEngine::setTileLodAlignDisplay(true)` 强制目标瓦片 LOD 等于仅由相机高度推导的 `displayLevel()`，用于测试“地图级=瓦片级”。该模式忽略视口高、FOV 与 DPR 的屏幕误差补偿，3D 全球大视口下会明显偏粗，仅建议作为对照，不建议直接作为默认策略。
- Harmony 门面：`NativeMapView.tileZoomLevel()`、`NativeMapView.setTileLodOffset()`、`NativeMapView.setTileLodAlignDisplay()`；NAPI 名称为 `nativeGetTileZoomLevel`、`nativeSetTileLodOffset`、`nativeSetTileLodAlignDisplay`。
- 缓存与限流：`TileLoader` 仍严格执行“磁盘缓存有效图像 → 未命中才联网”；HTTP 非 2xx（含天地图 429）不会写入缓存。若缓存中有历史瓦片，即使 token 被限流也应先显示缓存；卸载应用会删除沙箱缓存，冷启动后只能联网。



### @Jin 3D Tiles(b3dm) 倾斜摄影实验闭环（feature/3dtiles-b3dm，2026-11-11 合入主线）

- @Jin 新增独立 `globecore/src/main/cpp/tileset3d/` 模块：TilesetJson 解析 3D Tiles 1.0（transform/boundingVolume box·sphere·region/geometricError/refine/content.uri 兼容旧 url/asset.gltfUpAxis）；B3dmDecoder 解 b3dm header、Feature/Batch Table、RTC_CENTER 并提取内嵌 GLB；Tileset3DUrl 解析相对 URL；Tileset3DLoader 复用 HttpClient 下载。
- @Jin Provider 后台调度：外部 tileset 工作清单递归展开、SSE 屏幕误差 + 视锥 6 平面裁剪、量化眼点签名去重、锁内只遍历内存结构网络 IO 全部锁外、单帧最多 8 个 b3dm；ModelBuilder 完成 Z-up 换轴、RTC_CENTER 与 tile ECEF transform 的 CPU 转换，超 65535 索引自动展开非索引三角形兼容 GLES2；Renderer 独立 3D Tiles shader（Lambert + 深度测试，栅格之后矢量之前）。
- @Jin 真机阻断修复：① Tileset3DRuntimeNode::source 裸指针悬空（TilesetResult 析构/move 后 root 指针失效），改为 `unique_ptr<TilesetResult>` 堆托管，图 move 不搬动节点树；② b3dm 无 NORMAL 时按三角形叉积做面积加权自动平滑法向（genNormal=1），消除灰板；③ mars3d osgb2tiles3 产物 tile 局部帧本身是 Z-up ENU，套 Cesium 标准换轴反而 90° 立起，该类数据换轴取 identity；④ 纹理解码补齐 main 入口与 GLES2 NPOT 回退 LINEAR。
- @Jin 平台链路：`GlobeEngine::addTileset3D/setTileset3DVisible/setTileset3DHeightOffset` → Harmony NAPI → ArkTS NativeMapView；高度偏移按 layerId 逐层独立（atomic 存于 Tileset3DLayer，绘制期 uniform 平移、零重建）。qx-dyt（西安大雁塔）真机目视校准 heightOffset=-20m，仅 Demo 写死，SDK 默认 0。
- @Jin 边界：仅 b3dm 三角网；i3dm/pnts/cmpt、PBR、要素拾取、磁盘缓存、LRU 卸载、失败重试、ADD refine、高斯泼溅 GLB 不支持。方案细节见 `docs/design/3dtiles-b3dm-plan.md`。

### @Jin 地形父级回退与 3D Tiles 高度偏移（2026-11-11）

- @Jin 父级回退根因：210 地形服务（Mars3D 12.5m 镜像，quantized-mesh、EPSG:4326 地理网格 cols=2^(z+1)/rows=2^z、y 南起）layer.json 声称 z15 覆盖但瓦片实际全 404，z14 及以下正常；旧引擎无回退时整轮失败、地形不显示（真机 182 块全 404）。
- @Jin `CesiumTerrainProvider` 新增 fetchTileWithFallback：请求失败按地理网格逐级回退最近可用祖先（父索引=子 x/y 整除 2），按祖先真实 bounds 注入 tileSink；缺失负缓存 missingServiceTiles_ 上限 4096，resetDeliveredTiles 时清空；stats 增加 parentFallbacks。真机两轮验证 182/182、0 失败，第二轮父级全磁盘命中。

### @Jin 单图层 serviceZoomOffset（全局 LOD 偏移下沉，2026-11-11）

- @Jin 背景：原 `setTileLodOffset` 是 Navigator 级全局偏移，影像与注记一起受影响；本改造把 LOD 偏移下沉为每图层参数，与全局开关正交可叠加（最终级别 = 全局 LOD 结果 + 本层偏移）。
- @Jin C++：`TileLayer.serviceZoomOffset`（默认 0，钳制 [-3,2]）；`GlobeEngine::setLayerZoomOffset(index, offset)`；Renderer 2D 在 tileLevel() 结果上逐层叠加并钳制 [MIN_LEVEL, maxLevel]，下游可见范围/请求/祖先链/绘制统一使用调整后级别；3D 同步调整 targetLevel、四叉树细化帽与 SSE 因子（ldexp(1,-offset)）；交互 LOD 迟滞记忆增加偏移维度（lastIdleGlobeTileZoomOffset_），surface/纹理/图层 4 处重置点同步清空。
- @Jin Harmony 全链路：NAPI nativeSetLayerZoomOffset → NativeLib → `NativeMapView.setLayerZoomOffset(index, offset)`；index.d.ts 同步声明。默认 0 零行为变化，不持久化，切图源重建图层后由宿主重放。真机验证（BUILD_ID v0.1.3-20261111-per-layer-lod1）：注记层 -2 时同视角影像请求 z6、注记请求 z4，两层独立。
- @Jin 与 4490 回退实验的区别：2026-10-31 在 EPSG:4490 重采样路径内实验的同名参数已回退；本次是渲染选瓦层的通用逐层偏移，对墨卡托/地理图源均生效，但不能消除 cia_c 固有的 1.2 倍上下拉伸。

### @Jin SDK 能力查询与快捷定位峰顶标注

- @Jin 新增 `docs/sdk-capability-queries.md`，专门记录“SDK 是否支持某能力”的查询结论、代码依据、边界与建议 API；2026-10-29 已记录“地形夸张 / 垂直缩放”当前仅有 C++ 内部固定系数 `1.0`，尚未向 Harmony/Android/Qt 公开配置接口。后续每次回答类似能力查询后都要追加到该文档。
- @Jin HarmonyOS NEXT Demo 的快捷定位将“相机观察中心”和“真实山顶点”分离：太白山拔仙台、华山南峰、珠峰各自携带独立峰顶经纬度；初始全球视角不显示标注，只有点击快捷定位飞行时才显示当前目的地的 `poi.png` 山顶图标和文字，避免全球视角堆标签。
- @Jin 点击快捷定位时自动切到 3D 并开启地形；峰顶符号复用 Demo `rawfile/poi.png`，通过现有 overlay billboard + DEM draped 通路贴地；3D 文字锚点仅对 overlay 标注按同点 DEM 采高并上抬，普通文件矢量标注继续保持海拔 0 的旧行为，控制回归范围。快捷定位展开 UI 改为按钮右侧横向排列，减少纵向遮挡。
- @Jin Demo 初始化时显式调用 `setFontPath('/system/fonts/HarmonyOS_Sans_SC.ttf')` 加载鸿蒙简体中文字体；峰顶文字在屏幕空间上移到 POI 图标上方并放大到约 28dp，白字黑描边；2026-10-29 已在 HarmonyOS 平板人工确认太白山、华山、珠峰三个 POI 文字均可见；如真机仍觉不够醒目，可优先调整 Demo `VectorStyle` 的 `labelSize` 与 `Renderer.cpp` 中 quick-summit 的屏幕偏移/DEM 上方 120m 抬升。
- @Jin 本次构建核对 ID：`v0.1.3-20261029-local-quick-summit-label04`，真机日志已确认该 ID 生效；回退时移除 Demo 峰顶 POI overlay 调用和横向展开 UI，并恢复 `drawVectorLabels3D()` 中 overlay label 的固定 `alt=0` 锚点即可。

## 二、瓦片/地形加载性能与黑面治理实验（P0-2 exp1e–exp1h）

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

## 三、飞行相机与矢量地形贴地（drape 系列）

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

## 四、API 文档与工程约定

### @Jin API 帮助文档（Android + HarmonyOS NEXT）

- @Jin 新增 `docs/api/android-api-guide.html`，面向 Android/Kotlin/Java 宿主开发人员，分类整理 `NativeMapView`、`Camera`、`Position`、`VectorStyle`、`VectorExtent`、`FeatureGeometry`、`NativeSrs`、`NativeVector`、`NativeLayerInfo` 等公开 API，补充参数/返回值、生命周期、XYZ 瓦片、三维地形 Provider、动态 overlay、拾取反算、坐标转换、GDAL/OGR 文件 IO 与可复制示例。
- @Jin 维护约定：以后新增、修改、废弃或删除 Android 公开 API，必须在同一次定稿改动中同步更新该 HTML 文档；实验性 API 必须明确标注“实验性/可回退”。
- @Jin 新增 `docs/api/harmony-api-guide.html`（main 0.1.5.0-dev）：面向 HarmonyOS NEXT/ArkTS 宿主开发人员与 AI 编程助手，覆盖分发包内全部公开 API（NativeMapView、NativeSrs、NativeNet、NativeVector、NativeLayerInfo、CesiumTerrainProvider 及全部值对象），每个 API 按「签名→参数表→可运行示例」三段式组织，含 @Jin 改造标注、地形事件 JSON、业务示例与排错指南；分发包 .so 与门面快照位于仓库外 `dist/globecore-harmony-next-*`（二进制不入仓）。
- @Jin 维护约定补充：Harmony 侧 NAPI/ArkTS 公开 API 变更时同次定稿更新 harmony-api-guide.html，两端保持同步。

### @Jin Harmony 分发包命名与 LITE 约定（2026-11-11）

- 分发包（目录与 zip）文件名强制带构建形态：lite 构建（GLOBECORE_LITE=ON）为 `globecore-harmony-next-<version>-lite`，全量构建为 `globecore-harmony-next-<version>-full`；包内 README-integration.md 须声明形态与能力边界。
- 打包需求未明确 lite/full 时，必须先向使用者确认，不得自行选择；完整规则见仓库根 `AGENTS.md` 第 3 节。
- LITE 边界：在线瓦片（含 EPSG:4490）、三维地形、GeoJSON（本地/在线）、叠加层、拾取可用；GDAL/PROJ 系能力（shp/kml/tif 直读、PROJ 转换、NativeVector 文件 IO、NativeLayerInfo）为桩。

## 五、未解决待办与实验开关/回退索引

### 未解决待办

- @Jin 陕西省级 Polygon/MultiPolygon「大面」在 3D 地形下仍可能被地形遮挡（不限于 z4/z5/z6），与已修复的本地纯 MultiLineString 省界线不是同一条路径；详见三、drape36e 节末。
- @Jin 天地图 4490 栅格注记 cia_c 存在「字偏小」与「轻微上下拉伸」两个观感问题；每图层 `serviceZoomOffset` 方案经评估会加剧拉伸，已整体回退（固定 s=z、接口恢复 8 参、缓存回到 c4326-v2）；推荐方向：影像走 img_c、注记改用墨卡托 cia_w 或客户端矢量文字。详见一、EPSG 节 2026-10-31 记录。
- @Jin ~~全局 LOD 偏移改造为单图层参数~~ 已完成（2026-11-11）：`setLayerZoomOffset(index, offset)` 全链路落地，详见一、单图层 serviceZoomOffset 节。
- @Jin 地形 Provider 选块仍为中心 7×7 固定窗口，未升级为按 tilt/heading/FOV 的非对称视锥选块；大倾角极近景边缘可能缺 DEM。详见一、自动跟随节「已知边界」。
- @Jin 3D Tiles(b3dm) 已合入主线（2026-11-11，仍按实验能力管理）：纹理已通、朝向已修正、heightOffset=-20m 校准贴地；高斯泼溅 GLB、PBR、拾取、磁盘缓存为后续待办。
- @Jin 新增待办：heightOffset 步进按钮快速连点可能一次跳多档，需节流防抖；政务版陕西底图接入（服务地址与 xyz 规则待核实）。
- @Jin ~~3D 拾取/缩放锚点只与椭球求交（遗留 P3）~~ 已完成（2026-11-12 T9）：9×9 地形锚点网格 + 射线-地形迭代求交，双指缩放焦点漂移与长按拾取偏差一并修复，详见第九节。
- @Jin 新增待办（2026-11-13 能力核查）：**要素属性内存化回取（map.click 闭环）**。现状：C++ 拾取 `pickVector/pickVector3D` 已完备（GPU PickID+CPU 回退、3D 贴地口径、两阶段优先级），但命中仅得 layerIndex+fid；GeoJSON properties 在解析期被丢弃（仅留 labelField 单字段），`NativeVector.getFeatureAttributes` 按源文件路径走 GDAL 重读，LITE 桩实现不可用、overlay 层无源文件。计划：① C++ `VectorFeatureData/VectorLayer` 增加 fid→properties JSON 内存表（默认仅存白名单字段或设总字节上限，防省级大层内存膨胀），GDAL reader 与 `lite_geojson_reader.cpp` 双通路填充；② 新增 `GlobeEngine::featureAttributes(layerIndex,fid, outJson)`（层越界/fid 未命中返回 false），NAPI `nativeFeatureAttributes` + ArkTS `NativeMapView.featureAttributes()`、Android JNI/Kotlin 双端导出；③ 补齐 Harmony 缺失的 `featureRenderColor` NAPI 导出对齐 Android；④ 同步更新 docs/api 双平台文档；实验性可回退（不调新 API 时零行为变化，回退即删属性表与接线）。

### 实验开关 / 回退方式索引

| 能力 | 开关或回退方式 | 详档位置 |
|---|---|---|
| 在线 GeoJSON URL | `RemoteVectorCache::kExperimentalRemoteGeoJsonSource = false` | 一、1–2 |
| EPSG:4326/4490 地理瓦片 | 不调用 `addTileLayerWithOptions`（URL 改回 img_w/cia_w）即恢复墨卡托直连 | 一、3 |
| 地形相机自动跟随 | 宿主移除 `setTerrainProviderAutoFollow(true)`，回手动刷新 | 一、5 |
| 瓦片 LOD 偏移/同级对齐 | 运行时 API 不持久化，传 `0` / `false` 即恢复标准 LOD | 一、6 |
| exp1e 平滑调度 | 整体 `git revert` 实验提交；`kExperimentalAdaptiveUploadBudget=false` 仅部分回退 | 二 |
| exp1f LOD 迟滞/地形后备 | 整体 `git revert` 实验提交 | 二 |
| exp1g 近景黑面 | 整体 `git revert` 实验提交 | 二 |
| exp1h 安全剔除/安全高度 | 整体 `git revert` 实验提交 | 二 |
| 本地纯线 DEM 贴地 | `Renderer::kExperimentalLocalVectorLineTerrainDrape = false` | 三、drape36e |
| 叠加层 DEM 贴地/深度豁免 | 回退 Renderer 中 overlay 分支（文件矢量仍走原深度测试） | 三、flyTo/overlay 节 |
| 单图层 serviceZoomOffset | 移除 TileLayer.serviceZoomOffset 字段与 setLayerZoomOffset 调用链（默认 0 即原行为） | 一、单图层 serviceZoomOffset |
| 地形父级回退 | 删除 CesiumTerrainProvider fetchTileWithFallback 与 missingServiceTiles_ 负缓存 | 一、地形父级回退 |
| 3D Tiles(b3dm) | CMakeLists 移除 tileset3d/ 源清单及 GlobeEngine/Renderer 相关代码 | 一、3D Tiles 节 + docs/design/3dtiles-b3dm-plan.md |
| P1 高度地板/地形净空 | 运行时参数不持久化，不调用 setter 即默认 38.2m/150m 旧行为；调试坞选“默认”档恢复 | 六、P1 |
| T4-a 地平线椭球化 | GeographicQuadtree.cpp 高 0 采样点改回 nodeSurfaceMargin 即恢复 T3g 行为 | 八、3 |
| T4-b 垂直眼点安全壳 | Navigator.cpp 删除 eyeHeightAboveEllipsoid 分支，恢复地形高+altitude 单行旧写法 | 八、3 |
| T5 无 DEM bbox 安全高兜底 | 删除 Renderer::maxTerrainHeightForBounds 中 `if (maxHeight <= 0.0 && !terrainCache_.empty())` 块 | 九、2 |
| T8 侧视锥有界松弛 | GeographicQuadtree.cpp 阈值改回 `if (v < 0.0)`（删 kSideFrustumCullSlackM） | 九、3 |
| T9 地形锚点迭代求交 | `setTerrainAnchorEnabled(false)` 回退纯椭球；彻底移除需删 TerrainAnchorGrid 全套与 Renderer 发布块 | 九、4 |
| P2 射线投影平移 | `setRaycastPanEnabled(false)` 回退 mPerPx 线性近似；默认开，射线失败自动单帧回退 | 六、P2 |

## 九、低空周边黑块根治 T5/T8、缩放焦点地形锚点 T9 与捏合降单指窜动 T9-fix2（fix/3d-gesture-low-altitude，2026-11-12~13）

> 背景：T4 后真机仍反馈 3D 模式 z15+（尤其垂直视角、山区）屏幕周边影像不出、一片黑，且局部地形需手动刷新才加载；另有带倾角双指缩放焦点漂移（展开下移/收缩上移、不绕双指中心）。全部为实验性、可回退，注释统一 @Jin，构建号 v0.1.5-20261112-t9-anchor。

### 1. 诊断方法与关键数据（诊断代码定稿前已全部移除）

- 四叉树细分埋点（DIAGCULL2/DIAGREJ/DIAGLEAF）：输出每帧 leaves、6 平面剔除数、被剔节点 z/x/y 与平面有向距离 v、叶节点经纬范围；渲染端埋点（DIAGBLK）：leaves/tex/ancestor/none、复用缓存标志、眼高与倾角。
- 垂直视角（倾角 0°）z18、眼高约 150m（太白山）：修复前 leaves 仅 **8**，侧平面剔除 27~39 个高级别节点，被剔节点 v 多在 -5~-13m（临界误杀），地图区空洞 **50%~87%**；地形 28/28 已全部加载完成，排除纹理/网络/缓存原因。
- 判别实验：临时禁用 4 个侧视锥平面后画面铺满但 leaves 暴增至 657（超 kMaxTiles=640 预算），证明侧锥误杀是叶集不足主因，但全面禁用不可作正式方案。

### 2. 根因一：DEM 死锁环（T5 修复）

- `Renderer::maxTerrainHeightForBounds` 对 bbox 无已缓存 DEM 相交时返回 0，视锥 AABB 只剩 50m 逐瓦片余量；太白山真实山脊相对高差上千米，侧视锥平面把环周瓦片整支误剔。
- 地形请求又由可见叶集驱动：被剔瓦片永远拿不到 DEM → AABB 永远低矮 → 永远被剔，形成死锁，表现为必须手动“刷新地形”才出图。
- 修复（Renderer.cpp，实验/可回退）：无 DEM 相交且 terrainCache_ 非空时，兜底取 `viewportTerrainSafetyHeight(nav_.camera())`（含 1.8× 视口半径与 +50m 余量）；无地形图层时安全高返回 0，与旧行为逐位等价。回退：删除该 if 块。

### 3. 根因二：保守剔除与瓦片尺度失配（T8 修复）

- 眼高约 150m、fovY 45° 时视锥地面覆盖仅约 200m×124m，而单个 z18 瓦片跨度约 126~153m；p-vertex 保守 AABB 测试把角点稍出视锥的整瓦剔除，SSE 又强制细化到 z18、父级不留兜底，被剔区域彻底无几何。
- 修复（GeographicQuadtree.cpp，实验/可回退）：4 个侧视锥平面（L/R/B/T）剔除阈值由 v<0 放宽为 v<-300m（`kSideFrustumCullSlackM`，约 2 个 z18 瓦片跨度，等价多保留一环邻接瓦片）；near/far 平面保持严格，不削弱穿山保护。
- 真机数据：垂直 z18 leaves 8→**85**、地图区空洞 50%~87%→**0%~13%**（残余为侧栏 UI 深色干扰）；倾角 55° leaves 120→205，全部有纹理；88km 高空 leaves 42~48 不膨胀（300m 相对公里级视锥可忽略）；连续平移 leaves 33~89、none=0。回退：阈值改回 0。

### 4. 根因三：缩放锚点只交椭球不含地形（T9 修复）
- `Navigator::screenToGeo3DUnlocked` 的射线只与椭球面（海拔 0）求交；山区带倾角时射线穿过前景山脊后落在山后远处椭球点，`zoomBy` 按此错误锚点逐帧滑动中心 → 双指展开视角下移、收缩上移、不绕双指中心；小级别眼高数千公里可忽略，故仅大比例尺复现。
- 修复（Navigator.h/cpp + Renderer.cpp，实验/可回退）：
  1. Renderer 在 GL 线程按当前视口 footprint（与安全高同口径、1.8× 余量）采 9×9 地形高度网格（`Navigator::TerrainAnchorGrid`），经独立 mutex 发布；terrainCache_ 仍只允许 GL 线程访问，手势线程只读快照、无数据竞争。
  2. Navigator 新增双线性插值 `sampleAnchorHeightMeters`；`screenToGeo3DUnlocked` 以椭球交点为初值，逐次与“当前经纬点地形高处切平面”求交并更新经纬，最多 3 次、0.5m 收敛；网格外/无 DEM 返回 0，立即退出并与旧行为逐位等价。
  3. 开关 `setTerrainAnchorEnabled(bool)`（默认开）；关闭即恢复纯椭球求交。
- 影响面：该函数同时服务长按逆地理拾取与 P2 射线平移，修正后落点同样贴合地表（正向收益）；公开 API 无签名破坏（仅新增接口）。真机人工验证：带倾角/垂直双指缩放、平地/山区、长按拾取全部通过。

### 5. 根因四：native 触摸桥捏合降单指基准陈旧（T9-fix2，2026-11-13 定稿，构建号 v0.1.5-20261113-gesture-final）

- 现象：T9 锚点修复后真机仍反馈带倾角连续双指收缩，随机在第 3~35 下出现整视野向手指漂移方向（多为右下）窜动一下，zoom 本身正常。
- 关键通路勘误：SURFACE 型 XComponent 的真机触摸**只经 native `XComponentBridge::handleTouch`（物理像素 px）投递，ArkTS `MapGestures.ets` 仅为不触发的 .onTouch 兜底**（见 XComponentBridge.cpp 顶部注释）。早期一轮加在 MapGestures.ets 的 grace/fling 门控对真机无效，结论以 native 桥为准，ets 侧同类加固保留作兜底通路防御。
- 诊断方法（DIAGT9 统一毫秒时间线，定稿已移除）：在 Navigator 的 zoom/pan/tilt/地形高度四处埋点。3.15 万事件统计：5554 次 zoom 中锚点钳制 0 次、异常 factor 0 次、锚点网格连续无跳变；而漏网 pan 共 17 笔（40~123px），**全部紧跟 zoom 后 1~11ms 且孤立出现**，方向与手指漂移一致；另 18 笔 500~800px 大 pan 被 Navigator 既有 200px 硬防线拦截。
- 根因：双指捏合期间单指平移基准 `gLastX_/gLastY_` 不更新，停在捏合前位置；抬一指降为单指时 UP 分支只置 `gPinching_=false`，未重建基准；随后首个单指 MOVE 以“当前位置−捏合前陈旧位置”计算增量，产生横跨整个捏合过程的大平移。位移大小随机，故表现为间歇性窜动；fling 门控（gHadMulti_）原本就有效，问题不在惯性。
- 修复（XComponentBridge.h/.cpp，实验/可回退，注释 @Jin）：
  1. 新增 `gLastNumPoints_` 跟踪上一事件手指数，MOVE 中直接识别双指→单指转换（兼容 UP 晚于 MOVE 的机型时序），转换首帧以当前触点重建基准、本帧不平移；
  2. UP 降单指（n==1）时以**剩余手指** `touchPoints[0]`（非 ev.x/ev.y 那根抬起的手指）重建基准；
  3. 随后 6 帧 grace（约 100ms，`kPostPinchGraceFrames`）内单步超 120px（`kPostPinchGraceStepPx`，物理像素）的残余滑移直接丢弃，基准钉到当前点，下一帧恢复；
  4. 新 DOWN 复位全部状态。
- 真机验证（t9fix2 日志 5279 zoom/114 pan）：58 次双指→单指转换全部被基准重建捕获；>200px 被拒 pan 18→**0**、>35px 漏网 pan 17→**0**；正常单指拖动 114 笔全部保留。人工连续收缩 35+ 次无窜动。

### 6. 回退索引（新增）

- T5：删除 Renderer::maxTerrainHeightForBounds 中 `if (maxHeight <= 0.0 && !terrainCache_.empty())` 兜底块。
- T8：GeographicQuadtree.cpp 侧平面阈值改回 `if (v < 0.0)`（删除 kSideFrustumCullSlackM/cullThreshold）。
- T9：`setTerrainAnchorEnabled(false)` 整体回退；彻底移除需删除 TerrainAnchorGrid 全套与 Renderer 网格发布块。
- T9-fix2：删除 XComponentBridge 中 `gLastNumPoints_`/`gPostPinchGraceFrames_` 成员、MOVE 转换重建块与 grace 拦截块、UP 的 n==1 基准重建，即恢复“降单指沿用陈旧 gLastX_/gLastY_”的旧行为（不建议）。

## 十、单图层 serviceZoomOffset 模糊核查 A/B 结论与标准化测试机位（main，2026-11-13）

### 10.1 问题与结论

- 现象：注记层设 `serviceZoomOffset=-1` 后，主观观感比"全局 LOD 偏移改造为单图层参数之前"模糊，怀疑 LOD 下沉改坏取瓦/UV。
- 真机 A/B（天地图 3857、同机位、同注记层 idx=3、采样器强制统一为纯 GL_LINEAR），在 Renderer 2D/3D 渲染入口加临时级别诊断日志（定稿已删）：

  | 档位 | svcOff | nav_.tileLevel() | 注记层最终 targetLevel |
  |---|---|---|---|
  | 全局标准 | 0 | 14 | 14 |
  | 单图层注记 -1（新路径） | -1 | 14 | **13** |
  | 全局 -1（旧 Navigator.tileLodBias 路径） | 0 | 13（bias 已内置） | **13** |

- 结论：两条 -1 路径下注记层**请求的瓦片级别完全一致**，单图层改造（Renderer 逐层叠加 offset + 钳制、SSE 同步 `ldexp(1,-offset)`、迟滞 LOD 按 offset 隔离）数学上与旧全局路径等价，**无取瓦/UV bug**。
- 观感差异真因：① 旧全局 -1 对影像层与注记层同时降级，底图/注记粗细协调；新单图层 -1 只降注记，被放大的软笔画注记贴在高锐影像上，反差衬托显"糊"；② 同期实验的 unsharp mask 锐化着色器改变了原版双线性观感，已按评估结论**整体回退**（Renderer.cpp/.h 恢复 HEAD，BUILD_ID 恢复 gesture-final）。
- 保留能力：单图层 `serviceZoomOffset`（含建层 options 与运行时 setLayerZoomOffset）、旧全局 `setTileLodOffset` 接口均保留不变；默认 0 行为零变化。

### 10.2 标准化测试机位（长期有效）

- @Jin 约定：LOD/清晰度类 A/B 一律使用固定机位——西安（lat 34.344801, lon 108.934987）、地图 z8、倾角 0°、航向 0°，高度 110000m（真机 DPR 2.38 校准：220000m→z7，110000m→z8）。
- demo 调试坞「LOD 调试（单图层）」面板内新增按钮「测试机位·西安 z8/0°」，一键 `flyToCamera(new Camera(34.344801, 108.934987, TEST_XIAN_ALT_M=110000, 0, 0))`；常量 `TEST_XIAN_ALT_M` 集中在 Index.ets 顶部，换机/换 DPR 后只改这一处再按 HUD 校准。

### 10.3 其他定稿修复

- @Jin NativeMapView.ets 删除重复的 `export type { TileLayerInitOptions, Tileset3DInitOptions }`（重复导出阻断 ArkTS 编译），保留单处带 @Jin 说明的导出。
- 临时 A/B 入口（globalLodOffset state、selectGlobalLod、[临时AB] 按钮组）与 ABDBG 诊断日志、unsharp/bicubic 锐化全套着色器与程序均已移除/回退，工作区无实验残留。

## 十一、天地图 3857/4490 服务差异与 4490 注记字小/模糊根治（main，2026-11-13）

### 11.1 两套服务的切片矩阵差异（HTTP 实证）

- 天地图 WMTS 两套 TileMatrixSet 同属 256px 瓦片，但根矩阵结构不同（真机请求实测）：
  - **w（EPSG:3857 墨卡托，img_w/cia_w）**：TILEMATRIX=1 为 2×2（标准 XYZ z1），TILEMATRIX=0 为空 PNG；瓦内地面各向同性，引擎可墨卡托直连、1:1 采样。
  - **c（EPSG:4326/4490 地理，img_c/cia_c）**：TILEMATRIX=1 为 **2×1**（根 2 列 1 行），第 s 零基级列数=2^(s+1)、行数=2^s；像元地面足迹为“水平 cosφ、垂直 1”的**扁矩形**，水平与垂直地面分辨率不等。
- 引擎内部统一按墨卡托球体选瓦，c 服务瓦片必须在 TileLoader 内重投影（`generateGeographicTile`）：按墨卡托瓦四至求相交地理服务瓦→拼贴→逐行墨卡托反投影双线性重采样成 256² 墨卡托 PNG。

### 11.2 4490 标准档字小的根因（几何推导）

- v2 重投影选源层级 **s=z**（为修正 v1 的垂直 1/cosφ 拉高）：一个墨卡托瓦在水平方向恰好覆盖 2 块 c 服务瓦（512 源像素→256 输出，**2:1 缩小**），垂直约 2cosφ 块；各向同性达标，但服务瓦内所有内容（含注记字形）被整体缩到约一半，故 4490 注记比 3857 直连（1:1）视觉字号小约一半。
- 旧 v1 选 s=z-1：水平 1:1（字号与 3857 一致），但 c 像元垂直地面跨度为水平的 1/cosφ 倍，输出垂直被放大约 1/cosφ（西安 34°≈1.2 倍），即早期“轻微上下拉伸”的来源。两套方案是 c 服务扁像元下**不可同时消除的固有折中**。

### 11.3 serviceZoomOffset=-1 为什么模糊（两服务皆然）

- serviceZoomOffset 作用于**引擎墨卡托选瓦级**：-1 让引擎请求粗一级墨卡托瓦并由 GPU 双线性放大 2 倍显示——这是栅格放大的固有软化，与 LOD 下沉改造无关（第十节 A/B 已实证新旧路径取瓦级别完全一致）。
- 4490 下是**双重重采样**：先 geo→墨卡托双线性缩小，再 GPU 双线性放大，模糊更重；加载途中祖先兜底纹理与叶瓦锐度不一，主观上呈“碎片化”。
- 3857 标准档字号本就合适，不需要该偏移；4490 的正确解法应在**重投影选源环节**完成，而不是放大已生成的墨卡托纹理。

### 11.4 修复：geoSourceLevelBias 地理源选源层级偏置（实验、可回退）

- 新增**单图层建层选项** `geoSourceLevelBias`（仅 crs=EPSG:4326/4490 生效），重投影选源公式由 s=z 改为 **s=clamp(z+bias, 0, maxGeoLevel)**，引擎钳 [-2,1]，默认 0 与 v2 完全一致。
- 天地图 4490 注记层传 **-1**：恢复水平 1:1 源瓦映射，字号与 3857 标准一致，且全程无 GPU 放大（重投影在 256 源像素尺度一次完成），字形清晰；影像层保持 0 不变。
- 折中（如实记录）：bias=-1 时垂直方向保留 c 服务固有扁率，西安 34° 约 1.2 倍轻拉高，纬度越高越明显；用户可按图层在“字小但各向同性(0)”与“字号正常但轻拉高(-1)”间自行选择。
- 缓存隔离：不同偏置对同一墨卡托瓦产出不同 PNG，demo 结果缓存目录按偏置分目录（`tianditu-c4326-v2-gb-1-*`），原始地理瓦缓存（-geo-raw）仍按服务矩阵共享。
- 真机实证（西安 z8/倾角 0 标准机位，2026-11-13）：日志确认注记层 `srcLevelBias=-1`、影像层 0；三帧同机位对比中 4490(bias=-1) 字号/清晰度与 3857 标准基本一致，4490(bias=0) 明显字小。

### 11.5 改动清单（注释均带 @Jin）

- C++ 核心（globecore）：`TileLayer.h` TileLayerOptions 增 `geoSourceLevelBias`；`TileLoader.h/.cpp` 增成员、setGeographicSource 末位默认参数、generateGeographicTile 选源公式与日志；`GlobeEngine.cpp` 透传并入建层日志。
- Harmony NAPI/ArkTS：`napi_globe_engine.cpp` 解析 options.geoSourceLevelBias；`NativeLib.ets` TileLayerInitOptions 增字段；`NativeMapView.ets` 用法文档。
- Demo：调试坞 LOD 面板增「4490 注记源级偏置（标准 s=z / 放大 s=z-1）」，切换即墓碑化重建 4490 图层；仅 4490 生效，3857/ArcGIS 下点击仅提示。
- 文档：本节 + docs/api/harmony-api-guide.html（options 字段、推荐示例、已知边界）。
- 回退：不传该字段或传 0 即完全恢复 v2；删除选项字段与 TileLoader 偏置分支即彻底回退。
- 待办：Android JNI 尚未暴露 TileLayerOptions（C++ 默认 0 已保证零行为变化），后续按需补 JNI/Kotlin 入口与 Android API 文档。

### 11.6 两个“偏移”的分工定稿与单图层作用域评估（2026-11-13）

- 背景：serviceZoomOffset 下沉为单图层参数后，4490 注记设 -1 出现模糊/碎片化，需确认是否为改造回归、参数作用域是否合适。
- 全链路核查（ArkTS Index.ets → NAPI napi_globe_engine.cpp → GlobeEngine → Renderer/TileLoader）：传参无误；对照提交 7086275 的 diff，旧全局实现 `level += tileLodBias_` 与新单图层实现 `level = nav_.tileLevel(maxLevel) + layerZoomOffset` 数学等价，模糊不是下沉改造引入的回归。
- 两个参数的分工定稿：

| 参数 | 作用层面 | 机制 | 视觉结果 | 适用场景 |
|---|---|---|---|---|
| `serviceZoomOffset` | Renderer 引擎选瓦级（2D/3D 同步，SSE 同步缩放） | 选更粗墨卡托瓦，GPU 双线性放大显示 | 固有软化；4490 还叠加重投影重采样，模糊/碎片化更重 | 弱网/限流省流节流，全栅格图层通用 |
| `geoSourceLevelBias` | TileLoader 重投影选源（仅 crs=EPSG:4326/4490） | s=clamp(z+bias) 选地理源瓦，仍产出标准 z 级墨卡托瓦 | texel 1:1、无 GPU 放大，清晰；代价是高纬 sec(φ) 轻拉高 | 4490/c 切片注记字号修复 |

- 单图层作用域评估结论：**合适，不回退全局**。LOD 偏移本质是图源属性（各层金字塔/服务结构不同，档位本就可能不同）；粗瓦地理四至由四叉树严格保证，层间不会错位；全局 `setTileLodBias` 保留为整 App 省流总开关，与单图层偏移正交可叠加。
- 4490 注记推荐配置（定稿）：`serviceZoomOffset=0` + `geoSourceLevelBias=-1`；影像层两者均保持 0。
- demo 防误用（仅 ArkTS 层，C++ 零改动）：天地图 4490 图源下 LOD 调试面板对注记层 serviceZoomOffset 负档给出橙色警示，引导改用源级偏置；说明文字重新定位为“省流节流（清晰度换流量），不是字号修复手段”。

### 11.7 geoVerticalCompensation 垂直拉高补偿（实验封存，2026-11-13）

- 动机：11.4 的 bias=-1 恢复字号后保留 c 服务固有 sec(φ) 垂直拉高（西安 34.3°≈1.21 倍），真机反馈“垂直轻拉高过度，有点夸张”。
- 实现（实验、可回退）：新增连续系数 `geoVerticalCompensation`，引擎钳 [0,1]，默认 1.0。公式 `sv = 1 + vComp·(sec(中心纬度) − 1)`，以瓦中心锚定调整纬度带 `adjLatSpan=256/sv/pixelsPerLat`；vComp<0.999 时纵向在调整带内线性取纬（放弃墨卡托反投影），多采样的源内容压回 256 行；水平方向与瓦片几何完全不动。1.0＝各向同性严格瓦边界（与 v2 字节级一致），0.5＝拉高减半（西安有效 sv≈1.10），0＝源像素方形映射（字形最方正）。
- 缓存隔离：不同补偿产出不同 PNG，demo 结果缓存按补偿分目录（`-vc50-*`、`-vc0-*`，默认 1.0 不带后缀）。
- 真机验证情况：同机位（西安 z8/倾角 0）三档 1.0/0.5/0 日志均确认生效；但同 ROI 连通域高宽比测量受影像未加载完、跨帧字形粘连/分裂差异污染（三档样本 n=37/33/20，字形集合不一致，中位数不可比），未取得可信量化结论；代码几何方向经逐行推导确认正确。
- 代价（如实记录）：系数<1 时纵向采样超出瓦真实纬度带，水平接缝附近 overlay 注记理论上可能轻微重影，仅建议注记层使用，影像层保持默认。
- 结论：**实验封存**。库默认 1.0 零行为变化；demo 调试按钮保留但标题标注“实验封存：默认 1.0，专项讨论前勿调”；后续针对 4490 拉高专项讨论时再启用验证。回退方式：不传该字段或恒传 1.0，彻底回退删除 TileLayerOptions/TileLoader 中该字段与补偿几何分支。
- 改动文件（注释均带 @Jin）：C++ `TileLayer.h`（options 字段）、`TileLoader.h/.cpp`（成员/钳制/补偿几何/日志/缓存目录）、`GlobeEngine.cpp`（透传）；Harmony `napi_globe_engine.cpp`、`NativeLib.ets`、`NativeMapView.ets`；demo `Index.ets`（三档按钮与缓存 tag）；Harmony API 文档与本记录。
- 待办延续：Android JNI 尚未暴露 TileLayerOptions（含 geoSourceLevelBias 与 geoVerticalCompensation），C++ 默认值保证零行为变化，后续按需补 JNI/Kotlin 入口与 Android API 文档。

### 11.8 单图层参数收口：移除 Renderer 级 serviceZoomOffset，zoomOffset 承接选源语义（2026-11-13，构建号 v0.1.5-20261113-zoomoffset1）

- 背景：11.6 定稿两个偏移分工后，旧 `serviceZoomOffset`（Renderer 选瓦级，GPU 放大软化、4490 双重重采样模糊）已无适用场景（省流由全局 `setTileLodOffset` 承担），且与 `geoSourceLevelBias` 名称并存易误用。用户决策：移除旧参数，把对外名称 `serviceZoomOffset` 简化为 `zoomOffset` 并赋予 `geoSourceLevelBias` 的选源语义；引擎内部成员保留 `geoSourceLevelBias_` 原名。
- 语义变更（对外）：
  - `TileLayerInitOptions.serviceZoomOffset`（[-3,2]，全图层、含运行时 setter）**移除**；
  - `TileLayerInitOptions.geoSourceLevelBias` 更名为 **`zoomOffset`**（[-2,1]，仅 crs=EPSG:4326/4490 生效，墨卡托图层静默忽略，仅建层 options 传入、修改需重建图层，无运行时 setter）；
  - `geoVerticalCompensation` 字段名不变（实验封存，默认 1.0）。
- C++ 改动（注释均带 @Jin）：
  - `TileLayer.h`：TileLayerOptions 删除 `serviceZoomOffset`、`geoSourceLevelBias` 更名 `zoomOffset`；TileLayer 删除 `serviceZoomOffset` 成员。
  - `GlobeEngine.h/.cpp`：删除 `setLayerZoomOffset` 声明与实现；建层日志改为 `... zoomOffset=%d`；`setGeographicSource` 入参由 `options.zoomOffset` 传入（TileLoader 内部仍名 geoSourceLevelBias_，选源公式 s=clamp(z+bias) 不变）。
  - `Renderer.h/.cpp`：删除 `lastIdleGlobeTileZoomOffset_` 成员与 4 处重置点；2D `renderLayer` 与 3D `renderGlobeLayer3D` 选瓦恢复全局单值 `nav_.tileLevel(maxLevel)`；删除 `layerSseScale`，3D SSE 恢复 `lodDetailFactor × tileLodSseScale()`；交互迟滞恢复仅按 maxLevel 隔离。
- Harmony 桥改动：`napi_globe_engine.cpp` 删除 NativeSetLayerZoomOffset 与注册表项，options 解析改读 `zoomOffset`；`index.d.ts`、`NativeLib.ets`、`NativeMapView.ets` 同步删方法/改字段/更新注释。demo/library 为 junction 自动同步。
- Demo 改动（Index.ets）：删除 imgZoomOffset/labelZoomOffset 状态、selectLayerZoomOffset 方法与调试面板两组按钮及防误用提示；geoLabelLevelBias 状态更名 `geoLabelZoomOffset`、方法更名 `selectGeoLabelZoomOffset`；4490 注记 options 改传 `zoomOffset`，影像层与墨卡托图层传空 options；结果缓存 tag 由 `-gb` 改 **`-zo`**（旧实验缓存不再命中，首次重新下载，原始地理瓦 -geo-raw 缓存不变）；LOD 面板保留「4490 注记 zoomOffset（仅 c 切片）」两档按钮与垂直补偿封存组。
- 真机验证（设备 5KLBB25911200313，2026-11-13）：BUILD ID `v0.1.5-20261113-zoomoffset1` 入日志；切「放大 s=z-1」后日志 `addTileLayer ... crs=EPSG:4326 geo=1 zoomOffset=-1`、TileLoader `srcLevelBias=-1 vComp=1.00`、缓存目录 `tianditu-c4326-v2-zo-1-cia(-geo-raw)`；标准档全程 zoomOffset=0；应用启动、图源切换、地形加载正常。
- 影响面与回退：默认 0 时选瓦/选源与改造前完全一致；全局 `setTileLodOffset/setTileLodAlignDisplay` 原样保留（整 App 省流/对齐显示实验不受影响）；Android JNI 未暴露 options，零影响。回退方式：整体 revert 本提交即恢复双参数形态。
- 文档：Harmony/Android API 指南同步（字段表、示例、故障对照、移除说明、规划中段落）；历史设计文档（layer-init-options-proposal、epsg-4490 design、har-packaging-plan）保留原文作存档，不回改。
