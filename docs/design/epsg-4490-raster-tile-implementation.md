# EPSG:4326/4490 地理切片栅格瓦片 —— P1 实现详解（方案 A）

> @Jin 本文是 [`epsg-4490-raster-tile-proposal.md`](./epsg-4490-raster-tile-proposal.md)（可行性评估与方案选型）的**落地实现篇**，重点记录「为什么这样改、改了什么、用什么技术、重投影具体怎么做」。
>
> - 分支：`feature/epsg-4490`（基于 main `9cb7f00`）
> - 实施/真机验证日期：2026-10-30（鸿蒙 NEXT 平板 `5KLBB25911200313`，LITE 包）
> - 约束：实验性能力、**默认关闭、可回退**；新增/修改代码注释均带 `@Jin`
> - 适用：天地图 `img_c/cia_c` 等 **EPSG:4326/CGCS2000(4490) 地理切片（OGC 4326 TileMatrixSet）** 在线栅格瓦片

---

## 1. 要解决的问题

引擎内部全场景只有一套 **Web 墨卡托（EPSG:3857）切片四叉树**：z0 全球 1×1，纬度按墨卡托投影，纹理键、缓存、URL、2D 方块、3D 地球网格全部基于同一套 `(z,x,y)`。

而天地图 `img_c/cia_c` 是**地理等经纬切片（EPSG:4326 TileMatrixSet）**：

- z0 全球 **2×1**（2 列 1 行），每块跨 180°×180°；
- 纬度**线性**（不是墨卡托），低纬度面积分辨率被“压扁”、高纬被拉伸；
- 天地图的层级标识 `TILEMATRIX` **从 1 开始**（不是 0）；
- 行序 XYZ（y=0 在北），但同类服务也可能是 TMS（y=0 在南）。

直接把 `img_c` 的 URL 塞进原有 `(z,x,y)` 会出现：根块数不匹配（2×1 vs 1×1）、纬度映射不一致、层级错位一格，结果是**错位、重复、裁切错误**。

> 基准澄清：4490(CGCS2000) 与 4326(WGS84) 椭球差异亚米级，栅格可视化按同一套等经纬切片处理即可，**不需要基准变换**；真正的工作量在“切片矩阵不同”。

---

## 2. 总体思路：瓦片代理式重投影（方案 A）

### 2.1 一句话概括

> **不动引擎的选瓦四叉树和 3D 几何，只在取图层面对“地理源”多做一步：对每一个引擎想要的墨卡托瓦，反向找出与之相交的若干地理服务瓦，把它们下载/读缓存后，在 CPU 上重采样拼成一张标准 256×256 墨卡托瓦 PNG，再交回原有链路。**

这本质上是 **MapProxy / TerriaJS / GeoServer 的“服务侧/代理侧重投影瓦片（reprojected tile grid）”** 的做法，只不过重投影发生在端内 `TileLoader`，产物写进原有瓦片缓存。

### 2.2 为什么选它（而不是改坐标系）

| 维度 | 方案 A：图层级适配（本次） | 方案 B：原生 per-layer TilingScheme |
|---|---|---|
| 改动面 | 只改取图层，选瓦/相机/矢量/拾取/3D 网格**零改动** | 每层独立切片方案 + 地理 UV drape + 多纹理/图集，重构渲染与缓存 |
| 风险 | 低，默认关闭，回退＝不调新接口 | 高，触及全局单 UV/单键网格缓存 |
| 缓存 | 产物仍按墨卡托 `(z,x,y)`，**完全复用**现有磁盘缓存/去重/解码/GL 上传 | 需要分层键、分层网格 |
| 2D/3D | 通用（产物就是一张普通墨卡托瓦） | 需分别处理 2D 等经纬与 3D drape |
| 代价 | CPU 重采样 + 高纬/瓦内近似（见 §7） | 实现复杂、周期长 |

引擎里本就有两块可直接借鉴的先例：① 三维地形本身是 EPSG:4326 quantized-mesh，已用“墨卡托瓦→相交地理瓦”的映射在真机跑通；② 本地栅格 `RasterRenderer::renderTile()` 已按“单瓦四至重投影→落盘缓存”工作。方案 A 把这两条经验合到在线图层。

### 2.3 关键设计原则

