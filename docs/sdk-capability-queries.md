# SDK 能力查询结论

> 维护约定：每次回答“SDK 是否支持某能力”的查询后，将结论、代码依据、边界和后续建议追加到本文档，便于后续开发和 API 设计追溯。用户追加记录统一标注 `@Jin`。

## 索引

| 日期 | 能力 | 当前结论 | 备注 |
|------|------|----------|------|
| 2026-10-29 | 地形夸张 / 垂直缩放 | 暂不支持公开 API，仅有内部固定系数 `1.0` | 后续产品化为 `setTerrainExaggeration()` |
| 2026-10-29 | Overlay / 矢量文字标注 | 支持 `labels` 数据通路；Harmony 宿主应显式 `setFontPath()` 加载中文字体 | 已验证 native 几何含 `labels=1`，无字体时不绘制文字 |
| 2026-10-29 | 绕点旋转 | 支持绕当前相机目标点（屏幕中心）旋转；尚无任意经纬度/屏幕焦点旋转 API | 双指旋转可用；需要扩展 `rotateHeading(delta, focusX, focusY, lon, lat)` |
| 2026-11-10 | 3D Tiles / 倾斜摄影模型加载 | 实验分支 feature/3dtiles-b3dm 已打通 C++ 内核 P0-P2（b3dm 解码、SSE 调度、Renderer 纯色渲染），对外 API 尚未导出 | LRU/纹理/PBR/平台 API 待 P3/P4 |
| 2026-11-13 | 矢量要素点击命中（map.click / pick） | **支持命中检测，C++ 层实现**；但命中后仅返回 layerIndex+fid，GeoJSON 全属性回取在 LITE 包不可用 | 属性内存化回取列入待办 |

## 2026-10-29：地形夸张 / 垂直缩放参数

- @Jin 查询问题：SDK 是否支持地形夸张参数（terrain exaggeration / vertical scale），用于放大山体起伏。
- @Jin 当前结论：**暂不支持宿主通过 Harmony、Android 或 Qt 门面 API 配置地形夸张系数**；没有 `setTerrainExaggeration()`、`setVerticalExaggeration()` 或同类接口。
- @Jin 代码依据：`globecore/src/main/cpp/render/Renderer.cpp` 的 `buildGlobeTileMeshLocal()` 中存在内部常量：

```cpp
constexpr double kTerrainExaggeration = 1.0;
const double alt = rawAlt * kTerrainExaggeration;
```

- @Jin 当前值固定为 `1.0`，表示使用真实 DEM 高程；该常量只影响 3D 地形网格顶点，不属于 Provider、相机或公开 VectorStyle 配置。
- @Jin 已核查门面：Harmony `NativeMapView.ets` / NAPI `index.d.ts`、Android `NativeMapView.kt` / JNI、`GlobeEngine`、`CesiumTerrainProvider` 均未暴露夸张系数参数。
- @Jin 不能只把常量改成 `2.0` 后定稿：矢量贴地采样、3D 标注锚点、相机目标点/安全高度、近景瓦片排序和 VBO/地形缓存失效都仍读取真实高程，可能造成矢量埋地、相机撞山或 LOD 偏差。
- @Jin 后续建议 API：

```ts
mapView.setTerrainExaggeration(scale: number): boolean;
mapView.terrainExaggeration(): number;
```

```kotlin
fun setTerrainExaggeration(scale: Double): Boolean
fun terrainExaggeration(): Double
```

- @Jin 建议规则：默认 `1.0`；可先限制范围 `0.0..3.0`；修改后同步作用于地形网格、矢量 DEM 贴地、3D 标注、相机贴地/安全高度和瓦片距离排序，并使相关网格与贴地 VBO 缓存失效重建。


## 2026-10-29：Overlay / 矢量文字标注

