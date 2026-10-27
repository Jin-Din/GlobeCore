// libglobecore.so (NAPI) 类型声明 —— 与 ohos/napi_*.cpp 的导出描述符一一对应。
// 命名契约：so 名 libglobecore.so ↔ nm_modname "globecore" ↔ 本 import 名（大小写一致）。
// 类型映射（对齐 napi_util.h 口径）：
//   句柄 → number（int64，< 2^53 安全）
//   double[N] → Float64Array；ARGB 像素/计数 → Int32Array；字符串数组 → string[]
//   可空返回（对齐 JNI null）用 | null；可空入参用 undefined | null 联合。
// 内部门面（NativeLib.ets）统一做类型归一（Float64Array ↔ number[]），宿主不直接依赖本文件。

interface GlobeCore {
  // ── 生命周期 / GL 宿主 ──
  nativeCreate(): number;
  nativeDestroy(handle: number): void;
  /// surfaceId：XComponentController.getXComponentSurfaceId() 数值；width/height：surface 像素尺寸
  nativeAttach(handle: number, surfaceId: number, width: number, height: number): boolean;
  nativeDetach(handle: number): void;
  nativeRequestRender(handle: number): void;

  // ── 相机 / 视图模式 / 手势 ──
  nativeSetCamera(handle: number, latitudeDeg: number, longitudeDeg: number, altitudeMeters: number,
    headingDeg: number, tiltDeg: number, rollDeg: number, fieldOfViewDeg: number, altitudeMode: number): void;
  /// @Jin 平滑飞行到目标相机；动画在 C++ GL 帧循环推进，durationMs<=0 表示即时跳转。
  nativeFlyToCamera(handle: number, latitudeDeg: number, longitudeDeg: number, altitudeMeters: number,
    headingDeg: number, tiltDeg: number, rollDeg: number, fieldOfViewDeg: number,
    altitudeMode: number, durationMs: number): void;
  /// [lat, lon, alt, heading, tilt, roll, fov, altMode]
  nativeGetCamera(handle: number): Float64Array | null;
  nativeSetViewMode(handle: number, mode: number): void;
  nativeGetViewMode(handle: number): number;
  nativePanBy(handle: number, dxPx: number, dyPx: number): void;
  nativeZoomBy(handle: number, factor: number, focusXpx: number, focusYpx: number): void;
  nativeRotateHeading(handle: number, deltaDeg: number): void;
  nativeRotateTilt(handle: number, deltaDeg: number): void;

  // ── 瓦片 / 栅格图层 ──
  nativeAddTileLayer(handle: number, cacheDir: string, urlTemplate: string, maxLevel: number, overlay: boolean): void;
  nativeSetLayerVisible(handle: number, index: number, visible: boolean): void;
  nativeAddRasterLayer(handle: number, cacheDir: string, path: string): number;
  /// 移除瓦片图源图层（含栅格层，墓碑；index 为 addTileLayer/addRasterLayer 加入次序）
  nativeRemoveTileLayer(handle: number, index: number): void;
  /// 最近一次 nativeAddRasterLayer 失败原因（中文）；成功或从未失败 null
  nativeGetRasterAddError(handle: number): string | null;
  /// 全幅加载栅格层（后台整幅直读，独立索引空间）；返回新图层索引，无效 -1
  nativeAddRasterImageLayer(handle: number, cacheDir: string, path: string): number;
  nativeSetRasterImageVisible(handle: number, index: number, visible: boolean): void;
  /// 可见全幅栅格层是否仍有未完成的加载/纹理上传（纯查询不触发重绘）
  nativeHasRasterLoading(handle: number): boolean;
  /// 指定全幅栅格层的加载失败原因（中文）；未失败/越界/已成功 null
  nativeGetRasterImageError(handle: number, index: number): string | null;

