# @Jin 面状要素 3D 地形贴地专项（polygon-terrain-drape）设计与任务分解

> 状态：草案，待评审，未动代码（实验性/可回退）。
> 日期：2026-11-20。
> 分支：`feature/polygon-terrain-drape`（从 main `14f1065` 切出，已含 addGeoJSONLayer）。
> 关联：`docs/jin-modification-records.md` 第五节未解决待办、第十二节封存专项；README ⑰⑱ drape 系列记录。

## 1. 问题界定

### 1.1 现象

3D 地形开启后，**本地文件来源的面状要素**（Polygon/MultiPolygon，shp/kml/geojson/gpkg 等格式等价）
以及 `addGeoJSONLayer` 的 `data` 字符串面，被起伏地形遮挡：部分三角面埋入山体、整层在某些视角不可见，
且不限于固定缩放级别。在线 URL 省级大面虽已有实验通路，遮挡也未根治。

### 1.2 为什么是格式共性

各格式在读取期已统一转换为内部 `VectorGeometry`，Renderer 只按**图层来源标记 + 几何内容**分流，
不感知原始格式。当前 3D 贴地候选门控（`Renderer.cpp` `uploadGlobeVecChunks`）：

```text
candidateTerrainDrape = isOverlay
                      || (kExperimentalRemoteVectorTerrainDrape && remoteVectorSource)
                      || isExperimentalLocalDrapedLineLayer(vl)
```

| 图层类型 | 标记/几何 | 当前贴地通路 | 面的结果 |
|---|---|---|---|
| 内存动态叠加层 | `isOverlay=true` | `uploadGlobeOverlayDrapedChunks`，任意相交 DEM 即烘焙，整层关地形深度 | 正常 |
| 在线 URL 图层 | `remoteVectorSource=true` | `uploadGlobeRemoteDrapedChunks` 实验通路（精细覆盖门限+预算+逐 chunk 深度） | 大面仍有遗留遮挡 |
| 本地纯线层 | 无 fill/outline、仅 lineVerts | 同上共享函数（drape36） | 不涉及面 |
| **本地面/字符串面** | 两个标记均 false、含 fillVerts/outlineVerts | **非候选，顶点停在椭球面 alt=0** | **被地形深度测试剔除（本专项根因）** |

`addGeoJSONLayer` 中仅 `SourceKind::Url` 置 `remoteVectorSource=true`（`GlobeEngine.cpp`），
File/Data 的面都落入最后一行。

### 1.3 根因分层

1. **准入缺失**：本地面不在 `candidateTerrainDrape`，从不做 DEM 烘焙，这是本地面遮挡的直接根因。
2. **烘焙质量**：远程大面已在通路内但仍遮挡，涉及粗 DEM 下平面 chunk 与贴地 chunk 接缝、
   细分密度与 10 万填充顶点预算的平衡（README ⑰遗留）。
3. **深度策略**：overlay 整层关深度、远程逐 chunk 关深度；本地面接入后必须选择正确口径，
   否则要么继续被埋、要么穿透山体/背面可见。

## 2. 现有可复用资产（不重造轮子）

`uploadGlobeRemoteDrapedChunks` 虽带 "Remote" 旧名，drape36 起已同时服务在线大面与本地纯线，
本专项继续复用而非新增第三套烘焙：

- 自适应目标边长：小区域约 20m，省域按相交最精细 DEM ×0.35 放宽，受 32km 上限钳制。
- 顶点预算：大图层 `kRemoteDrapeFillVertexBudget=100000`、描边 12 万角点，防省域递归爆炸。
- 精细覆盖门限：`overlayHasTerrainCoverage` 对大图层要求相交 DEM 跨度 ≤1°，避免 z0 粗地形触发整省烘焙。
- 相机高度门限：`kRemoteDrapeMaxCameraAltMeters=500km`，高空走平面、低空停顿后烘焙。
- 运动防抖：交互/flyTo 中延迟重建（450ms 静止判定 + 1200ms 签名防抖），地形批量到达不逐块重烘焙。
- 逐 chunk `terrainDraped` 标记 + 绘制侧 `skipTerrainDepthForFill = globeDraped && (isOverlay || ch.terrainDraped)`。
- DEM 粗格索引（`demGrid`/`demCellKey`）、邻近兜底、签名失效与 `resetVectorUpload` 交换逻辑。

