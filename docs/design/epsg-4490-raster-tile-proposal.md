# EPSG:4490 地理坐标系栅格瓦片兼容评估与方案

> @Jin 本文档为 `feature/epsg-4490` 分支上的 P0 评估（Spike）产出，只做可行性论证与方案设计，**尚未改动任何实现代码**，等待确认后再实施。
>
> 状态：@Jin P0 代码走读完成，结论＝**可行**，推荐先走「引擎四叉树不动 + 图层级地理源适配（GDAL 单瓦重投影）」低风险闭环，必要时再做「原生 per-layer TileScheme」彻底方案。
> 日期：2026-10-29
> 适用仓库：`E:\WorkBuddy\qt_ohos_map_sdk\vendor\GlobeCore`（分支基于 main `9cb7f00`）
> 实施约束：实验性能力、可回退、默认关闭；新增/修改代码注释必须带 `@Jin`；定稿后同步更新 `README.md` 与 `docs/api/android-api-guide.html`；主线为 HarmonyOS NEXT，Android 只做 API 预埋。

---

## 1. 结论速览（TL;DR）

1. **EPSG:4490 ≠ 一种“瓦片切法”，而是一个地理坐标系（基准）。** 真正决定能不能直接上图的是服务的 **TileMatrixSet（切片矩阵）**：Web 墨卡托（z0 1×1）还是地理等经纬（z0 2×1、线性纬度）。
2. 4490（CGCS2000）与 4326（WGS84）椭球差异在亚米级，**栅格可视化可把 4490 瓦片的经纬度直接当 WGS84 用**，不需要额外基准转换。工程上把 `EPSG:4326` 与 `EPSG:4490` 的**地理切片**按同一套「等经纬切片」处理即可。
3. 引擎现状是「**全场景唯一一套 Web 墨卡托四叉树几何，图层只负责按同一 `(z,x,y)` 提供纹理**」。直接把 4490 地理瓦片塞进 `{z}/{x}/{y}` 会错位（根块数、纬度映射、纹理键全不一致）。
4. **代码里已有两块可直接复用的先例**：
   - 三维地形本身就是 **EPSG:4326 + quantized-mesh 地理切片**，通过 `cesiumGeoTileBounds()` + `intersectingGeoTileRange()` 把「引擎墨卡托瓦」映射到「地理服务瓦」，已经在真机跑通；
   - 本地栅格通路 `RasterRenderer::renderTile()` 已能用 GDAL **按单个引擎瓦片四至做重投影并落盘缓存**。
5. 因此推荐 **方案 A（适配层，低风险，2D/3D 通用，零几何改动）** 先做实验闭环；对齐精度/性能不达标再投入 **方案 B（原生 per-layer TileScheme，改动大但最“正”）**。若数据方能同时提供墨卡托端点（天地图 `img_w`），优先直接用，等于 **方案 C（零成本规避）**。

---

## 2. 先澄清：4490 / 4326 / 3857 到底差在哪

| 标识 | 类型 | 单位 | 备注 |
|---|---|---|---|
| EPSG:3857 | 投影坐标（Web 墨卡托） | 米 | z0 全球 **1×1**，纬度用墨卡托反投影，有效纬度截断在 ±85.0511° |
| EPSG:4326 | 地理坐标（WGS84 经纬度） | 度 | 地理切片常见 z0 **2×1**，纬度线性 |
| EPSG:4490 | 地理坐标（CGCS2000 经纬度） | 度 | 椭球与 WGS84 差异亚米级，可视化可忽略 |

- 常见误区：“数据是 4490 的，所以要做坐标转换”。**对显示而言真正的工作量在切片矩阵**，不在基准。
- 地理切片（OGC 通常以 `EPSG:4326` TileMatrixSet 提供；国测局/天地图 `_c` 同构）规则：