1. **默认关闭、按图层开启**：只有显式传 `crs=EPSG:4326/4490` 的图层走适配；旧 4 参 API 行为不变。
2. **几何只读不改**：适配层只产字节，不知道也不关心 GL/网格。
3. **两级缓存、结果归一**：原始地理瓦缓存一份（可被多个墨卡托瓦复用），重投影后的墨卡托瓦再按 `(z,x,y)` 落常规缓存；二次浏览零网络零重采样。
4. **失败优雅降级**：任一源瓦缺失/解码失败/映射数超限，本瓦返回失败，由既有“祖先瓦兜底”机制显示，不产生黑屏/坏图。
5. **GDAL 无关**：鸿蒙 LITE 包不编 GDAL 的 RasterRenderer，重采样自己做，PNG 自己编码（只依赖 zlib）。

---

## 3. 数据流与在管线中的位置

普通墨卡托图层 worker 顺序：`磁盘缓存 → 本地栅格(GDAL) → 联网直连`。

地理源图层在中间插入一步（`TileLoader.cpp` worker）：

```text
引擎请求墨卡托瓦 (z,x,y)
        │
        ▼
①读常规结果缓存 cache_ (z,x,y)  ── 命中 ──▶ 直接交付（与普通层完全一致）
        │未命中
        ▼
②地理源分支 generateGeographicTile()
     · 求目标墨卡托瓦地理四至（角点墨卡托反投影，瓦内线性经纬）
     · 选服务级 s=z，求相交地理瓦范围 [gx0,gx1]×[gy0,gy1]
     · 逐块：先读原始瓦缓存 geoRawCache_ (matrixZ,gx,gy)，未命中再 HTTP 下载并落盘
     · 解码 256²，按等经纬贴入拼贴画布（预乘 alpha）
     · 逐输出像素反算等经纬坐标 → 双线性采样 → 去预乘
     · 编码成 256² PNG
        │
        ▼
③把 PNG 按墨卡托 (z,x,y) 写回常规缓存 cache_，交付
        │
        ▼
后续与普通层一致：looksLikeImage 魔数校验 → ImageDecoder 解码 RGBA → GL 线程 glTexImage2D 上传
```

普通联网门加了 `&& !geoScheme` 条件，保证地理源**不会**再误按墨卡托直连一次。

---

## 4. 切片矩阵与参数定义

### 4.1 两种矩阵的数学

墨卡托瓦（引擎目标，标准 XYZ，`tileBounds()`）：归一化世界坐标 `[0,1]²`，角点经纬度由墨卡托正反算：

```text
n = 2^z
世界坐标 (wx,wy) = (x/n, y/n)
lon = wx*360 - 180
lat = atan(sinh(π*(1 - 2*wy))) * 180/π     // 墨卡托反投影；y=0 在北
```

地理瓦（服务源，0 基 s，根 2×1，线性纬度，`geoTileBoundsXYZ()`）：

```text
cols = 2^(s+1)     rows = 2^s
lonMin = -180 + 360 * x     / cols
lonMax = -180 + 360 * (x+1) / cols
latMax =  90  - 180 * y     / rows        // XYZ：y=0 在北
latMin =  90  - 180 * (y+1) / rows
```

### 4.2 三个每图层参数（`TileLayerOptions`）

| 字段 | 含义 | 天地图 `img_c/cia_c` 取值 |
|---|---|---|
| `crs` | `""`/`EPSG:3857` 走旧逻辑；`EPSG:4326`/`EPSG:4490` 启用地理源 | `EPSG:4326` |
| `tmsY` | 行序：`false`=XYZ（北在上），`true`=TMS（南在上，需翻转） | `false` |
| `tileMatrixOffset` | 服务 `TILEMATRIX` 相对“0 基 s”的偏移；请求 URL 的 `z = s + offset` | `1`（天地图根 2×1 记为 TILEMATRIX=1） |

服务级 0 基上限：`maxGeoLevel0 = max(0, maxLevel - offset)`。Demo `maxLevel=18、offset=1 → maxGeoLevel0=17`。

URL 行序在请求时换算：`urlY = tmsY ? (rows-1-gy) : gy`。

---

## 5. 重投影算法详解（核心）

实现：`TileLoader::generateGeographicTile(z,x,y,outPng,anySrcNetwork)`。下面逐步拆解。

### 5.1 步骤 1：目标墨卡托瓦的地理四至