## 3. 方案设计

### 3.1 核心思路：新增本地面状候选判定，接入共享烘焙函数

新增与 `isExperimentalLocalDrapedLineLayer` 对称的判定（Renderer 内部，无公开 API 变化）：

```cpp
// @Jin 本地面状贴地实验门控：本地文件/inline 字符串的 Polygon 层（含描边），排除 overlay 与在线源
bool Renderer::isExperimentalLocalDrapedPolygonLayer(const VectorLayer &vl) const;
// 条件：kExperimentalLocalPolygonTerrainDrape(=true)
//      && !vl.isOverlay && !vl.remoteVectorSource
//      && (!geom.fillVerts.empty() || !geom.outlineVerts.empty())
```

随后把 4 个现有分流点的 `localDrapedLine` 扩展为 `localDrapedLine || localDrapedPolygon`：
`uploadGlobeVecChunks` 候选门控、`overlayHasTerrainCoverage` 大图层判定、
`uploadGlobeRemoteDrapedChunks` 内 `largeDrapedVector`、重建触发段的签名/高度门限。
File 与 Data 两种来源因几何内容同构而自动同时生效，无需在 Renderer 区分 SourceKind。

### 3.2 混合内容图层的边界（关键约束）

一个矢量文件可能同时含面、线、点/图标。共享烘焙函数对 fill/outline/line 已验证，
**代码核查（2026-11-20 已完成）**：`uploadGlobeRemoteDrapedChunks` 内含完整点/图标分支——
`buildPoint` 对每个 `pointCoords` 输出点圆或 billboard，仅在 DEM **直接命中**时 `pointMode=1` 烘焙高程，
未命中保持 `pointMode=0` 平面顶点，图标纹理失败时安全降级为普通点通路。

- 结论：门控无需按几何内容排除混合层，面/线/点/图标可同批进入共享函数，与在线 URL 图层口径一致。
- 剩余风险在真机表现而非代码缺口：混合层的点在 DEM 未覆盖区保持平面，可能与已贴地的面出现短暂高差，
  列入 T8 回归观察项，若真机不可接受再加「混合层降级平面」保护开关。

### 3.3 深度策略选择

本地面采用**远程大面同口径的逐 chunk 深度**，不采用 overlay 的整层关深度：

- 采到 DEM 的 chunk：`terrainDraped=true`，关闭对地形的深度测试，稳定盖在地表。
- 仍是平面的 chunk（精细 DEM 未覆盖区）：保留深度测试，避免面穿透山体或背面可见。
- 与 drape36c 纯线过渡期口径一致，降低面/线/地形三者间的深度竞争风险。

### 3.4 分级放开（控制回归面）

| 级别 | 数据 | 门限策略 | 目的 |
|---|---|---|---|
| P0 | 太白山式小范围本地 GeoJSON 面（数公里） | 任意相交 DEM 即可（同 overlay） | 先打通准入与基础烘焙 |
| P1 | 县域 Polygon（数十公里） | 精细覆盖 ≤1° 门限 + 10 万预算 | 验证自适应细分与防抖 |
| P2 | 省级 MultiPolygon（千公里级、含岛洞） | 全部门限 + 运动延迟 | 攻克历史遗留大面遮挡 |

每级真机通过后再放开下一级；开关常量统一在 Renderer 顶部，便于按级别回退。

### 3.5 与既有遗留项的关系

- 透明填充零几何（`fillAlpha<=0` 跳过 earcut/fill VBO）：本专项**不并入**，保持独立待办，避免一次改两条管线。
- conformal outline ribbon 恒定像素宽度优化：不并入，本期沿用现有描边烘焙。
- 在线 URL 大面遗留遮挡：随本专项对共享函数的覆盖/深度修复**顺带受益**，但不单独为 URL 通路改门控。