- @Jin 查询问题：SDK 是否支持在 overlay 点要素或矢量要素上显示文字标注；为什么 `poi.png` 图标可见但中文文字不可见。
- @Jin 当前结论：**SDK 支持文字标注**。Harmony `NativeMapView.updateOverlayPoints(index, lonlat, fids, labels)` 可将逐要素 `labels` 传到 native，日志 `[VecReload] buildGeometry(serial): ... labels=1` 表明文字数据已进入 `VectorGeometry.labels`。
- @Jin 代码依据：Harmony 门面 `updateOverlayPoints()` → NAPI → `GlobeEngine::updateOverlayPoints()` → `VectorFeatureData.label` → `VectorBuilder` 写入 `LabelItem`；渲染由 `Renderer::drawVectorLabels()` / `Renderer::drawVectorLabels3D()` 绘制。
- @Jin 注意边界：文字绘制依赖 `FontAtlas` 字体加载。SDK 虽有系统字体自动探测，但 Harmony Demo/HAR 未主动调用 `setFontPath()` 时，可能出现几何中已有 `labels=1`、画面却没有文字的情况。宿主应在初始化 mapView 后、添加标注层前显式调用：

```ts
mapView.setFontPath('/system/fonts/HarmonyOS_Sans_SC.ttf');
```

- @Jin 对“图标 + 文字”的推荐用法：图标层和纯文字层拆成两个 overlay，图标层传 `iconRgba`，文字层不传图标且设置透明点样式/白色描边文字；这样可避免图标 billboard 分流与文字标注路径相互耦合。
- @Jin 真机验证结论（2026-10-29，HarmonyOS 平板）：快捷定位的太白山、华山、珠峰三个 POI 文字均已人工确认可见；最终方案为显式加载 `/system/fonts/HarmonyOS_Sans_SC.ttf`，POI 图标层与文字层拆分，文字约 28dp、白字黑描边并显示在图标上方。


## 2026-10-29：绕点旋转（heading / orbit rotation）

- @Jin 查询问题：SDK 是否支持绕点旋转。
- @Jin 当前结论：**支持 3D 模式下绕当前相机注视目标点旋转**，但暂不支持宿主直接指定“任意经纬度点”或“任意屏幕焦点”作为旋转轴心。
- @Jin 已有能力：
  - Harmony 门面 `NativeMapView.rotateBy(deltaDeg)` → NAPI `nativeRotateHeading()` → `GlobeEngine::rotateHeading()` → `Navigator::rotateHeading()`。
  - Harmony `MapGestures` 已支持双指旋转手势：两指连线角度变化后逐帧调用 `rotateBy(deltaDeg)`。
  - `Camera` 携带 `heading` 字段，宿主可通过 `setCamera()` / `flyToCamera()` 设置绝对方位角。
  - 2D 模式当前恒正北，不消费 heading；旋转只在 3D 球体透视模式生效。
- @Jin 代码依据：`Navigator.h` 中 `rotateHeading(double deltaDeg)` 注释说明“手势旋转：相机 heading 累加 deltaDeg”；`Navigator.cpp` 中该函数只修改 `heading_`，没有 focusX/focusY 或目标经纬度参数。
- @Jin 语义边界：当前 LookAt/Orbit 相机的旋转中心是相机当前目标点，即屏幕中心/相机 center；双指手势虽然计算了双指质心用于缩放，但旋转调用没有把质心传入 native。
- @Jin 若后续要实现严格“绕双指质心旋转”或“绕指定山顶/POI 旋转”，建议新增可回退 API：

```ts
mapView.rotateAroundPixel(focusXPx: number, focusYPx: number, deltaHeadingDeg: number): void;
mapView.rotateAroundPosition(longitude: number, latitude: number, altitude: number | undefined,
                             headingDeg: number, tiltDeg?: number, durationMs?: number): void;
```

```kotlin
fun rotateAroundPixel(focusXPx: Float, focusYPx: Float, deltaHeadingDeg: Double)
fun rotateAroundPosition(longitude: Double, latitude: Double, altitude: Double?,
                         headingDeg: Double, tiltDeg: Double = Double.NaN, durationMs: Double = 0.0)
```

- @Jin 实现要点：旋转前把焦点屏幕点反算为地理目标；旋转 heading 后保持该地理目标在同一屏幕位置，并维持 altitude/tilt；对地形模式应使用当前 DEM 表面高度或允许调用方传入目标高度。

## 2026-11-10：3D Tiles / 倾斜摄影三维模型加载

