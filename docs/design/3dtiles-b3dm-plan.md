# 3D Tiles（b3dm 倾斜摄影）分阶段实施方案

> @Jin 分支：`feature/3dtiles-b3dm`  
> @Jin 测试数据：`http://210.74.129.84:8520/app-pub/web-data/3dtiles/qx-dyt/tileset.json`  
> @Jin 原则：实验性能力，按阶段合入；P0 不接入现有渲染帧，不影响地图、地形和矢量既有行为，可通过移除 `tileset3d/` 源清单回退。

## 1. 测试数据结构

- @Jin 根 `tileset.json` 的根节点带 ECEF `transform`，子节点通过 `content.uri = 0/root.json` 引用外部 tileset。
- @Jin `0/root.json` 继续递归到 `top/*.b3dm` 与 `Tile_*/*.json`。
- @Jin 叶子 JSON 指向同目录 `*_L17_00.b3dm`；实测 b3dm header 为 `magic=b3dm, version=1`，Feature Table 后直接内嵌 `glTF` GLB。
- @Jin 资产声明 `gltfUpAxis=Z`，后续渲染必须处理 Z-up 到引擎 ECEF/相机坐标的轴向转换。

## 2. 分阶段计划

### P0：数据解析最小闭环（已完成本地验证）

- @Jin 补齐 `TilesetJson` 对 `transform`、`content.url` 兼容字段、`asset.gltfUpAxis` 的解析。
- @Jin 新增 `tileset3d/B3dmDecoder`：解析 b3dm header、Feature Table、Batch Table、`RTC_CENTER`，提取内嵌 GLB。
- @Jin 新增 `tileset3d/Tileset3DUrl`：解析相对路径、`./`、`../`、站点绝对路径和外部 tileset/b3dm 后缀。
- @Jin 新增 `tileset3d/Tileset3DLoader`：复用 `HttpClient` 下载 JSON/b3dm，但暂不做 LOD、缓存和 GL 上传。
- @Jin 验证方式：独立编译数据解析源码，并用真实 b3dm 验证 header、GLB 偏移和 `GltfDecoder` 可解码。

### P1：Tileset 调度与坐标数学（已完成，并随 P2 一起通过鸿蒙原生构建）

- @Jin 新增 `Tileset3DProvider`，递归加载外部 tileset，维护 tile 树和资源 URL。
- @Jin 基于 `boundingVolume`、相机视锥、`geometricError` 和屏幕空间误差（SSE）选择可见 tile。
- @Jin 实现 `REPLACE` 优先，随后补 `ADD`；加入加载中祖先兜底、卸载 LRU、并发限制、失败重试和磁盘缓存。
- @Jin 累积 tile `transform`，处理 `RTC_CENTER` 与 `gltfUpAxis=Z`。

### P2：Renderer 模型渲染闭环（C++ 内核闭环已完成，平台 API/真机可视化属 P3）

- @Jin 在 Renderer 增加模型图层，把 GLB POSITION/NORMAL/indices 上传为 VBO/IBO。
- @Jin 先以纯色/简单材质显示，再扩展纹理和完整 PBR。
- @Jin 接入深度测试，确保模型与地形、地球球体正确遮挡融合。
- @Jin 处理模型内存/显存淘汰和线程安全 GL 上传。

### P3：平台 API 与 Demo 验证

- @Jin C++ `GlobeEngine` 增加 `addTileset3D/removeTileset3D/setTileset3DVisible` 等最小 API。
- @Jin Harmony NAPI/ArkTS 与 Android JNI/Kotlin 同步导出；Demo 增加加载入口和状态提示。
- @Jin 使用给定倾斜摄影数据做真机定位、缩放、旋转、地形遮挡、内存占用和弱网缓存验证。

### P4：增强能力

- @Jin 支持 `i3dm/pnts/cmpt`、外部 `.gltf`、纹理/PBR、Batch Table 属性、要素拾取、高亮、透明度、剖切和压平。

