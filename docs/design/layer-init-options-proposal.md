# 图层初始化参数入参化方案存档（3D Tiles heightOffset / 瓦片层 serviceZoomOffset）

> @Jin 存档日期：2026-11-11
> 状态：**已实施并真机验证通过**（2026-11-11，分支 fix/3d-gesture-low-altitude，BUILD_ID v0.1.5-20261111-layer-init-options-fix1）；Android JNI/Kotlin 入口待移植，C++ 核心已双端共用。
> 基线：main 分支
> 核心诉求（使用方）：图层级校准参数应在**图层初始化时作为 options 传入**，而非只能创建后补调 setter；setter 保留用于运行时调节。

## 1. 现状核实结论（main）

两个参数的架构形态完全同构：**引擎内部已是逐层独立状态，但公开 API 只暴露了创建后 setter，没有初始化入口。**

### 1.1 3D Tiles heightOffset

- 状态：`tileset3d/Tileset3DModel.h` 中 `std::atomic<double> heightOffset{0.0}`，每个 Tileset3DLayer 独立，默认 0。
- 生效点：`render/Renderer.cpp` 绘制期 `uOffset = anchorEcef - eye + normalize(anchorEcef) * heightOffset`，纯 uniform 平移、零模型重建、可运行时连续调节。
- 创建入口（无 options）：C++ `GlobeEngine::addTileset3D(const std::string& url)` → NAPI `nativeAddTileset3D` → ArkTS `NativeMapView.addTileset3D(url): number`。
- 唯一注入方式：创建后 `setTileset3DHeightOffset(layerId, meters)`（全链路 C++/NAPI/ArkTS/index.d.ts 已通）。
- Demo 现状：qx-dyt 先 `addTileset3D` 再幂等补调 `setTileset3DHeightOffset(layerId, -20)`，常量 `TILESET3D_HEIGHT_OFFSET = -20.0`。

### 1.2 瓦片图层 serviceZoomOffset

- 状态：`layer/TileLayer.h` 中 `int serviceZoomOffset = 0`，每个 TileLayer 独立；`Renderer.cpp` 2D/3D 两处逐层读取并钳制 `[-3, 2]`。
- 已有初始化结构 `TileLayerOptions` 仅含 `crs / tmsY / tileMatrixOffset`，**不含 serviceZoomOffset**；ArkTS `addTileLayerWithOptions(...)` 参数同构，也无该字段。
- 唯一注入方式：创建后 `setLayerZoomOffset(index, offset)`，index 为 addTileLayer/addTileLayerWithOptions 的加入次序。
- 另存全局 `setTileLodOffset(offset)`（全局 LOD 调试遗留），单图层能力即为取代它而做，但同样落成了 setter。

## 2. 改造目标与原则

1. 两个参数都支持**初始化 options 一次性传入**，图层创建即带校准值，宿主无需"先建后补"两步调用。
2. **setter 全部保留**：初始化值负责开箱即贴合/即适配，setter 负责运行时目视校准（3D Tiles 步进防抖待办仍挂在 setter 链路上）。
3. 默认值保持 0，旧签名/旧调用方行为完全不变；新增入口以 WithOptions 后缀或可选 options 形式提供，实验性、可回退。
4. 引擎保持数据集无关：不内置任何数据集常数（如 qx-dyt 的 -20m 仅宿主侧传入）。
5. 按全局约定：改动处加 @Jin 注释；定稿同步 README、docs/api 下 android/harmony 两份 API 文档；Harmony 与 Android 双端 API 形态对齐。

## 3. 具体改造点

### 3.1 3D Tiles：新增 addTileset3DWithOptions

- C++：新增 options 结构（如 `Tileset3DOptions { double heightOffset = 0.0; }`）与重载 `int addTileset3D(const std::string& url, const Tileset3DOptions* opts)`，图层创建后立即 `setHeightOffset(opts->heightOffset)`；旧单参签名保留并委托新实现（opts=nullptr）。
- NAPI：新增 `nativeAddTileset3DWithOptions(handle, url, options)`，options 为 JS 对象，解析 heightOffset，缺省 0。
- ArkTS：`addTileset3DWithOptions(url: string, options: { heightOffset?: number }): number`；同步更新 cpp/types/libglobecore/index.d.ts。
- Demo：qx-dyt 改为创建时传 `{ heightOffset: -20 }`，移除创建后的补调；步进面板仍接 `setTileset3DHeightOffset`。
- 回退：options 不传或恒传 0 即恢复当前行为；删除 options 接线即完全回退。

### 3.2 瓦片图层：TileLayerOptions 增加 serviceZoomOffset

- C++：`TileLayerOptions` 增加 `int serviceZoomOffset = 0`；`addTileLayerWithOptions` 建层后写入该成员（沿用现有 `[-3,2]` 钳制）。
- NAPI：`nativeAddTileLayerWithOptions` 增加可选入参（建议改为 options 对象形态的新 NAPI 函数，避免位置参数继续膨胀；旧函数保留）。
- ArkTS：新增 `addTileLayerWithLayerOptions(cacheDir, urlTemplate, maxLevel, overlay, options: { crs?, tmsY?, tileMatrixOffset?, serviceZoomOffset? })`，或在现有函数追加带默认值参数（评估 ArkTS 可选参数与 NAPI 位置解析的兼容性后定）；更新 index.d.ts。
- Demo/天地图 4490 场景：把目前 LOD 调试后补调的 -1/-2 改为图源初始化时声明。
- 回退：字段缺省 0，行为与现状一致。

## 4. 验证清单

- [x] 旧调用方（单参 addTileset3D、现有 addTileLayerWithOptions）行为不变，默认偏移均为 0；
- [x] 初始化传入后首帧即生效，无"先按 0 渲染一帧再跳变"的观感；
- [x] 初始化值与运行时 setter 叠加语义明确（建议 setter 覆盖初始化值，文档写明）；
- [x] 多图层各自偏移互不影响（两个 3D Tiles 模型不同 heightOffset；影像/注记两层不同 serviceZoomOffset）；
- [x] qx-dyt 初始化 -20m 与当前补调效果目视一致；天地图 4490 注记初始化偏移与当前 LOD 调试效果一致；
- [x] 双端 API 文档与 README 同步，标注实验性/可回退与回退方式。

## 6. 实施记录（2026-11-11）

- ArkTS 侧因 arkts-no-obj-literals-as-types 限制，options 采用具名接口 `TileLayerInitOptions`/`Tileset3DInitOptions`，定义于 NativeLib.ets，NativeMapView 再导出，HAR 根出口不变。
- 调试期修复一次启动崩溃：`addTileLayer` 在 `std::move(layer)` 后的 LOGI 仍解引用 `layer->serviceZoomOffset`（SIGSEGV @0x7c），改为 move 前落本地变量。
- 真机验证：默认 zoomOffset=0、-1 档位建层日志与 4490/3857 各 crs 组合均符合预期且无 setter 重放；qx-dyt 建层日志 initHeightOffset=-20.000，步进 setter 覆盖正常。详细数据见 docs/jin-modification-records.md 第七节。
## 5. 关联事项

- 待办沿用：heightOffset 步进按钮节流防抖；4490 注记字小/轻微拉伸的彻底方案（serviceZoomOffset 初始化入参是其宿主侧适配手段之一，非引擎内拉伸修复）。
- 关联存档：`docs/design/3dtiles-b3dm-plan.md`（heightOffset 原始设计与 -20m 校准记录）、`docs/design/harmony-har-packaging-plan.md`（.har 交付方案）。