```text
列数 nCols = 2^(z+1)        // z0 = 2 列
行数 nRows = 2^z            // z0 = 1 行
lonMin = -180 + x     * 360 / nCols
lonMax = -180 + (x+1) * 360 / nCols
latMax =  90  - y     * 180 / nRows   // XYZ：y=0 在北（图像顶行）
latMin =  90  - (y+1) * 180 / nRows
```

  每块 z0 跨 `180°×180°`（256px）。TMS 行序（y=0 在南）需用 `reverseY = (2^z - 1) - y` 翻转——行序必须做成**每图层参数**，不能写死。

---

## 3. 现状代码走读（为什么“不能直接换 URL”）

### 3.1 2D：归一化墨卡托平面 + 轴对齐方块

- `globe/MercatorProjection.h`：世界坐标归一化到 `[0,1]²`，纬度走墨卡托。
- `globe/TileMatrix.h`：`tilesAtLevel(z)=2^z`，`visibleTileRange()` 按 `floor(wx*2^z)` 选瓦，**默认全球 2^z×2^z**。
- `render/Renderer.cpp::renderLayer()`（约 1632 行起）：瓦片画在 `tx/2^z`、`ty/2^z` 的**轴对齐方块**上，纹理线性映射；占位、祖先兜底、半纹素外扩都基于此。
- 结论：2D 平面坐标、相机、矢量、拾取大量建立在墨卡托世界坐标上，**不宜为 4490 整体改坐标系**。

### 3.2 3D：唯一一套墨卡托四叉树，几何全局共享

- `globe/Tessellator.cpp::tileToBounds()`：`(z,x,y)` 经纬度四至用**墨卡托反投影**（`atan·sinh`），根块固定 `subdivide(0,0,0)`（z0 1×1）。
- `globe/GeographicQuadtree.cpp`：带帧间一致性的选瓦器，但注释明确「**纹理键仍用 Web 墨卡托 `(z,x,y)`，与 TileCache/TileLoader/URL 全链路兼容**」。
- `render/Renderer.cpp::buildGlobeTileMeshLocal()`（约 2347 行）：网格顶点已经是 `经纬度 → Wgs84Globe ECEF`，但**经纬度四至来自墨卡托 `GlobeTile`**，UV 线性。
- **关键约束**：`Renderer::tileGlobeMesh_`（`Renderer.h:517`）是**全局**网格缓存，键仅为 `tileKey(z,x,y)`（见 `acquireTileGlobeMesh()` 2459 行），**不分层**。即所有图层共用同一套 3D 几何，图层只提供同 `(z,x,y)` 的纹理。
- 含义：若让某个图层改用 2×1 地理根，会与其它图层/全局几何的 `(z,x,y)` 语义冲突——这是方案 B 的主要改造成本所在。

### 3.3 纹理 / 缓存 / URL：全链路按 `(z,x,y)`，缓存隔离只靠目录

- `layer/TileLoader`：`key(z,x,y)`、`buildUrl()` 用 `{z}/{x}/{y}` 占位；请求去重、失败退避、视口代际都按这个键。
- `layer/TileCache`：文件 `<baseDir>/<z>/<x>_<y>.tile` 或 MBTiles；**不同切片方案只靠 `baseDir` 不同来隔离**。同 `(z,x,y)` 在 3857 与 4490 下不是同一张图，**必须分目录/分库，杜绝串缓存**。
- `core/GlobeEngine::addTileLayer(cacheDir,url,maxLevel,overlay)` → Harmony `NativeMapView.addTileLayer()`（`NativeMapView.ets:228`）→ NAPI `NativeAddTileLayer`（`napi_globe_engine.cpp:603`）→ `index.d.ts:35`。**当前没有任何 CRS / TileMatrixSet / tileSize / 行序参数**。

### 3.4 已有的两块“地基”（降低风险的关键）