## 3. 当前边界（2026-11-10 P2 后更新）

- @Jin 对外仍不可用：`addTileset3D` 仅为 C++ 内部 API，Harmony NAPI/ArkTS、Android JNI/Kotlin 与 Demo 均未导出。
- @Jin 渲染仅纯色 + 简单 Lambert 光照，无 glTF 纹理/PBR，真实倾斜摄影的观感验证留待纹理支持后。
- @Jin Provider 暂无磁盘缓存、LRU 卸载、失败重试与 ADD refine；GPU 模型缓存只增不删，长时间浏览显存会持续增长。
- @Jin 回退方式不变：从三套 CMakeLists 移除 `tileset3d/` 源清单，并删除 `GlobeEngine`/`Renderer` 中标注 @Jin 的 3D Tiles 接线即可，不影响栅格、地形、矢量通路。

## 3A. 历史边界（P2 前）

### P0 时期的原始边界描述

- @Jin P0 只证明 core 能识别并解开 3D Tiles/b3dm 数据，不代表已经能在地图上显示模型。
- @Jin 现有 `GltfDecoder` 仅提取 POSITION/NORMAL/indices/baseColorFactor，不支持纹理、动画、骨骼和 Morph Target。
- @Jin 在 P1/P2 完成前，不向 Harmony/Android 门面公开 3D Tiles API，避免宿主误接半成品能力。

## 4. 当前阶段验证记录

- @Jin P0：使用真实 `0/top/2_0_0_0.b3dm` 验证，b3dm 总字节 474,424，剥离 header/table 后得到 GLB 474,372 字节；`GltfDecoder` 成功解出 1 个 primitive、13,917 个顶点。
- @Jin P0：根 `tileset.json` 成功解析 `gltfUpAxis=Z`、根节点 ECEF `transform` 和外部 tileset 引用。
- @Jin P1：使用真实 `0/root.json` 构建运行时图，共 93 个 tile 节点；在模拟 1000 米视距、1200 像素视口高、SSE 阈值 16 时选中 12 个 b3dm，首个内容为 `0/top/3_0_0_0.b3dm`。
- @Jin 当前仍未接入 `GlobeEngine`/Renderer/Harmony NAPI，因此地图界面尚不会显示模型，也不会影响既有栅格、地形、矢量渲染。

- @Jin P2（2026-11-10）：`Tileset3DModelBuilder` 完成 Z-up 换轴、`RTC_CENTER`、tile ECEF transform 到 anchor-RTC 的 CPU 转换；超 65535 索引自动展开非索引三角形以兼容 GLES2。
- @Jin P2（2026-11-10）：`Renderer` 新增独立 3D Tiles shader（position/normal/color 交错顶点 + Lambert），在栅格之后、矢量之前绘制，开启深度测试与地球/地形正确遮挡；`GlobeEngine` 每帧把眼点/FOV/视口/视锥平面投递给 Provider。
- @Jin P2（2026-11-10）：Provider 调度加固——外部 tileset 改为工作清单递归展开，支持任意深度嵌套（实测 qx-dyt 数据：65 个 tileset JSON、92 个 b3dm 全部可达，最大嵌套深度 2）；移除 `lock_guard` 内手动 unlock 的危险写法，网络下载全部移到锁外；接入 `Camera3D` double 精度视锥 6 平面保守裁剪；单帧最多处理 8 个 b3dm。
- @Jin P2 验证（2026-11-10）：鸿蒙 NDK clang++（aarch64-linux-ohos）对 7 个 tileset3d 源及 `GlobeEngine.cpp`/`Renderer.cpp`/`TilesetJson.cpp` 语法核查全部通过；`.cxx` 缓存目录 ninja 全量增量构建 14/14 目标成功并链接出 `libglobecore.so`（零 error，仅 --gcc-toolchain 无关警告）。
- @Jin P2 数据验证（2026-11-10）：对真实服务模拟工作清单递归，root→`0/root.json`（93 节点：29 直连 b3dm + 63 外部 Tile JSON）→叶子 `*_L17_00.b3dm` 全链路可达；叶子 b3dm 实测 HTTP 200、23,580 字节、magic=b3dm/version=1。

