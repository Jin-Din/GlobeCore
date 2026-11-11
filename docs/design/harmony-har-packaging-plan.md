# HarmonyOS NEXT SDK 输出 .har 分发包方案（存档）

> @Jin 存档日期：2026-11-11
> 状态：**已执行验证并产出**（2026-11-11，main 基线 fb1cefd，lite 包 `@zys-globecore-0.1.5-dev-lite.har`，ohpm 本地安装实证通过）
> 适用模块：`vendor/GlobeCore/globecore-harmonyos/library`（SDK HAR 模块，模块名 `globecore`，包名 `@zys/globecore`）
> 背景：目标是把 SDK 最终输出为标准 `.har`，供多个 DevEco 工程复用，替代当前手工组装的 dist 扁平目录 + zip 交付方式。

## 1. 现状结论

### 1.1 代码层面已具备 .har 输出能力

`globecore-harmonyos/library/hvigorfile.ts` 已注册两个层级的任务：

- hvigor 内置 `assembleHar`：完成 ArkTS 编译、CMake native 编译（含 `libglobecore.so` / `libc++_shared.so` 自动打入 HAR libs）、资源与类型声明打包，产出标准 `globecore.har`；
- 自定义 `createVersionedHar`（`dependencies: ['assembleHar']`）：在标准产物之外，按模块 `oh-package.json5` 的版本号额外复制一份版本化 HAR，命名规则 `@zys-globecore-<version>.har`（包名中的 `/` 替换为 `-`）。
- 产物目录：`library/build/default/outputs/default/`。

### 1.2 现有两套产物的分工

| 方式 | 产物 | 用途 |
|---|---|---|
| `scripts/build-ohos-so-win.sh`（cmake+ninja，绕过 hvigor） | 仅 `libglobecore.so` | 引擎 C++ 迭代时分钟级暴露编译错误 |
| 手工组装的 `dist/globecore-harmony-next-<ver>-lite/` + zip | 扁平 HAR 目录（非 .har） | 当前交付形态，仓库内**无生成脚本**，手工拼装 |
| `hvigorw ... createVersionedHar`（本方案） | 标准 `.har` | 定稿交付、多工程复用（目标形态） |

### 1.3 已核实的环境缺口

1. 两个 DevEco 工程（`globecore-harmonyos/`、`demo/GlobeCoreDemo/`）均**不含 hvigorw 包装器脚本**，只有 `hvigor/hvigor-config.json5`；需使用 DevEco 自带的 `C:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat`（node 用 DevEco 自带 `tools\node\node.exe`，在 DevEco 内置 Terminal 中执行环境即就绪）。
2. SDK 独立工程 `globecore-harmonyos/oh_modules` 不存在，首次执行需先 `ohpm install`（或 DevEco Sync）。
3. `library/build/` 下无任何历史 .har，命令链路注册了但未跑通过，首次执行需实证。

## 2. 执行步骤（待主线程验证）

在 DevEco Studio 内置 Terminal 中：

```bash
cd E:\WorkBuddy\qt_ohos_map_sdk\vendor\GlobeCore\globecore-harmonyos
ohpm install
# debug 形态
"C:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat" ^
  --mode module -p module=globecore@default -p product=default ^
  createVersionedHar --no-daemon
# release 形态追加：-p buildMode=release
```

入口选择 SDK 独立工程 `vendor/GlobeCore/globecore-harmonyos/`（含 entry + library），不要用 demo app 工程。

## 3. 消费方工程引用方式（.har 形态）

1. 把产出的 `.har` 放入消费工程 `entry/libs/`；
2. `entry/oh-package.json5` 声明：`"@zys/globecore": "file:./libs/globecore.har"`；
3. Sync 后 `import { NativeMapView, Camera, ViewMode } from '@zys/globecore'` 使用；
4. 其余初始化不变：`EntryAbility` 调 `NativeNet.initCaBundle(this.context)`；申请 `ohos.permission.INTERNET`；http 明文地址放行 cleartext；
5. 仅 arm64-v8a，不支持 x86_64 模拟器。

## 4. 转 .har 交付前必须处理的三件事