1. **地形已经在消费 4326 地理切片**：
   - `terrain/CesiumTerrain.cpp::cesiumGeoTileBounds(z,x,y)`：`cols=2^(z+1)、rows=2^z` 的**线性经纬度**换算（正是 §2 的地理公式，TMS 行序）。
   - 同文件 `webMercatorTileBounds()` + `cesiumServiceTilesForEngineTile()` + `intersectingGeoTileRange()`：把「引擎墨卡托瓦四至」换算成「相交的一块或多块地理服务瓦」，URL 用 `{reverseY}` 翻转（`CesiumTerrainProvider.cpp:532`）。
   - 即“**引擎几何保持墨卡托、服务瓦片是地理切片，按地理交集映射**”这条路在本仓库已被地形验证过。
2. **本地栅格已在做单瓦 GDAL 重投影**：
   - `raster/RasterRenderer.h`：`renderTile(path, minLon,minLat,maxLon,maxLat, 256, outPng)`，内部建 WGS84 warp VRT、GDAL 全局互斥、交集外透明、MEM→PNG、按路径 LRU。
   - `layer/TileLoader.cpp:411` 磁盘未命中时用 `tileBounds(z,x,y)`（墨卡托四至，`TileLoader.cpp:48`）调它生成 256² PNG 并**回写磁盘缓存**。
   - 即“**把非墨卡托源重投影成引擎要的那块墨卡托 256² 瓦，再落盘**”的流水线、缓存、背压都已存在，只是当前输入是本地文件。

---

## 4. 三种方案对比

| 维度 | 方案 A：图层级地理源适配（推荐先做） | 方案 B：原生 per-layer TileScheme | 方案 C：直接用墨卡托端点 |
|---|---|---|---|
| 思路 | 引擎仍选墨卡托瓦；图层把它映射到相交 4490 服务瓦，GDAL/MEM 拼接+重投影成该墨卡托 256² 瓦 | 每图层自带切片方案（地理四叉树 2×1），独立选瓦/独立网格 | 数据方提供 3857（天地图 `img_w/cia_w`）就直接用 |
| 3D 改动 | **无**（几何/全局网格不动） | 大：选瓦、`tileGlobeMesh_` 全局键要按方案分命名空间、父子兜底、视锥/LOD | 无 |
| 2D 改动 | **无**（输出的就是墨卡托瓦） | 大：要么地理平面化，要么非线性变形网格嵌入墨卡托平面 | 无 |
| 矢量/地形/拾取 | 零影响 | 需逐一核对坐标系 | 零影响 |
| 对齐质量 | 单瓦 warp，一次性、落盘；赤道近 1:1，高纬轻微过采样 | 原生线性纬度，无 warp，最“正” | 最好（原生切片） |
| 首屏 CPU | 每块缺瓦一次 GDAL warp（全局互斥，缓存后零成本） | 无 warp，但重构面大、回归风险高 | 零 |
| 回退 | 图层加参数，默认走旧路径，删参数即回退 | 改动深，回退成本高 | 零 |
| 适用 | 多数政务/国土 4490 WMTS、天地图 `_c` | 长期产品化、强地理切片诉求 | 有 3857 端点时首选 |

---

## 5. 方案 A 详细设计（P1 实验闭环，默认关闭、可回退）

### 5.1 数据流

```text
引擎按墨卡托选出 (z,x,y)
   │  （2D renderLayer / 3D quadtree 完全不变）
   ▼
TileLoader.request(z,x,y)
   │  该层 scheme==Geographic4490？
   ├─ 否：旧路径（读缓存→直连 {z}{x}{y}）
   └─ 是：地理源适配
          1) 墨卡托瓦四至 tileBounds(z,x,y)
          2) 选服务级 s（赤道近 1:1：s=clamp(z-1,0,maxGeoZ)）
          3) intersectingGeoTileRange 得覆盖它的 geo 瓦集合 sx∈,sy∈
          4) 逐个取 geo 瓦字节：先查“原始地理瓦缓存”，未命中再 HTTP 下载（复用 HttpClient/退避/并发）
          5) 解码为 MEM，按各自线性经纬度拼成内存 VRT（GDAL，全局互斥）
          6) warp/裁剪到墨卡托目标四至，输出 256² PNG
          7) 当作该 (z,x,y) 的结果写回“图层正常缓存”并交付纹理
```