### P3：平台 API 与真机验证（进行中，2026-11-10）

- @Jin 平台接线已完成：C++ `GlobeEngine::addTileset3D/setTileset3DVisible` → Harmony NAPI（`nativeAddTileset3D/nativeSetTileset3DVisible`）→ ArkTS `NativeLib/NativeMapView` → Demo「数据与样例」面板「倾斜摄影 3D Tiles」按钮；点击后自动切 3D、创建图层并 flyTo 数据区域（34.2196N/108.9594E，2500m）。
- @Jin P3 真机首个阻断缺陷——runtime 节点 `source` 悬空指针（URL 乱码、b3dm 无法下载），根因分两层：
  - @Jin 第一层：`Tileset3DGraphBuilder::build()` 中 `addNode` 保存的 `source` 最初指向**入参** `const TilesetResult& tileset`；Provider 的 `loaded.tileset` 是局部变量，`build()` 返回即销毁，全部 `source` 成悬空指针。P1 离线验证时入参与图同作用域存活，所以未暴露。
  - @Jin 第二层（仅改指向仍不彻底）：`TilesetResult::root` 是**值类型内嵌成员**，Provider 以 `*graph = build(...)` move 赋值接管图，move 后 root 随对象搬到新地址，根节点的 `source` 依然悬空；子节点因位于 `std::vector` 堆缓冲、vector move 只转移缓冲而幸免。测试数据根节点恰好带 `content.uri=0/root.json`，必被解引用，真机表现为 `item diag` 打印乱码 URL。
- @Jin 修复方案（结构性治本，非判空兜底）：`Tileset3DGraph::tileset` 由值成员改为 `std::unique_ptr<TilesetResult>` 堆托管，`build()` 中 `make_unique<TilesetResult>(tileset)` 深拷贝入参后，所有 `source` 指向该堆对象内部节点；图的默认 move 只转移 unique_ptr，堆对象及其节点树地址在图整个生命周期内恒定。拷贝构造/赋值本就 delete，生命周期约束不变。改动文件：`tileset3d/Tileset3DGraph.h`、`Tileset3DGraph.cpp`、`Tileset3DProvider.cpp`（`gltfUpAxis` 访问改 `->`）。
- @Jin P3 真机验证（设备 `5KLBB25911200313`，2026-11-10）：根图 3 节点构建成功 → 外部 `0/root.json` 93 节点图构建成功 → 选中项 URL 全部完整无乱码（`.../0/root.json`、`.../0/top/0_0_0_0.b3dm` 等）；放大 3 级后累计 18 个不同 b3dm 完成「下载→GLB 解码→model built→delivered→GPU uploaded」全链路，`b3dm failed/glb failed/root load failed/external graph failed` 均为 0；画面在目标地理位置出现灰色纯色 Lambert 模型块（无纹理，符合 P2 当前边界）。
- @Jin 收尾（2026-11-10）：`Tileset3DProvider` 内 `select/item/refresh diag` 三条临时 LOGI 已移除，保留 `graph built/root graph ready/model built/delivered/GPU uploaded` 等关键生命周期日志；移除后重新构建 HAP 真机冒烟，根图+93 节点外图构建正常、b3dm 下载与 GPU 上传零失败。GPU 模型只增不删（LRU 卸载）、磁盘缓存、失败重试、ADD refine、纹理/PBR 仍属后续阶段。
### P3 真机排查（续）：模型朝向与明暗（2026-11-10）

