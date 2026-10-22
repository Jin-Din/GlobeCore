// libglobecore HAR 导出清单（对齐 Android globecore AAR 的公开 API）。
// NativeLib / MapGestures 为模块内部实现，不导出（宿主经 NativeMapView 等门面访问）。
export { NativeMapView, ViewMode } from './src/main/ets/NativeMapView';
export { Camera, AltitudeMode } from './src/main/ets/Camera';
export { Position } from './src/main/ets/Position';
export { VectorStyle } from './src/main/ets/VectorStyle';
export { VectorExtent } from './src/main/ets/VectorExtent';
export { FeatureGeometry, PickResult } from './src/main/ets/FeatureGeometry';
export { NativeSrs } from './src/main/ets/NativeSrs';
export { NativeNet } from './src/main/ets/NativeNet';
export { NativeLayerInfo } from './src/main/ets/NativeLayerInfo';
export { NativeVector } from './src/main/ets/NativeVector';
// @Jin 通用 Cesium quantized-mesh 地形提供器：数据源由宿主注入，HAR 不绑定具体服务。
export {
  CesiumTerrainProvider,
  TerrainSourceOptions,
  TerrainTile,
  GeoBounds,
  geoTileBounds,
  lonLatToGeoTile,
  mercatorTileCenter,
  lonLatToMercatorTile,
  serviceZoomForEngineZoom,
} from './src/main/ets/terrain/CesiumTerrainProvider';