  // ── 矢量图层 ──
  nativeAddVectorLayer(handle: number, path: string,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    fillExplicit: boolean, lineExplicit: boolean,
    labelField: string, labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    featureStyleOverride: string,
    iconArgb: Int32Array | undefined | null, iconW: number, iconH: number,
    hasExtent: boolean, minLon: number, minLat: number, maxLon: number, maxLat: number,
    maxFeatures: number): number;
  nativeUpdateVectorExtent(handle: number, index: number, hasExtent: boolean,
    minLon: number, minLat: number, maxLon: number, maxLat: number, maxFeatures: number): void;
  nativeHasVectorLoading(handle: number): boolean;
  nativeSetVectorLayerVisible(handle: number, index: number, visible: boolean): void;
  nativeSetVectorMinLevel(handle: number, index: number, minLevel: number): void;
  nativeGetCameraZoomLevel(handle: number): number;
  /// @Jin 当前相机在指定图层上限下的目标瓦片 LOD（3D 四叉树仍可能按距离混入更粗瓦片）。
  nativeGetTileZoomLevel(handle: number, maxLevel: number): number;
  /// @Jin 实验性瓦片 LOD 偏移：负数更粗，0 恢复标准细节。
  nativeSetTileLodOffset(handle: number, offset: number): void;
  /// @Jin 实验模式：目标瓦片 LOD 与相机地图显示级别保持一致。
  nativeSetTileLodAlignDisplay(handle: number, align: boolean): void;
  nativeRemoveVectorLayer(handle: number, index: number): void;