- @Jin 现象：悬空指针修复后模型能下载上 GPU，但真机上呈均匀深灰“平板”，看不出建筑起伏；进一步排查发现朝向也不对。
- @Jin 根因一（明暗）：实测 qx-dyt 的 b3dm（cesiumlab 生成，材质 `KHR_materials_unlit` + baseColorTexture）**只含 POSITION/TEXCOORD_0，没有 NORMAL**。旧构建器对无法线顶点统一给默认法向 (0,0,1)，Lambert 光照下所有面亮度一致，几何再复杂也退化为灰板。
- @Jin 修复一：`Tileset3DModelBuilder` 新增自动法向生成——当 primitive 缺 NORMAL 时，按三角形叉积做面积加权累加生成平滑顶点法向（支持 16/32 位索引与非索引拓扑；退化三角形零贡献，零累加顶点回退局部 +Z），随后与自带 NORMAL 走同一 localToWorld 变换；`model built` 日志追加 `genNormal=1` 标识。
- @Jin 根因二（朝向，关键）：先怀疑 Z-up→Y-up 换轴矩阵的列主序符号，两次修改（`(x,y,z)→(x,-z,y)` 与 `→(x,z,-y)`）真机都表现为模型相对地表旋转 90°、垂直立起。最终用根 tileset.json 的真实 transform 做确定性 ENU 验证：该 tile 局部帧本身就是 **Z-up 的 ENU 帧**（localX→东、localY→北、localZ→天，三组点积均为 ±1.0000），而 b3dm 内容同样是 Z-up，内容 +z（建筑高度）经 identity 后恰好指向 ENU Up；两种换轴都把高度轴转到局部 ±Y=南北水平方向，故必错。
- @Jin 修复二：mars3d osgb2tiles3 这类数据 `gltfUpAxis="Z"` 但局部帧已按 Z-up 烘焙，**换轴矩阵取 identity（不换轴）**，代码注释完整记录 ENU 推导。后续若接入局部帧为 Y-up 的标准 Cesium 数据，应先检测帧上轴（包围盒/帧基向量与 ENU 的点积）再决定是否换轴，不能无条件 identity，也不能无条件套 Cesium 换轴。
- @Jin 真机复测（2026-11-10，设备 `5KLBB25911200313`）：identity + 自动法向后，低角度可见建筑、树木与塔状结构垂直立起、地面水平贴合影像，三角面片明暗层次正常；单次进入视角最多 37 个 b3dm 上 GPU，下载/解码/上传零失败。仍为无纹理纯色（glTF 纹理/PBR 属 P4）。
### P4 待排查问题记录（2026-11-10 人工真机反馈）

- @Jin 问题：3D Tiles 模型**未贴地**——无论是否开启三维地形，模型与地表之间都存在高度偏差（疑似整体悬空或高程基准不一致）。下一步纹理解码任务并行/之后排查。
- @Jin 初步怀疑方向（待验证）：① b3dm 顶点高度基准与 tile transform 平移/锚点的关系，RTC_CENTER 与 ECEF anchor 组合是否引入垂直偏移；② 数据自身的高程基准（椭球高 vs 正高）或 osgb2tiles3 烘焙时的高度处理；③ 椭球定位经纬度与数据实际位置（根 transform 反算锚点 108.9594E/34.0409N，与文档预期 34.2196N 存在纬度差，需核对）；④ 地形开关均异常说明问题更可能在模型自身 ECEF 定位而非地形贴合环节。

### P4 纹理解码真机验证通过（2026-11-11，设备 5KLBB25911200313）