- 最终产物**就是引擎今天期望的那块墨卡托 256² 瓦**，所以 2D 轴对齐方块与 3D 墨卡托 `GlobeTile` 几何都天然正确，UV 无需改。
- **两级缓存**：①原始地理服务瓦（方案分目录，供相邻墨卡托瓦复用，避免重复下载）；②warp 后的墨卡托结果（图层现有缓存，键仍是 `(z,x,y)`，命中即零 warp 零网络）。
- 行序：地理瓦下载 URL 用参数 `tmsY` 决定是否 `reverseY`（XYZ 天地图 `_c` 不翻，TMS 翻），与地形 `{reverseY}` 同口径。
- 服务级映射 `s=z-1` 的依据：地理瓦经度跨 `360/2^(s+1)`，墨卡托瓦赤道经度跨 `360/2^z`，令相等得 `s=z-1`；高纬墨卡托瓦线性纬度跨得更小，由交集范围+warp 做轻微降采样，结果落盘。
  - @Jin 2026-10-31 落地修正：上述 s=z-1 只在赤道经度方向 1:1，未考虑地理像元地面 cosφ×1 的扁率，中纬度会造成约 1/cosφ（34°≈1.20 倍）的垂直整体拉伸（注记字明显）；最终实现改用 **s=z**，详见实现详解 §5.2。

### 5.2 API（向后兼容，新增可选配置，Android 同构预埋）

```ts
// @Jin 新增；不传 options 时与旧签名完全等价（默认 3857）
export interface TileLayerOptions {
  crs?: 'EPSG:3857' | 'EPSG:4490' | 'EPSG:4326'; // 4326/4490 都按地理切片处理
  tileScheme?: 'mercator' | 'geographic';        // 缺省随 crs：3857→mercator，4490/4326→geographic
  tileSize?: number;                             // 缺省 256
  tmsY?: boolean;                                // 地理切片行序；缺省 false(XYZ，y=0 在北)
  rootCols?: number;                             // 缺省 geographic=2 / mercator=1
  rootRows?: number;                             // 缺省 geographic=1 / mercator=1
}
addTileLayer(cacheDir, urlTemplate, maxLevel, overlay?, options?: TileLayerOptions): void
```

- NAPI 增加一个 `nativeAddTileLayerWithOptions`（或在原函数尾部接一个可选 JSON string，默认空＝旧行为），**不动旧函数签名**，保证作者原代码与既有宿主零破坏。
- Demo 增加一个**实验开关/实验图源**（例如“天地图 4490 地理切片（实验）”），影像与注记必须**同时**切到地理方案（`img_c` / `cia_c`），避免 3857 影像叠 4490 注记错位。

### 5.3 改动文件（预估，P1 仅在方案 A 范围）

- `layer/TileLoader.{h,cpp}`：新增 scheme/选项字段与“地理源适配”分支；复用 `terrain/CesiumTerrain.cpp` 的地理换算（建议把 `cesiumGeoTileBounds`/交集范围抽成公共 `globe/GeoTileScheme.{h,cpp}`，`@Jin` 标注，地形与栅格共用）。
- 新增 `raster/GeoSourceWarper.{h,cpp}`（或扩展 `RasterRenderer` 支持“内存 MEM 拼贴→warp”，与本地文件路径并列）。
- `core/GlobeEngine.{h,cpp}`、`globecore-harmonyos/.../napi_globe_engine.cpp`、`NativeLib.ets`、`NativeMapView.ets`、`index.d.ts`：透传 options。
- 缓存目录由调用方按方案区分（Demo 用 `.../tiles/tianditu-c4490-img`），native 侧再校验一层，避免误传同目录。
- **3D 几何、2D TileMatrix、矢量/地形/拾取：不改。**