1. **版本号统一**：模块 `oh-package.json5` 的 version 仍为 `0.1.3`，而 dist 包已命名 `0.1.5.0-dev`，出包前先对齐。
2. **命名规则缺口（需改代码）**：AGENTS.md 第 3 节强制分发包名带 `-lite`/`-full` 构建形态标记，当前 `createVersionedHar` 插件只拼版本号、不带该标记。需增强插件：读取构建参数 `GLOBECORE_LITE` 取值，在文件名追加 `-lite` 或 `-full`（示例：`@zys-globecore-0.1.5.0-dev-lite.har`）。改动处按全局约定加 `@Jin` 注释，并同步 README 与 API 文档记录。
3. **LITE 开关位置不变**：`library/build-profile.json5` 的 `externalNativeOptions.arguments` 中 `-DGLOBECORE_LITE=ON/OFF`；切 full 前须先由 `build-scripts/build-all.sh --target ohos` 交叉编译 GDAL/PROJ/libkml 静态库。未明确 lite/full 的打包请求，按 AGENTS.md 提醒规则必须先询问确认。

## 5. 验证清单（首次出 .har 时执行）

- [ ] 命令成功产出 `globecore.har` 与版本化 har；
- [ ] 解包 har 实证内容齐全：`libs/arm64-v8a/`（两个 .so）、ArkTS 门面、`index.d.ts` 类型声明、`rawfile/ca/cacert.pem`；
- [ ] `llvm-nm`/`readelf` 实证 lite 形态：真实 GDAL/OGR/PROJ 符号为 0、含 `readLiteGeoJsonFile`、ELF 为 AArch64；
- [ ] 新建最小 DevEco 工程按第 3 节引用，真机跑通在线瓦片 + 地形 + GeoJSON 最小闭环；
- [ ] 包内附 `README-integration.md`（构建形态、GLOBECORE_LITE 取值、能力边界、出包 commit）。

## 6. 首次执行实测记录（2026-11-11，@Jin）

- **独立工程 SDK 配置修复**：根 build-profile.json5 的 `targetSdkVersion` 原为非法纯数字串 `"26.0.0"`，hvigor 报 00306042；改为 `"6.1.0(23)"`（对齐 demo 工程）。注意 JSON5 行尾注释会吞掉属性逗号，注释须独占一行。
- **版本号 semver 约束**：ohpm 拒绝四段版本 `0.1.5.0-dev`（00618006 Invalid Dep Version）；HAR 内 oh-package.json5 版本改为合法三段 semver `0.1.5-dev`，外层交付目录/zip 仍按 AGENTS.md 命名保留 `0.1.5.0-dev` 口径，README-integration 注明两者差异。
- **命名插件已增强**：library/hvigorfile.ts 的 createVersionedHar 现读取 build-profile.json5 中 `-DGLOBECORE_LITE=` 取值，版本化 HAR 自动追加 `-lite`/`-full` 后缀（解析失败按 lite 兜底）。
- **HAR 容器格式**：.har 为 tar 而非 zip，用 `tar -tf/-xf` 查看解包；类型声明以 ArkTS 源码（package/Index.ets 与 src/main/ets）随包分发，无独立 .d.ts。
- **lite 形态实证**：readelf 为 ELF64 AArch64；动态符号表无真实 GDAL/OGR/PROJ 符号（注意 sqlite3_progress_handler 含 "ogress" 子串会误匹配 OGR）；.symtab 含内部符号 readLiteGeoJsonFile。
- **消费侧实证**：最小工程 `"@zys/globecore": "file:./globecore.har"` 经 ohpm install 成功，oh_modules 内 so/ets/rawfile 齐全。真机最小闭环（在线瓦片+地形+GeoJSON）待消费工程接入时补验。
- 交付物：dist/globecore-harmony-next-0.1.5.0-dev-lite/（HAR + docs 双端 API HTML + README-integration.md）及同名 zip；旧基线 6ff280b 扁平目录包已改名 *-oldbase-6ff280b 保留。
## 6. 备注：能力基线提醒

当前 dist lite 包基线为 main 提交 `6ff280b`，不含其后合入的 3D Tiles(b3dm)、单图层 `setLayerZoomOffset`、地形父级回退等能力；需要这些能力时基于最新 main 重新出包。