```cpp
tileBounds(z,x,y, tLonMin,tLatMin,tLonMax,tLatMax);   // 角点纬度为墨卡托反投影
```

瓦片四角与内部像素均按真实墨卡托处理（见 5.6）；早期实现曾在瓦内按线性纬度排像素，已于 2026-10-31 修正为逐行墨卡托反投影。

### 5.2 步骤 2：选服务级 s（关键修正：s=z，不是 z-1）

**先看一个容易踩的坑（2026-10-31 真机发现注记上下拉伸后修正）。** 地理切片的像元在地面上不是正方形：经度方向每像元 `360/(2^(s+1)·256)` 度、纬度方向每像元 `180/(2^s·256)` 度，对应地面尺寸是 **cosφ（宽）× 1（高）的扁矩形**；而目标墨卡托瓦像元在地面是正方形（经度 1 份、纬度 1/cosφ 份，地面等大）。

- 若按赤道经度 1:1 选 **s=z-1**：水平源/需求正好 1 像元，但垂直方向源的地面分辨率只有需求的 **cosφ 倍**（34°≈0.83），输出时垂直被整体放大 **1/cosφ≈1.20 倍**。这个倍率**与层级无关**，所以任何缩放下中文注记都会被整体拉高（影像是自然纹理几乎无感）。
- 选 **s=z**：每个输出像元从源的约 `2（宽）× 2cosφ（高）` 个像元重采样，水平 2:1、垂直 2cosφ:1 的不同压缩率恰好抵消源像元自身的 cosφ 扁率，得到**地面各向同性**的墨卡托瓦，与原生 EPSG:3857 注记完全同比例。

```cpp
const int s = std::max(0, std::min(maxS, z));   // @Jin s=z 修正垂直 1/cosφ 整体拉高
cols = 2^(s+1);  rows = 2^s;
```

> **@Jin 2026-10-31 重要补充：`s=z` 的“各向同性”只对连续影像严格成立，对天地图 `cia_c` 栅格注记不完全成立。**
>
> - 影像（`img_c`）是连续自然纹理，没有“按瓦片像素定义的符号”，warp 到墨卡托（保角投影）确实恢复真实形状，地面各向同性，结论成立。
> - 注记（`cia_c`）是服务端**按等经纬度瓦的像素方块**栅格化的文字/符号。c 瓦一个像元在地面是 `cosφ(宽)×1(高)` 的扁矩形，天地图把方块字直接画在这个扁矩形像元上，于是“字”本身在地面就是南北 `1/cosφ`（太白山 33.7°≈1.20）倍的椭圆。warp 是地理正确的（墨卡托保角），会**如实**把这个扁字显示成纵向拉长，无法通过重投影把它“纠正”回方块——因为这不是映射错误，正是地理真实形状。
> - 因此本方案对 **c 栅格注记存在固有残余**：要么 `s=z` 字形拉长较小但**字号偏小**（服务在更细矩阵重新制图，字更密更小），要么取粗一级把字号调大但**纵向 1.20 倍拉长更明显**。两者是服务制图规则与切片方案决定的权衡，不是本引擎 warp 能同时消除的。

代价是多取一级地理瓦（赤道附近一个墨卡托瓦约交 2×1 个地理瓦；高纬最多到 2×约(2/cosφ)，受 §5.3 的 64 块上限保护，引擎有效纬度 ±85.05° 内不超限）。天地图 c 叠加 `offset=1`，请求 TILEMATRIX=s+1=z+1。

> 边界：天地图 c 最高矩阵 18（0 基 s=17），引擎 z=18 时 s 被钳到 17，该单一层级垂直会略有 1/cosφ 放大；日常层级不受影响。

### 5.3 步骤 3：求相交地理瓦范围

把目标四至映射到连续地理瓦下标，再向外交整（含 `-1e-9` 边界容差，避免共边漏算一列）：

```cpp
gx0 = floor((tLonMin + 180)/360 * cols)
gx1 = floor((tLonMax + 180)/360 * cols - eps)
gy0 = floor((90 - tLatMax)/180 * rows)      // y=0 在北
gy1 = floor((90 - tLatMin)/180 * rows - eps)
// 全部 clamp 到 [0,cols-1]/[0,rows-1]
```

**数量保护**：`spanX*spanY > 64（kMaxGeoSourceTiles）` 直接放弃本瓦（记日志、祖先兜底）。正常近赤道一个墨卡托瓦与约 1×1～2×2 个地理瓦相交；高纬或异常请求才可能放大，上限防止请求量/内存被异常放大。