### 5.4 风险与缓解

- GDAL 全局互斥可能拖慢首屏批量 warp：沿用中心优先投递 + 限制地理源 worker 数；warp 结果落盘后浏览零成本；必要时把“拼接+warp”做成有界队列并入现有视口代际取消。
- 一块墨卡托瓦在高纬可能覆盖多块地理瓦：限定服务级与最大覆盖块数，超块用上级粗瓦或降级，防止请求放大。
- 天地图配额：原始地理瓦独立缓存 + 复用，可显著减少重复请求；仍需用户关注日配额。
- 若 GDAL 内存拼贴/warp 在真机表现不佳，作为回退点，可仅对 3D 先开（3D 本来就经纬度采样），2D 后续。

---

## 6. 方案 B 预留（产品化，P2 以后再评估）

- 新增内部 `enum class RasterTileScheme { WebMercatorXyz, GeographicEpsg4490 }`，挂到 `TileLayer`。
- 3D：每图层独立地理叶集（2×1 根、线性纬度、`texelSize` 改按经纬度/实地宽度），`tileGlobeMesh_` 全局键加入 scheme 命名空间或改为分层网格缓存；图层纹理与地形几何解耦（向 Cesium「每影像层独立 imagery quadtree 贴到地形根瓦」靠拢）。
- 2D：保持墨卡托内部坐标，用 16×16/32×32 加密的非线性变形网格把地理瓦嵌进墨卡托平面（顶点 lon/lat→Mercator），矢量/overlay 不动；或另起地理平面相机（成本更高，不建议）。
- 同步：视锥裁剪、缝隙外扩、父瓦兜底、split LRU、缓存隔离、拾取都要按方案核对。改动深，需完整回归，故放在方案 A 验证之后。

---

## 7. P0 待用户/服务方确认的输入

1. 目标服务的 **WMTS GetCapabilities 或可直接访问的瓦片样例 URL**（确认 TileMatrixSet、tileSize、最大级、行序 XYZ/TMS、是否与天地图 `_c` 同构）。
2. 天地图 token 是否有 `img_c` / `cia_c`（4326 地理切片）权限；或提供自有 4490 WMTS。
3. 是否只要求 HarmonyOS NEXT（Android 仅 API 预埋）。
4. 可接受的实验形态：Demo 加“4490 地理切片（实验）”图源开关，默认关闭。

### 真机验收要点（实施后）

- z0 是否 2×1 根、全球无海陆错位；中国轮廓/POI/省界与地形对齐。
- 2D 与 3D 分别验证；太白山/华山/珠峰近景对齐；高纬拉伸/极区裁切。
- 影像与注记同方案叠合；切图源无串缓存（缓存目录隔离）。
- 与三维地形、矢量 overlay 对齐；缩放/旋转无缝隙、无黑块。
- 缓存二次进入零网络/秒开；首屏 warp 帧率可接受。

---

## 8. 分期与任务清单

- [x] **P0 评估（本次，仅文档）**：代码走读、方案对比、改造点、风险与验收口径。
- [ ] **P0+ 服务确认**：拿到 4490/4326 WMTS 元数据与可测 URL，核对行序/层级/tileSize。
- [ ] **P1 Harmony 实验闭环（方案 A）**：抽公共地理切片数学；TileLoader 地理源适配 + 两级缓存；options 透传；Demo 实验图源；默认关闭、可回退。
- [ ] **P1 真机验收**：§7 全部通过后再决定去留。
- [ ] **P2（条件触发）**：若方案 A 对齐/性能不足，再立项方案 B 原生 per-layer TileScheme。
- [ ] **P3 产品化**：Harmony/Android API 对齐、自定义 tileSize/TileMatrixSet/TMS、更新 Android API HTML 与 README、自动化与回归。

