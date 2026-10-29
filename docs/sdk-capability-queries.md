# SDK 能力查询结论

> 维护约定：每次回答“SDK 是否支持某能力”的查询后，将结论、代码依据、边界和后续建议追加到本文档，便于后续开发和 API 设计追溯。用户追加记录统一标注 `@Jin`。

## 索引

| 日期 | 能力 | 当前结论 | 备注 |
|------|------|----------|------|
| 2026-10-29 | 地形夸张 / 垂直缩放 | 暂不支持公开 API，仅有内部固定系数 `1.0` | 后续产品化为 `setTerrainExaggeration()` |
| 2026-10-29 | Overlay / 矢量文字标注 | 支持 `labels` 数据通路；Harmony 宿主应显式 `setFontPath()` 加载中文字体 | 已验证 native 几何含 `labels=1`，无字体时不绘制文字 |
| 2026-10-29 | 绕点旋转 | 支持绕当前相机目标点（屏幕中心）旋转；尚无任意经纬度/屏幕焦点旋转 API | 双指旋转可用；需要扩展 `rotateHeading(delta, focusX, focusY, lon, lat)` |

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