### 5.4 步骤 4：取原始地理瓦（原始瓦缓存 + 联网）

```text
for gy, gx in 相交矩形:
    matrixZ = s + offset          // 天地图=1 基
    urlY    = tmsY ? rows-1-gy : gy
    ① geoRawCache_.readTile(matrixZ,gx,gy)  —— 命中且 looksLikeImage → 直接用
    ② 未命中：HttpClient::get(formatTileUrl(...)) → 魔数校验 → geoRawCache_.writeTile(...)
       anySrcNetwork=true
    ③ 仍失败/非图（限流 XML 等）→ 本瓦整体失败，返回（祖先兜底）
    ④ ImageDecoder::decodeRGBA，且必须为 256×256，否则失败
```

- 原始瓦缓存目录：`<结果缓存目录>-geo-raw`，**与结果缓存物理隔离**，两套切片编号绝不串图。
- `looksLikeImage` 魔数校验把“HTTP 200 但返回限流 XML/HTML”的响应挡在缓存之外（沿用普通层防污染策略）。
- `formatTileUrl()` 与普通层共用，支持 `{z}/{x}/{y}` 与 `{rand=0,1,...}` 子域轮询，保证行为一致。

### 5.5 步骤 5：等经纬拼贴（Mosaic）+ 预乘 Alpha

申请一块 `spanX*256 × spanY*256` 的 RGBA 画布，缺失处保持全 0（透明），原点取相交矩形西北角 `(gx0,gy0)`：

```cpp
originLon  = -180 + 360*gx0/cols;     // 拼贴西边界
originLatN =  90  - 180*gy0/rows;     // 拼贴北边界
```

每张源瓦按其下标整块 memcpy 到对应位置（`ox=(gx-gx0)*256, oy=(gy-gy0)*256`），同时做**预乘 alpha**：

```cpp
dst.RGB = src.RGB * a/255;  dst.A = a;
```

预乘后再做双线性插值，透明注记 PNG 的边缘才不会插值出“黑边”。

### 5.6 步骤 6：逐输出像素反向采样 + 双线性（重投影本体）

这是“等经纬源图 → 墨卡托目标瓦”的关键。对目标瓦每个输出像素，取其**像元中心**：经度方向墨卡托本身线性（按瓦内线性经度即可）；纬度方向则把该像素行的归一化墨卡托坐标 `wy=(y+(py+0.5)/256)/2^z` **逐行反投影成精确纬度**（不再在南北角纬度间线性插值）：

```cpp
invTilesZ = 1.0 / 2^z
for py in 0..255:
    wy  = (y + (py+0.5)/256) * invTilesZ
    lat = mercatorInverseLat(wy)          // 逐行精确墨卡托反投影，北在上
    fy  = (originLatN - lat) * pixelsPerLat - 0.5
    for px in 0..255:
        lon = tLonMin + (px+0.5)/256 * (tLonMax-tLonMin)   // 经度线性
        fx  = (lon - originLon) * pixelsPerLon - 0.5
```

其中源拼贴图的每度像素密度为常数（等经纬）：

```cpp
pixelsPerLon = cols*256/360;
pixelsPerLat = rows*256/180;
// -0.5 是“像元中心 ↔ 像素坐标”的半像素校正
```

得到浮点源坐标 `(fx,fy)` 后做**双线性插值**（边界 clamp）：

```cpp
top = p00*(1-t) + p10*t;  bot = p01*(1-t)+p11*t;
v   = top*(1-ty) + bot*ty;     // 对 R,G,B,A 四个预乘通道分别做
```

输出再**去预乘**还原 straight alpha：

```cpp
A = pm.A;
if (A==0) RGB=0;                       // 全透像素清掉垃圾色
else      RGB = pm.RGB*255/A;          // 去预乘
```

> **精度（2026-10-31 修正）**：输出瓦每个像素行的纬度都由真实墨卡托反投影得到，因此结果 PNG 的纬向像素密度与引擎纹理 UV（2D 轴对齐瓦、3D 网格线性 UV）**严格一致，瓦内垂直尺度误差为 0**，瓦片四角与内部都不错位、不拉伸。早期实现“瓦内 256 行纬度线性排布”在中纬度低层级会造成约 ±1%~3% 的整块垂直各向异性，天地图 c 的中文注记字被肉眼看出轻微上下拉伸（3857 原生注记无此问题），已由逐行反投影消除；每瓦仅多 256 次 `atan·sinh`，CPU 开销可忽略。经度方向墨卡托即线性，无需特殊处理。