---

## 9. 对标 Mars3D / Cesium：能否借鉴

> @Jin 本节应要求补充：对照 Cesium 与 Mars3D 对“地理切片（EPSG:4326/4490）影像”的处理机制，判断本引擎可借鉴的部分与代价。结论先行：**Mars3D 本质是 Cesium 的二三维封装，自身不做切片数学；它的 4490 能力＝Cesium 的能力。** Cesium 的做法正好印证本方案，并给出了“比 GDAL CPU 重投影更正统”的目标架构。

### 9.1 Cesium 的关键机制

1. **`TilingScheme` 抽象（影像层各自携带）**
   - `GeographicTilingScheme`：根 **2×1**，`X=2^(z+1)`、`Y=2^z`，纬度**线性**（plate carrée），配 `GeographicProjection`；
   - `WebMercatorTilingScheme`：根 **1×1**，±85.0511° 截断，配 `WebMercatorProjection`；
   - 每个 `ImageryProvider/ImageryLayer` 自带一个 `tilingScheme`，与地形/地表的方案**可以不同**。这正是我们提议给 `TileLayer` 加 `crs/tileScheme` 的出处。
2. **影像方案与地表（地形）方案解耦，按“地理 UV 贴花（drape）”叠加**
   - 地表网格由地形 Provider 的 TilingScheme 决定（EllipsoidTerrainProvider 默认墨卡托，Cesium World Terrain 用地理方案）；
   - 每个地表瓦根据其**地理矩形**找出相交的若干影像瓦；网格**顶点用自身经纬度反算到影像瓦的本地 UV**（影像链 `ImageryState` + 纹理坐标），着色器采样；
   - 一个地表瓦可对应**多张**影像瓦（纹理图集/多纹理 + 祖先兜底），缺失的用父级；
   - 跨方案**不做 CPU/GDAL 重投影**——靠密集地形网格顶点的地理 UV 把任意方案的影像“贴”上去；早期版本另有 FBO `ReprojectWebMercator` 着色器做一次 GPU 重投影作为兼容通路。
3. **基准（datum）不做实时转换**：椭球可换（CGCS2000：a=6378137、f=1/298.257222101），实践中直接用 `Ellipsoid.WGS84`，亚米级差异可视化忽略——与本文 §2 判断一致。
4. **2D 默认等经纬度圆柱投影（plate carrée）**：地理切片在 Cesium 的 2D 里反而是“原生对齐”，墨卡托影像才需要重投影。

### 9.2 Mars3D 的做法

- Mars3D 基于 Cesium 封装，没有独立切片实现。接入天地图/政务 4490 服务时，只是按服务选 `tilingScheme`：`img_w/cia_w` 用默认墨卡托；`img_c/cia_c` 或 EPSG:4326/4490 WMTS 用 `new GeographicTilingScheme()`（2×1、线性纬度），行序/层级通过 WMTS 参数或 URL 处理。
- 我们地形侧的 Mars3D layer.json 也已验证是 **EPSG:4326 + TMS + quantized-mesh**（见 Demo `CesiumTerrainProvider.ets` 注释），与 Cesium 规范一致。

### 9.3 与本引擎逐项映射/差距