- @Jin 装机版本核对：启动 hilog 实测 `globecore build id = v0.1.3-20261110-t3tex-phase16-npot`，与当前 WIP 源码 Renderer.cpp 顶部 GLOBECORE_BUILD_ID 一致；HAP 内 stripped libglobecore.so（5,026,760 字节）同样检出该 ID，排除旧 HAR 缓存误判。
- @Jin 全链路日志（两轮加载/隐藏/恢复）：b3dm 解码（`b3dm decoded`，含 ftJson/btJson 长度）→ `Tileset3D model built`（vertices/indices、zUp=1）→ `delivered` → `Tileset3D GPU model uploaded`，累计 49 次 GPU 上传、21 条 `Texture NPOT fallback to LINEAR`（实测尺寸 553x1000、532x1024、407x1024、347x421 等），零 globecore GL/纹理解码错误；日志中出现的 ERROR 均为华为 analyticsservice 系统服务，与本引擎无关。
- @Jin 关键修复点验证：测试数据纹理为非 2 幂（NPOT）尺寸，GLES2 下 NPOT 纹理不允许 mipmap 与 repeat wrap；Texture.cpp 的 `isPowerOfTwo` 分流生效，NPOT 自动回退 GL_LINEAR（无 mipmap），模型不再全黑/全白；GltfDecoder 内嵌 JPEG/PNG 提取、TEXCOORD_0、KHR_materials_unlit 通路工作正常，V 不翻转后贴图方向正确。
- @Jin 视觉 A/B 验证：同视角点击「隐藏倾斜摄影 3D Tiles」后只剩 ArcGIS 平面影像（无立体几何），恢复显示后近距离斜视可见带真实纹理的低多边形摄影测量建筑（深色瓦顶、院落、八角亭、树木），证明纹理确实附着在 b3dm 模型而非底图；朝向（identity 不换轴）保持正确，建筑垂直立起、地面水平。
- @Jin 显隐切换：`setTileset3DVisible(false/true)` 生效，恢复显示后模型与纹理重新上传成功，图层复用不重复创建 Provider。
- @Jin 结论：白模→纹理阶段目标达成，纹理解码修复真机验证通过。
- @Jin 遗留与下一步：① 模型不贴地（见上节 P4 四个怀疑方向：RTC_CENTER/椭球高 vs 正高/锚点纬度 34.0409N vs 预期 34.2196N）仍未排查；② Demo 中 TILESET3D_FLY_ALT=700 与 flyTo tilt=30 带 `[diag-temp]` 标记，属近距离纹理核查临时值，定稿前恢复 2500m/默认倾角；③ NPOT 回退 LINEAR 后远距离无 mipmap 可能轻微闪烁/闪烁感，后续可评估纹理重采样到 POT 或运行时生成 mipmap 的扩展方案。

### P4 不贴地排查：高程基准量化核实（2026-11-11，离线确定性计算）

