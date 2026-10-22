# @zys/globecore

GlobeCore 独立实现的 C++ 地图渲染引擎在鸿蒙（HarmonyOS NEXT）上的 HAR 封装：ArkTS 门面 + NAPI + 共享 C++ 引擎（静态链接 GDAL / PROJ / libcurl 等）。

## 安装

```shell
ohpm install @zys/globecore
```

## 使用

```ts
import {
  NativeMapView, Camera, Position, NativeSrs, NativeNet,
  CesiumTerrainProvider, TerrainSourceOptions
} from '@zys/globecore';

// 首次需在 Ability 初始化时解压内置 PROJ 数据与 CA 证书
NativeSrs.initProjData(context);
NativeNet.initCaBundle(context);
```

公开 API 以 `Index.ets` 导出清单为准（`NativeMapView` / `ViewMode` / `Camera` / `Position` / `VectorStyle` / `NativeSrs` / `NativeNet` / `CesiumTerrainProvider` 等）。

`CesiumTerrainProvider` 不绑定具体地形服务，宿主需通过 `TerrainSourceOptions` 注入 `baseUrl`、独立 `cacheDir`，并可配置 `maxZoom/concurrency/headers`。

`NativeMapView` 还提供统一地形事件监听：

```ts
mapView.setOnTerrainProviderEventListener((payload: string): void => {
  // @Jin payload 为单行 JSON，包含 event 与完整 stats
  console.info(`terrain event: ${payload}`);
});
mapView.configureTerrainProvider(baseUrl, cacheDir, 15, 4, 3, headers);
mapView.prefetchTerrainProviderMetadata();
```

事件包括 `metadata_started`、`metadata_ready`、`refresh_started`、`first_tile`、`progress`、`refresh_completed`、`error`、`enabled`、`disabled`。页面退出时传 `undefined` 注销监听。

## 支持 ABI

`arm64-v8a`、`x86_64`。

## 版本

`0.1.3`

## 许可

Apache-2.0，详见 `LICENSE`。