| Cesium 概念 | GlobeCore 现状（代码位置） | 差距判定 |
|---|---|---|
| `GeographicTilingScheme`（2×1、线性） | 栅格只有墨卡托 `TileMatrix`(2^z)；**地形侧已有同款** `cesiumGeoTileBounds()`(2^(z+1)/2^z) 但未给栅格复用 | 小：抽公共 `GeoTileScheme` 即可 |
| 每影像层独立 tilingScheme | `TileLayer` 无 scheme 字段；选瓦是全场景唯一墨卡托四叉树（`Tessellator/GeographicQuadtree`） | 中：加字段容易，选瓦解耦难 |
| 地表瓦↔多影像瓦 + 地理 UV 链 | 3D 顶点单 UV（`[pos3,uv,rgba]` 共 9 浮点，见 `Renderer.h:203`），逐瓦只绑**一张同键**纹理；`tileGlobeMesh_` 全局、仅按 `(z,x,y)` 键控、**不分层** | **大：这是最主要差距**，需分层 UV/图集或多纹理 + 祖先链 |
| GPU 重投影/地理 drape | 无；本地栅格走 **CPU GDAL** warp 落盘（`RasterRenderer::renderTile`） | 可新增“GPU warp”中间方案 |
| 2D 等经纬度原生 | 本引擎 2D 是**墨卡托平面 + 轴对齐方块**（`renderLayer`） | 地理瓦需 warp 或加密变形网格嵌入 |

### 9.4 可借鉴性结论

1. **可直接借（低成本，建议照做）**：`TilingScheme` 抽象与 2×1 线性数学——而且我们地形里已有同一份公式，应抽成栅格/地形共用的 `GeoTileScheme`；公开 API 字段（`crs/tilingScheme/tileSize/tmsY/rootCols/rootRows`）与 Cesium/Mars3D 惯例对齐；4490 不做基准转换，仅复用 WGS84 椭球。
2. **作为目标架构借鉴（＝方案 B 的“正解”）**：影像方案与地表解耦、按顶点地理 UV 贴花。我们 3D 网格顶点本来就是**由经纬度生成**（`buildGlobeTileMeshLocal`），天然具备“逐顶点算地理 UV”的前提；真正成本在于“一个地表瓦对多张影像瓦”的图集/多纹理、祖先兜底，以及把全局单 UV、单键网格缓存改为按方案分层。属较大重构，宜在方案 A 验证诉求确实存在后再立项。
3. **启发新增方案 A′（GPU warp，介于 A 与 B 之间）**：把方案 A 的 **CPU/GDAL** 重投影改为 **GPU 顶点网格 warp**——目标瓦网格加密（2D 用 16×16/32×32 变形网格，3D 复用 24×24 地形网格），逐顶点把经纬度换算成地理源瓦片 UV，配合小图集或 FBO 摊平等经纬↔墨卡托差异；产物仍以墨卡托 `(z,x,y)` 写入现有纹理缓存。2D/3D 通用、**不动选瓦与缓存键**、回退容易，规避真机 GDAL 全局互斥带来的首屏 CPU 尖峰。
4. **不照搬**：Cesium 的 2D 默认等经纬度，与我们墨卡托平面不同，不能假设地理瓦在 2D 原生对齐；其成熟的多纹理影像链体量远超本最小引擎，一期不必整体移植。

### 9.5 修订后的路线

- **P1（不变，最低风险）**：方案 A，内建“瓦片代理式”单瓦重投影（思路等同 MapProxy / TerriaJS 的服务侧重投影），默认关闭、可回退，先验证 4490 服务能正确上图、对齐与缓存。
- **P1.5（新增，按需）**：若真机首屏 GDAL warp CPU/帧率不达标，优先升级为 **方案 A′（GPU warp）**，而不是直接上方案 B。
- **P2（目标架构）**：借鉴 Cesium 实现方案 B——每影像层独立 `TilingScheme` + 地理 UV drape + 多影像瓦/祖先链 + 分层网格缓存。

> @Jin 备注：本次新增本评估文档（含 §9 对标 Cesium/Mars3D），未修改引擎代码，未提交。确认方案后再进入 P1 实现。

---

## 10. @Jin P1 实施与真机验证结果（2026-10-30，分支 feature/epsg-4490）

