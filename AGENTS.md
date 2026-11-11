# 全局工作约定（AGENTS.md）

> 本文件是本工作区所有 AI 编程助手与开发协作的强制约定，最后更新：2026-11-11。

## 1. 代码与文档注释

- 在代码改动处添加注释，并以 **@Jin** 前缀标识本库追加/修改的内容，便于与原作者代码区分。
- 拥有 git 仓库的代码工程（vendor/GlobeCore），每次改动定稿后同步更新 README；实验性改动须注明「实验性/可回退」与回退方式。
- 新增、修改、删除公开 API 时，同次定稿同步更新对应平台的 API 帮助文档（docs/api/ 下 android-api-guide.html 与 harmony-api-guide.html）。

## 2. 分支与提交

- main 为主线稳定分支；实验性能力在 feature/* 分支开展（如 feature/3dtiles-b3dm、feature/epsg-4490），定稿后合并 main 并推送云端。
- 提交信息中文描述，涉及本库改造的内容标注 @Jin。

## 3. HarmonyOS NEXT 打包约定（GLOBECORE_LITE）

- 构建开关：globecore-harmonyos/library/build-profile.json5 的 externalNativeOptions.arguments 中
  -DGLOBECORE_LITE=ON 为 lite 构建，改为 OFF 为全量构建（全量需先由 build-scripts 交叉编译 GDAL/PROJ/libkml 静态库）。
- **命名强制规则**：分发包（目录与 zip）文件名必须带构建形态标记：
  - lite 构建：globecore-harmony-next-<version>-lite（示例：globecore-harmony-next-0.1.5.0-dev-lite.zip）
  - 全量构建：globecore-harmony-next-<version>-full（示例：globecore-harmony-next-0.1.5.0-dev-full.zip）
- 包内必须附 README-integration.md，声明构建形态（lite/full）、GLOBECORE_LITE 取值、能力边界与出包 commit。
- **提醒规则**：当用户要求打包但未明确选择 lite 还是全量时，助手必须先询问确认，不得自行选择；确认后再构建，并在交付说明中复述实际构建形态与命名。
- LITE 能力边界：在线 XYZ 瓦片（含 EPSG:4490）、三维地形 Provider、GeoJSON（本地文件/在线 URL）、叠加层、拾取、手势、flyTo、LOD 调试可用；
  本地 shp/kml/dwg/tif 等 GDAL 系文件直读、PROJ 坐标转换、NativeVector 文件 IO/SQL/属性写回、NativeLayerInfo 为桩实现（返回 false/null/空串/中文错误，不崩溃）。
- 出包后用 llvm-nm/readelf 实证构建形态（真实 GDAL/OGR/PROJ API 符号应为 0、含 readLiteGeoJsonFile 符号），并检查 ELF 架构为 AArch64。

## 4. 真机调试

- 鸿蒙安全锁屏拒绝 uitest/uinput 合成事件解锁，锁屏时需请用户手动解锁，不要反复尝试注入。
- 构建后用 GLOBECORE_BUILD_ID（Renderer.cpp 顶部宏）日志核对装机版本，避免旧 HAR 缓存误判。