- @Jin 锚点纠偏（推翻旧怀疑）：对根 tileset.json 的 transform 平移 ECEF(-1715331.32, 4993175.00, 3566614.93) 做 WGS84 标准迭代反算，结果为 lon=108.959400°E、lat=34.219600°N、椭球高 h=0.00m（精确）。与 Demo flyTo 目标点完全一致，**旧记录中“锚点纬度 34.0409N 偏差”的怀疑不成立**，水平定位无问题，差异只在垂直方向。整棵树仅根节点一个 transform，中间 0/root.json 与全部叶子均无 transform，叶子 b3dm 顶点 local z 即沿根 ENU Up 轴的世界高度。
- @Jin 模型侧高度（世界=局部，因锚点 h=0）：根包围盒 box 中心 z=445.02、半高 41.11，z 区间约 403.9~486.1m；6 个直连 b3dm 顶点实测 local zmin≈404.4~405.4m；0/root.json 深度 5 子块 box 底面（cz-hz）约 404~434m。模型底面整体在 **405m 量级**，且随地形起伏烘焙，属绝对高程而非建筑相对高度。
- @Jin 地形服务寻址澄清（排查中纠正了三处离线解码错误）：210 服务即 Mars3D《中国地形12.5米》镜像，quantized-mesh-1.0、scheme=tms、EPSG:4326 地理网格；服务瓦片索引为 cols=2^(z+1)、rows=2^z、y 自南向北（与引擎 `CesiumTerrain.cpp::intersectingGeoTileRange/cesiumServiceTilesForEngineTile` 口径一致），并非墨卡托 XYZ。quantized-mesh 头部为 **88 字节**（60 基础头 + 24 字节 HorizonOcclusionPoint），u/v/h 为 zigzag 差分（引擎 `QuantizedMesh.cpp` 实现正确），u/v 按**瓦片边界**（非全球）映射经纬度。
- @Jin 地形侧实测高程（Node 解码真实 .terrain，目标点 108.9594E/34.2196N）：z12≈403m（9 顶点，粗）；z13 IDW≈390.6m（46 顶点）；**z14 IDW≈395.8m、最近顶点 396.5m（相距约 30m），瓦片高度区间 385.0~405.8m（54 顶点）**；z15 服务无覆盖（HTTP 404），即该区域最细 DEM 网格间距约 150m，无法分辨 300m 厂区内部起伏。
- @Jin 大地水准面佐证：GeographicLib GeoidEval 查得该点 EGM2008 大地水准面差 **N=-34.64m**（EGM96 -34.88m），即 h椭球 = H正高 - 34.64。
- @Jin 结论：模型底面 ~405m 对锚点处 DEM 地表 ~396m，模型整体偏高约 **+9m（厂区各子块因 DEM 精度与真实坡度，离散到约 +10~+25m）**，方向为悬空，与人工真机观感一致。离线无法 100% 区分“椭球高/正高混用”与“DEM 综合误差”各占多少：若两侧同为正高口径，残差主要来自 12.5m DEM 栅格综合与测绘控制差；若模型为 GNSS 椭球高而 DEM 未做 geoid 改正（mars3d 转换链常见做法），则属垂直基准混用。EGM2008 的 -34.6m 是本区域基准差异的理论上限参考。
- @Jin 修复方案（实验性、可回退）：给 3D Tiles 图层增加用户可配的 `heightOffset`（米，沿 ENU Up 平移，默认 0 不改旧行为），链路 C++ `addTileset3D` 选项 → Harmony NAPI → ArkTS → Demo 调试步进控件；先在真机以 -10m 为起点目视 dial-in，记录该数据集的经验偏移。不把 geoid 常数硬编码进引擎，保持数据集无关的通用性。回退方式：heightOffset 恒传 0 即恢复当前行为，删除参数接线即完全回退。
- @Jin 高斯泼溅结论（2026-11-11）：大古柏/保生柏/黄帝陵/老君柏四个地址 HTTP 200，但 tileset.json 声明 `asset.extensions.DPToolKits.type=gaussianSplat`、`extensionsRequired` 含 `KHR_gaussian_splatting` 与 `KHR_gaussian_splatting_compression_spz_2`，叶子为 `merge/merge_0_gs.glb` 点云原语；与 qx-dyt 的 b3dm 三角网（mars3d osgb2tiles3）是不同渲染原语，当前 GLES2 三角网管线不支持。已列入未解决待办（需新增高斯泼溅解码 + 点云/泼溅渲染管线），Demo「模型」面板仅挂待办说明，不提供可点击入口。
- @Jin Demo UI 改造（2026-11-11 真机验证通过，设备 5KLBB25911200313）：左侧功能坞新增「模型」入口（图层/数据/模型/地形/绘制/调试/更多 共 7 项），倾斜摄影按钮迁入「三维模型」面板，「数据与样例」面板移除 3D Tiles 入口并留引导文案；高斯泼溅四数据集在面板内标注待支持原因。

### P4 配套修复：地形 z15 全 404 父级回退 + 3D Tiles heightOffset 实验参数（2026-11-11，构建核对 `v0.1.3-20261111-terrain-fallback-heightoffset2`）

#### A. 地形服务 z15 缺失 → 父级瓦片回退（实验性，可回退）