- @Jin 查询问题：当前 GlobeCore 是否具备加载 3D Tiles 格式倾斜摄影三维模型（典型内容为 `tileset.json` + `.b3dm`）的能力。
- @Jin 当前结论：**当前 SDK 不具备开箱可用的完整 3D Tiles/倾斜摄影加载能力**；仓库中已经存在两个“预备型”基础模块，但尚未形成数据调度、坐标转换、GPU 渲染和平台 API 的闭环。
- @Jin 已有基础一：`globecore/src/main/cpp/layer/TilesetJson.*` 可解析 3D Tiles 1.0 `tileset.json` 的部分字段，包括 `boundingVolume` 的 `box/region/sphere`、`geometricError`、`refine`、`content.uri` 和 `children`；但当前没有解析节点 `transform`，也未被 `GlobeEngine`、Renderer 或 Harmony/Android 门面调用。
- @Jin 已有基础二：`globecore/src/main/cpp/gltf/GltfDecoder.*` 可解码 binary glTF（`.glb` v2）的 `POSITION/NORMAL/indices` 和 `baseColorFactor`；但头文件明确说明暂不支持外部 `.gltf`、纹理、动画、骨骼、Morph target，且 glTF 到引擎 ECEF 坐标的变换需要调用方负责。
- @Jin 关键缺口：没有 `.b3dm` 容器解析（magic/header/feature table/batch table/内嵌 GLB 提取/RTC_CENTER），没有 `i3dm/pnts/cmpt` 解析，没有基于相机视锥和屏幕空间误差（SSE）的 Tileset LOD 遍历，没有 ADD/REPLACE 细化调度、异步加载/卸载/缓存策略，也没有 ECEF transform、RTC 坐标、深度融合和模型 VBO/纹理上传的 Renderer 通路。
- @Jin API 核查：Harmony NAPI/ArkTS、Android JNI/Kotlin 与 Demo 层均未发现 `addTileset3D`、`load3DTiles`、`b3dm`、模型图层等公开接口；当前公开能力仍集中在栅格瓦片、地形、矢量、Overlay 和标注。
- @Jin 可行性判断：项目已具备 OpenGL ES 渲染线程、相机/视锥/矩阵、WGS84/ECEF 转换、HTTP 下载、磁盘缓存、地形 LOD 调度和 GLB 雏形，因此**可以在现有 C++ core 上扩展实现**；但这是中等偏大的底层能力建设，不是 Demo 层接入即可完成。
- @Jin 建议路线：第一阶段只实现最常见的 `b3dm + 内嵌 GLB` 最小闭环，新增 `Tileset3DLayer/Provider`，先支持 `tileset.json` 的 `transform/content/boundingVolume/geometricError/refine/children`、b3dm 解包、RTC_CENTER、SSE LOD、REPLACE/ADD 和纯色/简单材质渲染；第二阶段补齐 glTF 纹理、外部资源、`i3dm/pnts/cmpt`、要素拾取、批量表属性、透明/高亮/剖切等高级能力。
- @Jin 2026-11-10 分支 `feature/3dtiles-b3dm` 进展：已完成 P0/P1 数据层实验，包括 b3dm 解包、内嵌 GLB 提取、RTC_CENTER 保留、`transform/gltfUpAxis/content.url` 解析、相对 URL、双精度世界变换和基础 SSE 选择器；真实数据可解出 GLB 并选中 b3dm，但尚未接入 `GlobeEngine`、Renderer 或 Harmony/Android API，因此对外能力结论仍为“暂不支持完整加载”。

- @Jin 2026-11-10 P2 进展（C++ 内核渲染闭环，仍未导出平台 API）：
  - @Jin 新增 `tileset3d/Tileset3DModel.*`（b3dm/GLB 转 anchor-RTC 交错顶点，处理 `gltfUpAxis=Z` 换轴、`RTC_CENTER`、GLES2 超 65535 索引展开）与 `Tileset3DProvider.*`（后台线程异步调度）。
  - @Jin `GlobeEngine` 新增内部 API `addTileset3D(url)/setTileset3DVisible(id,visible)`；`Renderer` 新增独立 3D Tiles shader 与 VBO/IBO 上传、深度测试通路，栅格之后矢量之前绘制。
  - @Jin Provider 调度加固：外部 tileset 改为工作清单递归展开，支持 root→external→external→b3dm 任意深度（实测数据深度 2：65 个 JSON、92 个 b3dm 全部可达）；修复 `lock_guard` 内手动 unlock 的未定义行为；接入 `Camera3D` double 精度视锥 6 平面做保守裁剪；单帧最多处理 8 个 b3dm 限制冲击。
  - @Jin 验证：鸿蒙 NDK clang 全量语法核查通过；`.cxx` ninja 完整编译链接产出 `libglobecore.so`（14/14 目标零 error）；Node 对真实服务模拟递归确认全部叶子 b3dm URL 可解析下载。
  - @Jin 当前边界：仅纯色/Lambert，无纹理与 PBR；无磁盘缓存、LRU 卸载、失败重试、ADD refine；无 NAPI/JNI/ArkTS/Demo 入口；移除 `tileset3d/` 源清单与 `GlobeEngine/Renderer` 接线即可整体回退。