### 5.7 步骤 7：编码 PNG，回归普通链路

重采样得到 256×256 straight-alpha RGBA，用自研 `PngEncoder::encodeRGBA()` 编成 PNG 字节返回；worker 随即按墨卡托 `(z,x,y)` 写入常规缓存 `cache_`。此后这张瓦与普通在线瓦**没有任何区别**：魔数校验、解码、去重、GL 上传、祖先兜底全部复用。

### 5.8 为什么不用 GDAL，而是自己重采样 + 自研 PNG 编码器

- 鸿蒙 LITE 构建（`GLOBECORE_LITE=ON`）**不编译 GDAL 版 `RasterRenderer`**，且它在匿名命名空间、无法直接复用；
- GDAL warp 在真机有全局锁/首屏 CPU 尖峰隐患，对“每瓦一次、高频”的在线路径偏重；
- 等经纬 ↔ 瓦内线性这一映射是**闭式公式**（§5.6），不需要通用投影库，手写双线性开销极小（256²×4 通道）；
- 输出 PNG 只为复用“字节缓存→解码”链路，于是新增 `render/PngEncoder`：手写 PNG 容器（IHDR/IDAT/IEND + CRC32），原始扫描线逐行 filter=None，zlib `compress2(Z_BEST_SPEED)` 压缩——**只依赖 zlib**（全平台/Android/Harmony LITE 都已链接），已登记进两套 CMake。

---

## 6. 缓存设计（两级）

| 层级 | 目录（Demo 示例） | 键 | 内容 | 命中效果 |
|---|---|---|---|---|
| 原始地理瓦缓存 | `…/tianditu-c4326-img-geo-raw`、`…-cia-geo-raw` | `(matrixZ,gx,gy)` 1 基矩阵 | 服务原始 PNG（256²） | 多个墨卡托瓦可共享同一源瓦，**只下载一次** |
| 结果（墨卡托瓦）缓存 | `…/tianditu-c4326-img`、`…-cia` | `(z,x,y)` | 重投影后的 256² PNG | 二次浏览**零网络、零重采样** |

- 影像层与注记层各自独立两套缓存，注记透明 PNG 不污染影像。
- 切图源/切回墨卡托源使用不同目录前缀，避免串图（ArcGIS 用 `arcgis/`）。
- 真机实测：冷切首屏 54 瓦 `reproject online=1`；缩放/平移复用源瓦时出现 `online=0`；再次浏览命中结果缓存，直接走第①层、连重投影都不做。

---

## 7. 代码改动清单（分层）

### 7.1 C++ 引擎（主体）

| 文件 | 改动 |
|---|---|
| `layer/TileLoader.{h,cpp}` | 新增 `setGeographicSource()` / `isGeographicSource()` / `generateGeographicTile()`；成员 `geoScheme_/geoUrlTemplate_/geoMaxLevel0Based_/geoTmsY_/geoTileMatrixOffset_/geoRawCache_`；worker 插入地理源分支；普通联网门加 `&& !geoScheme`；新增匿名/文件级工具 `formatTileUrl()`、`geoTileBoundsXYZ()`、常量 `kMaxGeoSourceTiles=64` |
| `render/PngEncoder.{h,cpp}` | **新增**：zlib-only 的内存 RGBA→PNG 编码器 |
| `layer/TileLayer.h` | 新增 `struct TileLayerOptions{ std::string crs; bool tmsY=false; int tileMatrixOffset=0; }` |
| `core/GlobeEngine.{h,cpp}` | 新增 5 参 `addTileLayer(..., const TileLayerOptions&)`；识别 `EPSG:4326/4490` 调 `setGeographicSource(cacheDir+"-geo-raw", …, maxGeoLevel0, tmsY, offset)`；旧 4 参版转发默认选项，行为不变 |
| `CMakeLists.txt`（globecore 与 globecore-harmonyos 各一套） | 登记 `render/PngEncoder.cpp` |

### 7.2 Harmony 绑定（薄透传，无算法）