- @Jin 现象：真机近距离（displayZoom 被钳到 maxZoom=15）加载 210 地形时整轮失败，日志 `refresh complete z=15 tasks=182 delivered=0 failures=182`，182 块 z15 瓦片全部 HTTP 404。
- @Jin 根因：210 服务 layer.json 的 available 声明 z15 覆盖目标区域（范围 52534-52689 / 22486-22725 含目标瓦片 52603/22613），但服务器上 z15 数据整体缺失（声明范围角点同样 404），z14 及以下正常；属“元数据夸大覆盖”。旧引擎无父级回退，404 即计失败，导致地形完全不显示。
- @Jin 修复（`CesiumTerrainProvider.h/.cpp`，全部 @Jin 注释）：
  - 新增 `fetchTileWithFallback()`：请求瓦片失败后按 EPSG:4326 地理网格逐级回退祖先（父索引 = 子 x/y 整除 2），首个成功的祖先瓦片按其**真实地理 bounds** 经 tileSink 注入 Renderer；仅当祖先成功、子级失败时才把子级写入缺失负缓存（网络整体故障不污染缓存）。
  - 新增缺失负缓存 `missingServiceTiles_`（上限 4096，满则清空重探）：`buildTasks()` 对已知缺失瓦片直接换祖先，后续刷新不再重复打 404；`resetDeliveredTiles()`（手动刷新/切换图源/关地形）同步清空，保留重新探测机会。
  - stats 新增 `parentFallbacks` 计数并经 statsJson 透出。
- @Jin 真机证据（设备 5KLBB25911200313）：日志连续 `parent fallback 15/52609/22614 -> 14/26304/11307` 等，`refresh complete z=15 tasks=182 delivered=182 failures=0`，UI 状态条“地形加载完成 182/182 失败 0”；第二轮刷新父级全部磁盘缓存命中。
- @Jin 回退方式：删除 `fetchTileWithFallback`/`nearestAvailableAncestor`/负缓存相关代码块，worker 改回直接调 `fetchTile` 即恢复旧行为；成功路径零开销（仅失败后才进入回退分支）。

#### B. 3D Tiles heightOffset 垂直校准参数（实验性，可回退）

- @Jin 目的：针对离线量化结论（模型底面 ~405m vs 锚点 DEM ~396m，模型整体悬空约 +9m，子块离散 +10~25m；EGM2008 大地水准面差 N=-34.64m 为理论上限参考），提供数据集无关的运行时垂直校准手段，不把 geoid 常数硬编码进引擎。
- @Jin 数学：RTC 顶点存储为 `world - anchorEcef`，着色器 `rtc = aPos + uOffset`、`uOffset = anchorEcef - eye`。沿锚点 ECEF 天顶 `normalize(anchorEcef)` 平移 Δ 米只需改 uniform：`uOffset = anchorEcef - eye + up * heightOffset`，RTC 顶点与法线均不动，纯平移不影响光照，运行时连续可调、零模型重建。
- @Jin 全链路：C++ `Tileset3DLayer.heightOffset`（atomic double，默认 0）→ `Renderer::drawTileset3D` uniform 注入 → `GlobeEngine::setTileset3DHeightOffset(layerId, meters)` → Harmony NAPI `nativeSetTileset3DHeightOffset`（含 index.d.ts 声明）→ ArkTS `NativeLib`/`NativeMapView.setTileset3DHeightOffset` → Demo「三维模型」面板步进控件（步长 5m、范围 ±100m、一键复位 0，仅模型显示时出现）。
- @Jin 回退方式：Demo 控件恒显示 0、setter 恒传 0 即与旧行为逐位一致；彻底回退删除上述链路即可。当前仅 Harmony 链路导出，Android JNI 暂无 3D Tiles 能力，API 指南待能力定稿后同步。
- @Jin 真机校准方法：加载 210 地形 + qx-dyt，在模型面板从 0 起以 -5m 步进下调，目视模型底面与地形贴合最佳后记录该数据集经验偏移量（离线预判起点约 -10m，最终以真机目视为准）。

