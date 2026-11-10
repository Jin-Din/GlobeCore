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