### 3.6 回退

- 总开关 `kExperimentalLocalPolygonTerrainDrape=false`：本地面立即回到椭球平面旧路径，行为与 main 完全一致。
- 不新增持久化结构、不改文件格式与公开 API；回退即还原 Renderer 分流条件，无数据迁移。

## 4. 任务分解（WBS）

> 粒度：每任务可独立编译/提交，改动处加 @Jin 注释。除文档外全部集中在 `render/Renderer.{h,cpp}`，
> 预期 diff 远小于 GeoJSON 专项（无桥接层/平台层改动）。

### 阶段 0：基线

- T0 记录 main 基线 `GLOBECORE_BUILD_ID`；桌面冒烟确认 2D 矢量、overlay、纯线 drape36 现状，留存对照日志。

### 阶段 1：核查与门控

- T1 【已完成】核查确认共享烘焙函数已内置点/图标分支（直接命中 DEM 才抬升，否则平面），混合层无需排除。
- T2 新增 `isExperimentalLocalDrapedPolygonLayer` 与开关常量；4 处分流点接入，先仅 P0 小面门限。
- T3 深度策略核对：确认本地面走逐 chunk `terrainDraped`，绘制侧无需新增分支（复用现有 skip 条件）。
- 验收：OHOS clang `-fsyntax-only` + hvigor HAR 构建；2D/overlay/纯线零回归（平面路径不变）。

### 阶段 2：分级放开

- T4 P0 小范围本地面真机：烘焙命中数、chunk 数、无破碎/无穿模。
- T5 放开 P1 县域：验证 ≤1° 精细覆盖门限、10 万预算截断日志、运动中平面→停顿后贴地切换无黑缝。
- T6 放开 P2 省级 MultiPolygon：岛洞环、8.1 万原始顶点场景，复核 GL 线程耗时与签名防抖（对齐 ⑰ 的卡死教训）。

### 阶段 3：混合内容与收尾

- T7 真机验证混合层点/图标在 DEM 未覆盖区的平面-贴地高差表现；不可接受时加「混合层降级平面」保护开关。
- T8 回归矩阵：2D/3D 切换、500km 高度门限跨越、地形加载过渡期、显隐/拾取/`setVectorMinLevel`、
  LITE 与 full 双形态（格式无关，各验一个本地面文件）、在线 URL 面顺带回归。
- T9 文档：README @Jin 区段与 `jin-modification-records.md` 新增专项记录（现象/门控/真机数据/回退）；
  本设计文档状态改「已实现」；无公开 API 变化，`docs/api/` 预计无需改动，若 T7 引入 setter 再同步。

## 5. 风险与对策

- 省域大面 GL 线程长阻塞（2026-10-27 曾卡死被杀）：复用现有运动延迟+1200ms 防抖+10 万预算，
  P2 必须量测单轮烘焙耗时并写入真机记录，超标先收紧预算而非放开。
- 平面/贴地 chunk 接缝黑缝：沿用 `kRemoteDrapeDemEdgeScale=0.35` 与预算余量，真机逐级别检查边缘。
- 深度口径错误导致穿模：严格逐 chunk 策略，禁止本地面整层关深度；增加背面/地下视角人工检查。
- 混合层点/图标高差：共享函数仅对 DEM 直接命中的点抬升，未覆盖区保持平面；T7 真机验证，必要时加降级开关。
- 与远程大面/纯线共享函数的相互回归：任何共享函数内改动必须同时回归三类既有通路。

## 6. 工作量预估

- T1–T3 约 1.5–2 天（主要是核查与小范围门控）；T4–T6 真机分级约 2 天；T7–T9 约 1–1.5 天。
- 合计约 5–6 人日，不含透明填充零几何、ribbon 恒像素宽度两个独立遗留项。