#### C. 真机功能验证（2026-11-11，设备 5KLBB25911200313，BUILD_ID `v0.1.3-20261111-terrain-fallback-heightoffset2`）

- @Jin 地形父级回退真机确认：两轮加载均 `delivered=182 failures=0`，日志连续 `parent fallback 15/... -> 14/...`，第二轮父级 182 块全部磁盘缓存命中；修复前为 `delivered=0 failures=182`。
- @Jin heightOffset 控制闭环真机确认：「三维模型」面板在模型显示后出现 -5 / 偏移量 / +5 / 复位 四控件；连续点 -5 到 -20m 状态条显示 `3D Tiles -20m（高度偏移）`、点复位回 `0m`、再点两次到 `-10m`，全程无崩溃、渲染连续、RTC 模型不重建。
- @Jin 方向数学复核：绘制期 `uOffset = anchorEcef - eye + normalize(anchorEcef) * heightOffset`，顶点世界位置 = 原始 world + up * heightOffset；负值沿天顶反方向（地心方向）整体下移模型，符合“模型悬空需下调”的预期。
- @Jin 待人工最终校准：离线量化预判起点 -10m（模型底面 ~405m vs DEM ~396m），最佳偏移量以真机目视模型底面与 210 地形贴合为准，确定后作为 qx-dyt 数据集经验值记录。
- @Jin 定稿前收尾：恢复 Index.ets 两处 [diag-temp]（TILESET3D_FLY_ALT 700→2500、flyTo tilt 65°→默认倾角）；若高度偏移能力定稿，再补 Harmony API 帮助文档（docs/api/ 目前仅有 android 指南，3D Tiles 暂只在 Harmony 导出）。

#### D. 真机校准结论（2026-11-11，人工目视确认）

- @Jin qx-dyt 数据集经验高度偏移：**heightOffset = -20m**（在 210/Mars3D 12.5m 地形、65° 斜视下人工确认为当前最佳贴合值）。
- @Jin 与离线分析的对照：离线量化差值约 -9m（模型底面 405m vs 锚点 DEM 396m），真机最佳值 -20m 明显更大，说明残差不只是 DEM 栅格综合，模型烘焙高程与 quantized-mesh 椭球高之间还存在约 10m 级的垂直基准/控制差；EGM2008 理论上限 -34.6m 未被吃满，两侧更可能是混合口径而非纯 geoid 差。
- @Jin 通用性原则：引擎默认 heightOffset=0，不内置任何数据集常数；-20m 仅作为 qx-dyt 的宿主侧经验参数，由 Demo/集成方按数据集传入，换数据集需重新目视校准。
- @Jin 操作备注：步进控件为按钮点击式（步长 5m），快速连点存在一次多点的可能（实测出现过 -10 连点两跳变到 -30 的情况），用“复位”回 0 后重新单步设置即可；后续可加节流防抖。

#### E. Demo 写死经验偏移（2026-11-11，BUILD_ID heightoffset3 验证）

- @Jin 按使用方要求，Demo 对 qx-dyt 加载即应用 heightOffset=-20m：Index.ets 新增常量 `TILESET3D_HEIGHT_OFFSET = -20.0`，`toggleTileset3D()` 在图层创建/恢复显示后幂等调用 `view.setTileset3DHeightOffset(layerId, -20)`，`@State tileset3dHeightOffset` 初值同步为该常量，面板上电即显示“偏移 -20m”，无需手动步进。
- @Jin 边界：写死只发生在演示工程 Index.ets，SDK（C++/NAPI/ArkTS 门面）默认值仍为 0、不内置任何数据集常数；步进控件保留，人工仍可临时覆盖，点“复位”回 0，重新加载模型恢复 -20m。
- @Jin 全新卸载重装后首载实测：面板直接显示“偏移 -20m”，状态与图层一致，写死链路生效。