> @Jin 本节为落地记录；代码已实现并在鸿蒙真机通过技术验证，等待用户最终人工确认后再提交。
>
> @Jin 改造思路、分层改动清单，以及重投影（相交地理瓦映射 / 等经纬拼贴 / 预乘 alpha / 双线性采样 / 两级缓存 / 自研 PNG 编码）的逐步算法，见同目录 [epsg-4490-raster-tile-implementation.md](./epsg-4490-raster-tile-implementation.md)。

- @Jin 按“方案 A（图层级适配、默认关闭、可回退）”实施，未改动选瓦四叉树与 3D 几何：
  - 新增 `render/PngEncoder.{h,cpp}`：仅依赖 zlib 的内存 RGBA→PNG，GDAL 无关，`GLOBECORE_LITE=ON` 可用；两套 CMake（Android/Harmony）均登记。
  - `layer/TileLoader`：新增 `setGeographicSource()` 与 worker 内地理源分支；原始地理瓦独立缓存 `<cache>-geo-raw`，按 `cols=2^(s+1)/rows=2^s`、`s=z` 取相交服务瓦（注：落地时由 s=z-1 改为 s=z，原因见实现详解 §5.2——s=z-1 会导致垂直 1/cos(lat) 整体拉高），等经纬拼贴+双线性重采样为墨卡托 256² PNG，结果仍以 `(z,x,y)` 落常规缓存。
  - `layer/TileLayer.h` 增 `TileLayerOptions{crs,tmsY,tileMatrixOffset}`；`GlobeEngine` 增 5 参重载（4 参版转发，行为不变）。
  - Harmony：NAPI `nativeAddTileLayerWithOptions` + `NativeLib/NativeMapView.addTileLayerWithOptions` + `index.d.ts`。Android/JNI 公开 API 本次未改（后续预埋）。
  - Demo：天地图改 `img_c/cia_c`，切天地图时 `crs=EPSG:4326、tmsY=false、tileMatrixOffset=1`，独立缓存目录 `tianditu-c4326-{img,cia}`；默认启动图源仍 ArcGIS。
- @Jin 真机（`5KLBB25911200313`，LITE 包）日志：`geographic source ... on=1 maxGeoLevel0=17 tmsY=0 matrixOffset=1`；影像/注记两层均 `geo4490 reproject`，首屏 `online=1`、随后 `online=0`（两级缓存生效），无失败/尺寸异常/映射超限。
- @Jin 画面人工核对（脚本截图）：3D 全球中国轮廓正确；2D 放大至地图 z4/瓦片 z7 城市与道路中文注记贴合；太白山开地形飞行后影像随地形起伏贴合。
- @Jin 2026-10-30 追加「右上角图源菜单手动切换」冷缓存全链路复核（卸载重装清数据后）：日志显示初始 ArcGIS 两层 `crs=EPSG:3857 geo=0`，菜单切天地图后影像/注记两层均重建为 `crs=EPSG:4326 geo=1`；首屏 54 次 `geo4490 reproject online=1`、0 失败/0 尺寸异常/0 映射超限，原始地理瓦复用时出现 `online=0`，二次浏览命中墨卡托结果缓存不再重采样；3D 全球与 2D 放大截图均正常。
- @Jin 2026-10-30 顺带修复 Demo UI 遮挡（非引擎改动，Demo 目录不在本仓库）：顶部 LOD 调试 Column 宽度 `100%` 且 zIndex=31，透明空白区吞掉右上图源按钮（zIndex=21）上半部触摸，真机上只有底部约 18px 窄条可点；对该 Column 加 `.hitTestBehavior(HitTestMode.Transparent)`（空白透传、LOD 子按钮照常响应），重新构建安装后点图源按钮中心即可展开菜单。
- @Jin 待办/边界：Android（JNI/Kotlin）`TileLayerOptions` 重载预埋；高纬近景若觉线性经纬拉伸，再评估 A′（GPU warp）/B（Cesium 式 per-layer scheme）；等待用户确认后提交并按需更新 Android API HTML（当前未新增 Android 公开 API，故该文档无需改动）。