  // ── 动态叠加层 ──
  nativeAddOverlayLayer(handle: number,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    iconArgb: Int32Array | undefined | null, iconW: number, iconH: number): number;
  nativeUpdateOverlayPoints(handle: number, index: number, lonlat: Float64Array,
    fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeUpdateOverlayLines(handle: number, index: number, lonlat: Float64Array,
    vertexCounts: Float64Array, fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeUpdateOverlayPolygons(handle: number, index: number, lonlat: Float64Array,
    ringVertexCounts: Float64Array, ringsPerFeature: Float64Array,
    fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeRemoveOverlayLayer(handle: number, index: number): void;
  nativeSetOverlayNoPick(handle: number, index: number, noPick: boolean): void;
  nativeClearOverlayLayers(handle: number): void;

  // ── 拾取 / 反算 / 要素几何 ──
  /// [layerIndex, fid]，未命中 null
  nativePickVector(handle: number, sxPx: number, syPx: number): Float64Array | null;
  /// 单击拾取回调（事件源 native 手势识别，坐标 vp）；传 null 反注册
  nativeSetTapCallback(handle: number, callback: ((x: number, y: number) => void) | null): void;
  /// 长按拾取回调（单指保持按住未越过拖动阈值达长按时长，坐标 vp）；传 null 反注册
  nativeSetLongPressCallback(handle: number, callback: ((x: number, y: number) => void) | null): void;
  /// [lon, lat]，视口未就绪 null
  nativeScreenToGeo(handle: number, sxPx: number, syPx: number): Float64Array | null;
  /// [ [type], ringCounts, ringsPerFeature, lonlat ]，未命中 null
  nativeFeatureGeometry(handle: number, layerIndex: number, fid: number): Float64Array[] | null;

  // ── 显示 / 标记 / 字体 ──
  nativeSetDisplayDensity(handle: number, density: number): void;
  nativeSetLocationMarker(handle: number, lonDeg: number, latDeg: number, visible: boolean, headingDeg: number): void;
  nativeSetLocationMarkerIcon(handle: number, iconArgb: Int32Array, iconW: number, iconH: number): void;
  nativeSetFontPath(handle: number, path: string): void;

  // ── 网络 / HTTPS 证书（全局，无句柄）──
  /// 设置 CA 证书包（cacert.pem）绝对路径供瓦片 HTTPS 校验；空串回退引擎默认（Android 系统目录）
  nativeSetCaBundle(path: string): void;

  // ── PROJ 坐标转换（bridge/srs）──
  nativeGetProjVersion(): number;
  nativeInitProjDataPath(path: string): void;
  /// [x, y]（目标坐标系），失败 null
  nativeConvert(x: number, y: number, srcCrs: string, tgtCrs: string): Float64Array | null;

  // ── 三维地形（feature/terrain-pre）──
  /// @Jin 从 layer.json 创建 C++ 地形元数据句柄；解析失败返回 0。
  nativeCesiumTerrainCreate(jsonText: string, fallbackMaxZoom: number): number;
  /// @Jin 释放 C++ 地形元数据句柄。
  nativeCesiumTerrainDestroy(handle: number): void;
  /// @Jin 返回 layer.json 声明的有效 maxzoom。
  nativeCesiumTerrainMaxZoom(handle: number): number;
  /// @Jin 返回 layer.json 声明的瓦片 URL 模板。
  nativeCesiumTerrainTileTemplate(handle: number): string;
  /// @Jin 返回 [z,x,y,z,x,y,...]，表示一个引擎瓦片相交的全部服务 DEM。
  nativeCesiumTerrainEngineToServiceTiles(handle: number, z: number, x: number, y: number): Float64Array | null;
  /// @Jin 返回服务瓦片经纬度四至 [lonMin,latMin,lonMax,latMax]。
  nativeCesiumTerrainTileBounds(z: number, x: number, y: number): Float64Array;
  /// @Jin 判断服务瓦片是否位于 layer.json 的 available 范围内。
  nativeCesiumTerrainIsAvailable(handle: number, z: number, x: number, y: number): boolean;
  /// 注入一块 Cesium quantized-mesh v1.0 地形瓦片。
  /// z/x/y 用【本引擎瓦片网格】编号（渲染时按它查缓存）；四至用【地形服务自己的分块方案】算出。
  /// bytes 传空数组 = 删除该块。可任意线程调用（内部入队，GL 线程帧首生效）。
  nativeSetTerrainTile(handle: number, z: number, x: number, y: number, bytes: Uint8Array,
    lonMin: number, latMin: number, lonMax: number, latMax: number): void;
  /// 清空全部地形缓存（换地形源 / 关闭地形时调用）
  nativeClearTerrain(handle: number): void;

  // @Jin C++ CesiumTerrainProvider：平台仅配置数据源与读取诊断状态。
  nativeConfigureTerrainProvider(handle: number, baseUrl: string, cacheDir: string,
    maxZoom: number, concurrency: number, tileRadius: number, headersJson: string): boolean;
  nativeSetTerrainProviderEnabled(handle: number, enabled: boolean): boolean;
  nativeRefreshTerrainProvider(handle: number): void;
  /// @Jin 开关相机静止后的 C++ 地形自动刷新；默认关闭以保持旧行为。
  nativeSetTerrainProviderAutoFollow(handle: number, enabled: boolean): void;
  nativePrefetchTerrainProviderMetadata(handle: number): void;
  nativeTerrainProviderStatus(handle: number): string;
  /// @Jin 统一地形事件回调；payload 为含 event 与 stats 的单行 JSON，null 反注册。
  nativeSetTerrainProviderEventCallback(handle: number, callback: ((payload: string) => void) | null): void;

  // ── GDAL / 图层元信息（bridge/gdal_info、layer_info）──
  nativeGetGdalVersion(): string;
  nativeGetVectorDrivers(): string;
  /// [minLon, minLat, maxLon, maxLat]，失败 null
  nativeGetLayerExtent(path: string): Float64Array | null;
  nativeGetVectorFieldNames(path: string): string[];
  /// "名称\nproj4定义"，无 SRS null
  nativeGetLayerSrs(path: string): string | null;

  // ── 矢量要素 IO（bridge/vector_io，JSON 传输）──
  nativeReadVectorFeatures(path: string, minLon: number, minLat: number, maxLon: number, maxLat: number,
    includeAllFields: boolean, labelField: string, simplifyGeometry: boolean, simplifyTolerance: number): string | null;
  nativeQueryVectorFeatures(path: string, sql: string): string | null;
  nativeCountVectorFeatures(path: string): number;
  nativeUpdateFeatureAttributes(path: string, featureId: number, keys: string[], values: string[]): boolean;
  nativeGetFeatureAttributes(path: string, featureId: number): string | null;
}

const globeCore: GlobeCore;
export default globeCore;