- `napi_globe_engine.cpp`：新增 `nativeAddTileLayerWithOptions`（8 参，类型显式写 `globecore::TileLayerOptions`），并注册到 NAPI 方法表；
- `NativeLib.ets` / `NativeMapView.ets`：`addTileLayerWithOptions(...)` 转发；
- `types/libglobecore/index.d.ts`：补类型声明。

### 7.3 Demo（验证程序，不在 GlobeCore git 仓库）

- 图源菜单三项：**ArcGIS / 天地图4490(`img_c/cia_c`，geo=1 重投影) / 天地图3857(`img_w/cia_w`，geo=0 墨卡托直连)**，三者缓存目录互不相同（`arcgis` / `tianditu-c4326` / `tianditu-w3857`）；`w` 实测根为 2×2、等价 XYZ z1，`{z}` 直传无需 offset；
- 切天地图时调 `addTileLayerWithOptions(url,maxLevel,overlay,'EPSG:4326',false,1)`，缓存前缀 `tianditu-c4326-v2-{img,cia}`；ArcGIS 与默认启动源不变（仍默认 ArcGIS）；
- 顺带修复右上角图源按钮被顶部 LOD 调试柱（`width('100%')、zIndex 31`）吞触摸的问题：对该柱加 `.hitTestBehavior(HitTestMode.Transparent)`。

### 7.4 Android

仅在 C++ 具备能力；**JNI/Kotlin 公开 API 本次未接**（后续预埋），故 `docs/api/android-api-guide.html` 本轮无需变更。

---

## 8. API 用法

C++：

```cpp
// 旧接口（墨卡托，行为不变）
engine.addTileLayer(cacheDir, urlTemplate, 18, /*overlay*/false);

// 天地图 c（4326 地理切片）：XYZ 行序、TILEMATRIX 偏移 1
TileLayerOptions opt;
opt.crs = "EPSG:4326";
opt.tmsY = false;
opt.tileMatrixOffset = 1;
engine.addTileLayer(cacheDir, urlTemplate_c, 18, /*overlay*/false, opt);
```

Harmony/ArkTS：

```ts
view.addTileLayerWithOptions(url, maxLevel, overlay, 'EPSG:4326', false, 1);
```

回退方式（任一即可完全恢复旧逻辑）：不传 options / crs 留空 / Demo URL 换回 `img_w/cia_w` / 缓存目录换回。

---

## 9. 真机验证记录（2026-10-30）

- 冷启动默认 ArcGIS：日志两层 `crs=EPSG:3857 geo=0`。
- 右上角菜单切天地图（冷缓存，卸载重装清数据）：影像/注记两层均 `geographic source on=1 maxGeoLevel0=17 tmsY=0 matrixOffset=1`、`addTileLayer … crs=EPSG:4326 geo=1`。
- 首屏 **54 次 `geo4490 reproject online=1`，0 失败 / 0 解码尺寸异常 / 0 映射超限**；1 秒量级处理 45 瓦；源瓦复用出现 `online=0`；二次浏览命中结果缓存。
- 画面：3D 全球中国轮廓正确；2D 放大到“地图 z4 / 瓦片 z7”，城市与道路中文注记贴合；太白山开地形飞行后影像随地形起伏贴合。
- @Jin 2026-10-31 拉伸修复定稿：主因是服务级 s=z-1 造成垂直 1/cos(lat) 整体拉高，已改 s=z（见 §5.2）；次因瓦内线性纬度改为逐行墨卡托反投影（见 §5.6）；4490 结果缓存目录升级为 `tianditu-c4326-v2-*`。真机同视角（太白山 z9/瓦片z12、标准 LOD、开地形）4490/3857 截图对比已出，注记等比例待人工最终确认。
- 关键日志关键字（hilog 域 `A08811`，tag `globecore`）：
  `TileLoader geographic source`、`geo4490 reproject`、`地理源映射瓦数超限`、`地理源服务瓦获取失败`、`GlobeEngine addTileLayer … geo=`。

---

## 10. 已知边界与后续路线

### @Jin 2026-10-31 遗留问题：天地图 4490（c 套图）中文注记“字偏小”与“轻微上下拉伸”（暂不修，记录在案）

> 结论先行：这是天地图 **EPSG:4326/4490 的 c 套图（plate carrée）把文字按像素方块栅格化**与 Web 墨卡托/3D 球面显示之间的固有矛盾，当前方案 A（服务瓦→墨卡托瓦重采样）无法同时消除。影像层 `img_c` 无此问题，问题仅在栅格注记层 `cia_c`。