## 2026-11-13：矢量要素点击命中查询（GeoJSON 等 map.click 能力）

- @Jin 查询问题：SDK 是否支持地图数据要素（如 GeoJSON 图层）的点击命中查询（map.click）；能力在哪一层实现，C++ 还是 ArkTS。
- @Jin 总结论：**点击命中检测能力已具备，且核心实现在 C++ 层，ArkTS/Android 仅为门面透传**；当前 demo 已在用（单击 Toast 显示经纬度 + 命中 fid）。真正的缺口是「命中后取 GeoJSON properties 属性表」在 Harmony LITE 包不可用。
- @Jin 分层证据：
  1. **C++ core（能力本体，globecore/src/main/cpp）**：`GlobeEngine::pickVector(sx,sy,outLayerIndex,outFid)` 2D 走 GPU PickID 离屏 pass（RGBA8 FBO + pickIdVbo，UI 线程发请求→GL 线程 drawFrame 回读，100ms 超时自动回退 `pickVectorCpu`）；`pickVector3D` 为屏幕空间投影判据，对 pickPrims（RTC 世界坐标，与贴地渲染同口径）逐图元点/线/面容差判定。两阶段优先级：先动态叠加层后文件矢量层，层内逆序（后加优先）、点线优先于面；`setOverlayNoPick` 可让高亮瞬态层退出拾取竞争；隐藏/级别不足/墓碑层与绘制同进同退不可拾取。
  2. **命中后配套（C++）**：`featureGeometry(layerIndex,fid)` 回取经纬度几何（供画选中高亮）、`featureRenderColor` 回取实际渲染色；`screenToGeo` 回算点击点经纬度。
  3. **Harmony 通路**：native 手势桥 `XComponentBridge` UP 分支（位移 < touchSlop）→ `postPendingTap` → 渲染线程 TSFN → NAPI `nativeSetTapCallback` → ArkTS `NativeMapView.setOnTapListener`；NAPI 另导出 `nativePickVector/nativeFeatureGeometry/nativeSetOverlayNoPick`，ArkTS 门面 `pickVector()` 返回 `PickResult{layerIndex,fid}`。
  4. **Android 通路**：JNI 导出 pickVector/featureGeometry/**featureRenderColor**（Harmony NAPI 目前缺 featureRenderColor 导出，平台不一致小缺口）。
  5. **demo 现状**：`Index.ets onMapTap` 已串联 screenToGeo + pickVector，Toast 输出「经纬度 + 命中要素 fid」。
- @Jin 关键缺口（属性表）：
  - 拾取结果只有 layerIndex + fid，**没有按 (layerIndex,fid) 回取 GeoJSON feature.properties 的公开 API**；解析期 `VectorFeatureData` 只保留单个 labelField 文本，全量 properties 被丢弃（GDAL 通路与 LITE `lite_geojson_reader.cpp` 同）。
  - 既有 `NativeVector.getFeatureAttributes(path, featureId)` 是**按源文件路径重新打开 GDAL/OGR 读取**，不接受 layerIndex；full 构建对本地文件可用，**LITE 构建在 `lite_gdal_stubs.cpp` 为桩（恒返回空串/false）**；在线 GeoJSON 虽落了缓存文件，但 LITE 无 OGR 无法二次解析，overlay 内存层则根本无源文件。
- @Jin 改造方向（已列入 jin-modification-records.md 第五节待办）：解析期把 properties 内存化（fid→JSON 字符串，配字段白名单/层数与体积上限），新增 C++ `featureAttributes(layerIndex,fid)` 并双端导出，替代按路径二次读文件的旧模式。