1. **现象一：注记字偏小（全球小比例视角尤其明显，小到看不清）。**
   - 现状：地理源固定按 `s=z` 取图（见 §5.2）。在同样的墨卡托显示尺度下，c 套图 `z+1` 矩阵的注记是服务按更细矩阵重新制图的，字更密、更小；像素对比（西安 z8 墨卡托瓦）：原生 `cia_w` 字高中位约 12px，而 `cia_c` 经 `s=z` warp 后约 8.5px。
   - 曾尝试：给地理源加每图层 `serviceZoomOffset`，对 `cia_c` 传 `-1`（即 `s=z-1`）取粗一级，字高回到约 13~14px，视觉大小接近 `cia_w`。**该实验已按用户意见回退**（认为“放大字体”与“解决拉伸”混为一谈的方案不妥），代码恢复为固定 `s=z`、接口回到 8 参、缓存回到 `tianditu-c4326-v2-*`。

2. **现象二：注记轻微上下（南北）拉伸。**
   - 机理：c 瓦像元地面为 `cosφ×1` 扁矩形，方块字栅格化后在地面已是 `1/cosφ` 南北拉长；warp/墨卡托保角投影如实呈现。合成像素实验（在 c 源拼贴画正方形再 warp，33.7°）：无论 `s=z / z-1 / z-2`，结果高宽比恒约 **1.20**（理论 `1/cos33.7°=1.202`），offset 只改尺寸（21→41→82px），**不改拉长比**。
   - 结论：改服务取样级（任何整数 offset）都**只能放大/缩小字与模糊度，不能消除拉伸**；方案 B（Cesium 式每层独立 GeographicTilingScheme + 地理 UV drape）同样救不了——c 栅格符号在球面上本就该按扁矩形地面显示。

3. **为什么 `s=z` 时“看着不太拉伸”、取粗后“拉伸变明显”**：同样 1.20 倍拉长，8px 的字只差约 2px（肉眼难辨），13px 的字差约 3~4px（明显）。即拉伸始终存在，字号小被掩盖，字号一大就暴露。

4. **后续可选方向（待评估，未实施）**：
   - **推荐：影像走 `img_c`，中文注记改用墨卡托 `cia_w`**（引擎本就支持每层独立方案；c/w 同源，WGS84 与 CGCS2000 差异亚米级，注记对齐无影响），从根上避免栅格符号拉伸与字号问题，注记还省一次重投影。代价：注记不再是“纯 4490 链路”。
   - **彻底解：中文注记改客户端矢量标注**（引擎自己用字体绘制文字），与底图切片方案彻底解耦，任意 CRS 都不拉伸。
   - 若必须坚持 `cia_c`：只能在“字小但拉伸不明显（s=z，现状）”与“字大但纵向拉长约 1.2 倍（s=z-1）”之间二选一，无两全整数档。
   - 备注：另存在与本问题正交的待办“**把全局瓦片 LOD 偏移改造成单图层参数**”（当前 `setTileLodOffset` 是全局的，会同时影响影像和注记）；它控制的是“要哪一级结果瓦/请求量/细分密度”，与 `serviceZoomOffset`（控制重投影内部取哪一级服务原始瓦）语义不同，不应合并。

1. ~~早期两个近似~~（均已于 2026-10-31 修复）：服务级 s=z-1 的垂直 1/cos(lat) 整体拉高改 s=z（§5.2）；瓦内线性纬度改逐行墨卡托反投影（§5.6）。仅天地图 c 最高矩阵 18 对应引擎 z18 单一层级受服务上限钳制，可能有轻微残余。
2. 若真机首屏 CPU/帧率或高纬精度不满足，升级 **方案 A′（GPU warp）**：复用 3D 24×24 / 2D 加密网格，逐顶点算地理 UV 采样，产物仍写墨卡托 `(z,x,y)` 缓存，不动选瓦。
3. 终极架构 **方案 B**：每影像层独立 `TilingScheme` + 地理 UV drape + 多影像瓦/祖先链 + 分层网格缓存（对标 Cesium）。
4. 待办：Android JNI/Kotlin 暴露 `TileLayerOptions`；必要时把层级偏移、目标瓦大小等做成更显式的每图层参数。