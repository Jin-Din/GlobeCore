// NativeMapView.ets —— globecore HAR 对外地图视图组件（对应 Android NativeMapView.kt）。
//
// 只是地图视图的「接口外壳」，渲染引擎（相机、瓦片网格、取瓦片、
// 缓存、GL 绘制）全部位于 native 的 C++ GlobeEngine。与 Kotlin 版差异仅在 GL 宿主：
//  - Android：Kotlin GLSurfaceView 创建 EGL 并转发 surfaceCreated/Changed/drawFrame/releaseGl；
//  - 鸿蒙：XComponent(SURFACE) 提供窗口，onLoad 时把其 OPEN_GL 上下文交给 nativeAttach，
//    native 侧 MapRenderHost 起 EGL 渲染线程接管 GL 生命周期；瓦片到位重绘经
//    nativeRequestRender 通路（对齐 RENDERMODE_WHEN_DIRTY 按需渲染）。
// 手势识别（平移/捏合/双指旋转/双指俯仰/双击/惯性/单击/长按）在 native DispatchTouchEvent：
// SURFACE 型 XComponent 触摸不投递 ArkTS .onTouch（实证），[MapGestures] 仅作事件兜底保留；
// 单击/长按经 NAPI 回调转投本类 tapListener/longPressListener，惯性由渲染线程自驱动。双指旋转与俯仰仅 3D 生效（同 Android 兜底门控）。
//
// 使用方（entry 演示页 / 宿主页面）需：
//  - 组件 aboutToAppear 自动创建 native 实例，aboutToDisappear 自动 detach + destroy；
//  - XComponent libraryname 'globecore' ↔ libglobecore.so（nm_modname "globecore"）。
import { NativeLib } from './NativeLib';
import { Camera, AltitudeMode } from './Camera';
import { Position } from './Position';
import { VectorStyle } from './VectorStyle';
import { VectorExtent } from './VectorExtent';
import { FeatureGeometry, PickResult } from './FeatureGeometry';
import { MapGestures, GestureSink } from './MapGestures';

/** 组件内区域/偏移坐标（onAreaChange 的 Area.position 取值，与地理 Position 无关） */
interface Position2D {
  x: number;
  y: number;
}

/** 地图视图模式（口径同 native Navigator::ViewMode）：2D 平面墨卡托正交 / 3D 球体透视 */
export enum ViewMode {
  TWO_D = 0,
  THREE_D = 1,
}

/** XComponent 实例 id 自增序列（模块级）：为每个 NativeMapView 生成唯一 id，满足官方 id 唯一性约束 */
let g_xcIdSeq: number = 0;

class MapViewGestureSink implements GestureSink {
  private view: NativeMapView;

  constructor(view: NativeMapView) {
    this.view = view;
  }

  isReady(): boolean {
    return this.view.isNativeReady();
  }

  panBy(dxVp: number, dyVp: number): void {
    NativeLib.nativePanBy(this.view.getHandle(), dxVp, dyVp);
  }

  zoomBy(scaleFactor: number, focusX: number, focusY: number): void {
    NativeLib.nativeZoomBy(this.view.getHandle(), scaleFactor, focusX, focusY);
  }

  rotateBy(deltaDeg: number): void {
    // 仅 3D 消费旋转：2D 正交恒正北，手势不应用（避免 heading 被隐形写入而后续切 3D 突变）
    if (NativeLib.nativeGetViewMode(this.view.getHandle()) === ViewMode.THREE_D) {
      NativeLib.nativeRotateHeading(this.view.getHandle(), deltaDeg);
    }
  }

  is3DMode(): boolean {
    return NativeLib.nativeGetViewMode(this.view.getHandle()) === ViewMode.THREE_D;
  }

  tiltBy(deltaDeg: number): void {
    // 仅 3D 消费俯仰（与 rotateBy 同口径门控）；native 钳 [0,80]，透传累加
    if (NativeLib.nativeGetViewMode(this.view.getHandle()) === ViewMode.THREE_D) {
      NativeLib.nativeRotateTilt(this.view.getHandle(), deltaDeg);
    }
  }

  viewHeightVp(): number {
    return this.view.getViewHeightVp();
  }

  requestRender(): void {
    this.view.requestRender();
  }
}

@Component
export struct NativeMapView {
  /** 屏幕密度（宿主传 display 密度或 1.0），作为 LOD 细分判据的 densityFactor */
  density: number = 1.0;
  /** XComponent 实例唯一 id（官方约束 id 必须唯一）：避免多实例共存时按 id 反查组件冲突 */
  private xcId: string = `globeCoreMap_${g_xcIdSeq++}`;

  /** 引用挂载回调：aboutToAppear 时把组件引用交给宿主（struct 不可 new，宿主经此驱动 API） */
  onReady: ((view: NativeMapView) => void) | undefined = undefined;

  /** native C++ GlobeEngine 实例句柄（对象地址）；0 表示已释放或创建失败 */
  private nativeHandle: number = 0;
  /** XComponent 控制器：onLoad 后经其取 surfaceId / surface 尺寸交给 native 直建渲染线程 */
  private xcController: XComponentController = new XComponentController();
  private gestures: MapGestures = new MapGestures(new MapViewGestureSink(this));
  /** 视图在窗口中的位置偏移（vp）：TouchEvent 视口坐标 → 视图本地坐标换算 */
  private viewOffsetX: number = 0;
  private viewOffsetY: number = 0;
  /** 视图高（vp）：供手势 tilt 按 参考实现 口径 180°·Δcy/视图高 换算（onAreaChange 下发） */
  private viewHeight: number = 0;
  /** 单击监听缓存：事件源在 native 手势识别，经 nativeSetTapCallback 注册投递器 */
  private tapListener: ((x: number, y: number) => void) | undefined = undefined;
  /** 长按监听缓存：事件源在 native 手势识别（长按候选到点），经 nativeSetLongPressCallback 注册投递器 */
  private longPressListener: ((x: number, y: number) => void) | undefined = undefined;

  // ── 对外 API（与 Kotlin NativeMapView 逐一对应）──

  /** native 实例是否可用（手势 sink 守卫用；宿主勿调） */
  isNativeReady(): boolean {
    return this.nativeHandle !== 0;
  }

  /** native 句柄只读取器（同文件 GestureSink 实现转调用；宿主勿调） */
  getHandle(): number {
    return this.nativeHandle;
  }

  /** 视图高（vp）只读取器（GestureSink 换算 tilt 灵敏度用；宿主勿调） */
  getViewHeightVp(): number {
    return this.viewHeight;
  }

  /**
   * 设置单击回调：native 手势识别确认单击时触发（已避开双击缩放与拖动平移）。
   * 事件源在 native（SURFACE 触摸不进 ArkTS .onTouch），经 NAPI threadsafe function 转投主线程；
   * 注册时机自由（attach 前注册由 native 在 attach 时补装），传 undefined 反注册。
   *
   * 单位口径：native 触摸坐标（OH_NativeXComponent_TouchEvent.x/y）本身就是 px（物理像素），
   * 与引擎视口（screenToGeo/pickVector 的换算分母，同为 surface px）天然一致，故原样透出、无需
   * 密度换算。切勿再乘 density：否则坐标会被顶到视口外（x>viewportWidth），screenToGeo 只能外推出
   * 错误经纬度——表现为「toast 数值正常、但测点/面画在可见区外、地图上看不到绘制」。
   */
  setOnTapListener(listener: ((x: number, y: number) => void) | undefined): void {
    this.tapListener = listener;
    if (listener !== undefined) {
      const cb: (x: number, y: number) => void = (x: number, y: number): void => {
        if (this.tapListener === listener) {
          listener(x, y);
        }
      };
      NativeLib.nativeSetTapCallback(this.nativeHandle, cb);
    } else {
      NativeLib.nativeSetTapCallback(this.nativeHandle, null);
    }
  }

  /**
   * 设置长按回调：native 手势识别确认长按（单指保持按住、未越过拖动阈值达长按时长）时触发，
   * 坐标为视图内像素口径（同 setOnTapListener，原样透出不乘密度）；供宿主弹落点操作菜单等。
   * 注册/反注册时机同 setOnTapListener（口径对齐 Android NativeMapView.setOnLongPressListener）。
   */
  setOnLongPressListener(listener: ((x: number, y: number) => void) | undefined): void {
    this.longPressListener = listener;
    if (listener !== undefined) {
      const cb: (x: number, y: number) => void = (x: number, y: number): void => {
        if (this.longPressListener === listener) {
          listener(x, y);
        }
      };
      NativeLib.nativeSetLongPressCallback(this.nativeHandle, cb);
    } else {
      NativeLib.nativeSetLongPressCallback(this.nativeHandle, null);
    }
  }

  /**
   * 设置相机（[Camera] 值对象唯一入口）：仅更新 native 相机状态、不涉及 GL，
   * 可在 surface 就绪前调用（首帧绘制时生效）。native 按 fieldOfView + 视口高把高度
   * 换算为连续缩放级别；heading/tilt 由 3D 透视通路消费。
   */
  setCamera(camera: Camera): void {
    NativeLib.nativeSetCamera(this.nativeHandle,
      camera.latitude, camera.longitude, camera.altitude,
      camera.heading, camera.tilt, camera.roll,
      camera.fieldOfView, Number(camera.altitudeMode));
    this.requestRender();
  }

  /** 读回当前相机；句柄已释放/surface 未建返回 undefined */
  getCamera(): Camera | undefined {
    const pose = NativeLib.nativeGetCamera(this.nativeHandle);
    if (pose === null || pose.length < 8) {
      return undefined;
    }
    let mode: AltitudeMode = AltitudeMode.ABSOLUTE;
    const idx: number = Math.trunc(pose[7]);
    if (idx >= 0 && idx <= 3) {
      mode = idx as AltitudeMode;
    }
    return new Camera(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], mode);
  }

  /**
   * 设置视图模式（2D 平面正交 / 3D 球体透视）：两模式共用相机状态，切换保持视角连续；
   * 瓦片纹理缓存按 (z,x,y) 键互通。3D 下瓦片球面 + 矢量层 + 定位标记均绘制且可拾取。
   */
  setViewMode(mode: ViewMode): void {
    NativeLib.nativeSetViewMode(this.nativeHandle, Number(mode));
    this.requestRender();
  }

  /** 读回当前视图模式；句柄已释放返回 TWO_D */
  getViewMode(): ViewMode {
    return NativeLib.nativeGetViewMode(this.nativeHandle) === ViewMode.THREE_D
      ? ViewMode.THREE_D : ViewMode.TWO_D;
  }

  /**
   * 添加瓦片图源图层（可多次叠加，按调用顺序绘制：底图在前、注记 overlay 在后）：
   * 磁盘缓存目录 + URL 模板（Token 已替换；空串禁用联网只读缓存）+ 图源最大级别 + 是否叠加层。
   * native 按 <cacheDir>/<z>/<x>_<y>.tile 读写；未命中后台联网拉取并异步上传纹理。
   */
  addTileLayer(cacheDir: string, urlTemplate: string, maxLevel: number, overlay: boolean = false): void {
    NativeLib.nativeAddTileLayer(this.nativeHandle, cacheDir, urlTemplate, maxLevel, overlay);
    this.requestRender();
  }

  /** 设置指定瓦片图层可见性（index 为 addTileLayer 加入次序，0 起）：隐藏层不绘制、不取瓦片 */
  setLayerVisible(index: number, visible: boolean): void {
    NativeLib.nativeSetLayerVisible(this.nativeHandle, index, visible);
    this.requestRender();
  }

  /**
   * 注入一块三维地形瓦片（Cesium quantized-mesh v1.0，feature/terrain-pre）。
   *
   * [z]/[x]/[y] 用本引擎瓦片网格编号（与底图瓦片一致），[bbox] 用该地形服务自己的分块方案
   * 算出的真实四至 [lonMin, latMin, lonMax, latMax]（度）—— 标准 header 不含 bbox，
   * 必须显式给出，否则高度采样会整体错位。
   * [bytes] 传空数组表示删除该块。仅 3D 球体模式消费地形。
   *
   * 可任意线程调用；native 侧入队，GL 线程下一帧帧首生效。地形瓦片的下载与调度由宿主负责
   * （本 HAR 不带地形服务适配，避免绑定具体数据源与坐标系）。
   */
  setTerrainTile(z: number, x: number, y: number, bytes: Uint8Array, bbox: number[]): void {
    NativeLib.nativeSetTerrainTile(this.nativeHandle, z, x, y, bytes, bbox);
    this.requestRender();
  }

  /** 清空全部三维地形缓存（换地形源 / 关闭地形时调用）；清空后地表回到零高程 */
  clearTerrain(): void {
    NativeLib.nativeClearTerrain(this.nativeHandle);
    this.requestRender();
  }
  /**
   * @Jin 配置 C++ CesiumTerrainProvider。
   * @Jin 宿主只提供地址、缓存目录、并发与请求头；中心优先视口调度、下载、去重、重试和注入均由 native 完成。
   */
  configureTerrainProvider(baseUrl: string, cacheDir: string, maxZoom: number,
    concurrency: number, tileRadius: number, headers: Record<string, string>): boolean {
    return NativeLib.nativeConfigureTerrainProvider(this.nativeHandle, baseUrl, cacheDir,
      maxZoom, concurrency, tileRadius, headers);
  }

  /** @Jin 开关 C++ 地形 Provider；关闭时 native 同步清空 DEM。 */
  setTerrainProviderEnabled(enabled: boolean): boolean {
    return NativeLib.nativeSetTerrainProviderEnabled(this.nativeHandle, enabled);
  }

  /** @Jin 请求 C++ Provider 按当前相机视口刷新；连续请求在 native 内自动合并。 */
  refreshTerrainProvider(): void {
    NativeLib.nativeRefreshTerrainProvider(this.nativeHandle);
  }

  /** @Jin 后台预取 layer.json，不要求地形已开启。 */
  prefetchTerrainProviderMetadata(): void {
    NativeLib.nativePrefetchTerrainProviderMetadata(this.nativeHandle);
  }

  /** @Jin 读取 C++ Provider 诊断 JSON，用于 hilog 核实磁盘命中/网络请求/去重/失败。 */
  terrainProviderStatus(): string {
    return NativeLib.nativeTerrainProviderStatus(this.nativeHandle);
  }

  /** @Jin 地形事件宿主监听器；payload 为 C++ 已序列化的单行 JSON。 */
  private terrainProviderEventListener?: (payload: string) => void;

  /**
   * @Jin 注册/替换统一地形事件监听；传 undefined 反注册。
   * @Jin 事件包括 metadata_started/metadata_ready/refresh_started/first_tile/progress/
   * refresh_completed/error/enabled/disabled，每条均带完整 stats。
   */
  setOnTerrainProviderEventListener(listener?: (payload: string) => void): void {
    this.terrainProviderEventListener = listener;
    NativeLib.nativeSetTerrainProviderEventCallback(this.nativeHandle, listener ?? null);
  }

  /**
   * 移除一个瓦片图源图层（index 为 addTileLayer / addRasterLayer 的加入次序，0 起）：墓碑—native
   * 置 removed + 隐藏，不 erase 底层容器，其它层 index 保持稳定；其瓦片纹理由 GL 线程下一帧释放
   * （其它层纹理原封不动），后台加载队列即时清空。宿主删除栅格图层时可调本函数就地生效，不必
   * 再 recreate 整个界面（recreate 会销毁 GL 上下文，使底图全部瓦片重解码上传 → 可见闪动）。
   * 越界时 native 忽略；磁盘瓦片缓存不删（下次加入同一文件秒命中）；内部触发一帧重绘。
   */
  removeTileLayer(index: number): void {
    NativeLib.nativeRemoveTileLayer(this.nativeHandle, index);
    this.requestRender();
  }

  /**
   * 添加本地栅格图层（tif/img 等 GDAL 可打开格式）：native 内建 GDAL 重投影按墨卡托瓦片
   * 四至即时生成（磁盘缓存未命中时回写 cacheDir 复用）；绘制在底图之上、矢量层之下。
   * 返回图层 index（与 addTileLayer 共享索引空间）；打不开返回 -1。
   */
  addRasterLayer(cacheDir: string, path: string): number {
    const index = NativeLib.nativeAddRasterLayer(this.nativeHandle, cacheDir, path);
    this.requestRender();
    return index;
  }

  /**
   * 最近一次 [addRasterLayer] 失败的原因（中文，含文件线索，如「GDAL 无法打开该文件…」「无地理参考…」）：
   * 成功或从未失败返回 null。纯查询、可主线程调用，供宿主 Toast 向用户反馈具体失败原因。
   */
  rasterAddError(): string | null {
    return NativeLib.nativeGetRasterAddError(this.nativeHandle);
  }

  /**
   * 添加一个「全幅加载」栅格图层（tif/img 等）：native 后台线程整幅直读为单张贴图（原生分辨率，仅在超过
   * 设备 GL_MAX_TEXTURE_SIZE/内存帧时沿 overviews 等比降采样），按其 WGS84 四至贴面绘制（2D/3D 均支持）。
   * 主线程零 GDAL、立即返回（区别于 [addRasterLayer] 的同步读四至，大文件不卡界面）。返回新图层索引
   * （0 起，独立于 [addRasterLayer] 的索引空间，供 [setRasterImageVisible] / [rasterImageError]）；
   * 句柄已释放或路径为空返回 -1。图层加入后不删除，只经可见性开关显隐。
   * [cacheDir] 非空时启用全幅 sidecar 磁盘缓存（未压缩 raw RGBA，与 [addRasterLayer] 瓦片缓存共用同一
   * 目录，按原文件名称命名）：命中时二次启动免 GDAL 秒级就绪；传空串禁用。
   */
  addRasterImageLayer(cacheDir: string, path: string): number {
    const index = NativeLib.nativeAddRasterImageLayer(this.nativeHandle, cacheDir, path);
    this.requestRender();
    return index;
  }

  /**
   * 设置指定全幅栅格图层可见性（index 为 [addRasterImageLayer] 的加入次序，0 起）：隐藏层不绘制、
   * 不计入 [hasRasterLoading]。index 越界时 native 忽略；内部触发一帧重绘使变更立即生效。
   */
  setRasterImageVisible(index: number, visible: boolean): void {
    NativeLib.nativeSetRasterImageVisible(this.nativeHandle, index, visible);
    this.requestRender();
  }

  /**
   * 可见全幅栅格层是否仍有未完成的整幅加载/纹理上传：纯状态查询、不触发重绘，可主线程调用，
   * 供「加载中」提示轮询显隐（与 [hasVectorLoading] 并联使用）。隐藏层不计入；句柄已释放返回 false。
   */
  hasRasterLoading(): boolean {
    return NativeLib.nativeHasRasterLoading(this.nativeHandle);
  }

  /**
   * 取指定全幅栅格层的加载失败原因（中文，如「文件不存在或已被移动」「数据跨 180° 经线…」）：
   * 未失败/越界/已成功加载返回 null。纯查询，供加载胶囊消失后 Toast 反馈失败图层。
   */
  rasterImageError(index: number): string | null {
    return NativeLib.nativeGetRasterImageError(this.nativeHandle, index);
  }

  /**
   * 添加矢量图层（shp/kml/kmz/dwg/dxf 等 OGR 可读格式）：native 直接读文件、重投影 WGS84、
   * earcut 剖分并异步上传 VBO；绘制在底图之上、注记 overlay 之下。样式由 [VectorStyle] 承载；
   * iconArgb 非空时（w/h 配套）为 ARGB 像素图标 billboard，为空点要素回退固定圆。
   * extent 非空时按该范围只加载屏内要素；maxFeatures≤0 取 native 硬上限。返回图层 index，失败 -1。
   */
  addVectorLayer(path: string, style: VectorStyle,
    iconArgb: number[] | undefined = undefined, iconW: number = 0, iconH: number = 0,
    extent: VectorExtent | undefined = undefined, maxFeatures: number = 0): number {
    const index = NativeLib.nativeAddVectorLayer(this.nativeHandle, path,
      style.fillColor, style.outlineColor, style.outlineWidth,
      style.lineColor, style.lineWidth, style.pointColor, style.pointRadiusDp,
      style.fillExplicit, style.lineExplicit,
      style.labelField, style.labelColor, style.labelSize,
      style.labelOutline, style.labelOutlineColor,
      style.featureStyleOverride,
      iconArgb, iconW, iconH,
      extent !== undefined && extent !== null,
      extent !== undefined && extent !== null ? extent.minLon : 0.0,
      extent !== undefined && extent !== null ? extent.minLat : 0.0,
      extent !== undefined && extent !== null ? extent.maxLon : 0.0,
      extent !== undefined && extent !== null ? extent.maxLat : 0.0,
      maxFeatures);
    this.requestRender();
    return index;
  }

  /** 按新屏幕范围重载指定矢量层（Swap-on-ready 无空窗）；extent 空走整文件全量 */
  updateVectorExtent(index: number, extent: VectorExtent | undefined, maxFeatures: number = 0): void {
    const has: boolean = extent !== undefined && extent !== null;
    // 显式可空三元（ArkTS 不经布尔变量收窄类型）；口径同 addVectorLayer
    const minLon: number = extent !== undefined && extent !== null ? extent.minLon : 0.0;
    const minLat: number = extent !== undefined && extent !== null ? extent.minLat : 0.0;
    const maxLon: number = extent !== undefined && extent !== null ? extent.maxLon : 0.0;
    const maxLat: number = extent !== undefined && extent !== null ? extent.maxLat : 0.0;
    NativeLib.nativeUpdateVectorExtent(this.nativeHandle, index, has,
      minLon, minLat, maxLon, maxLat, maxFeatures);
    this.requestRender();
  }

  /** 文件矢量层是否仍有未完成加载/上传（纯状态查询不触发重绘，供「加载中」轮询） */
  hasVectorLoading(): boolean {
    return NativeLib.nativeHasVectorLoading(this.nativeHandle);
  }

  /** 设置矢量标注字体文件路径（空则 native 自动探测系统 CJK 字体）；宜在添加矢量图层前调一次 */
  setFontPath(path: string): void {
    NativeLib.nativeSetFontPath(this.nativeHandle, path);
    this.requestRender();
  }

  /** 设置指定矢量图层可见性（index 为 addVectorLayer 加入次序）：隐藏层不绘制 */
  setVectorLayerVisible(index: number, visible: boolean): void {
    NativeLib.nativeSetVectorLayerVisible(this.nativeHandle, index, visible);
    this.requestRender();
  }

  /** 设置文件矢量层级别可见性下限：相机显示级别 < minLevel 整层隐藏；≤0 不限 */
  setVectorMinLevel(index: number, minLevel: number): void {
    NativeLib.nativeSetVectorMinLevel(this.nativeHandle, index, minLevel);
    this.requestRender();
  }

  /** 当前相机显示级别（与矢量级别可见性同口径）；句柄无效返回 0 */
  cameraZoomLevel(): number {
    return NativeLib.nativeGetCameraZoomLevel(this.nativeHandle);
  }

  /** 移除矢量图层（墓碑，GL 线程下一帧回收）：其它层 index 保持稳定，供样式变更就地换层 */
  removeVectorLayer(index: number): void {
    NativeLib.nativeRemoveVectorLayer(this.nativeHandle, index);
    this.requestRender();
  }

  /**
   * 新建动态叠加层（运行时内存几何：测量/拍照/轨迹/业务叠加），返回 index（与 addVectorLayer
   * 共享索引空间）；样式同矢量层但 labelField 不生效（标注经 updateOverlay* 逐要素传入）。
   */
  addOverlayLayer(style: VectorStyle,
    iconArgb: number[] | undefined = undefined, iconW: number = 0, iconH: number = 0): number {
    const index = NativeLib.nativeAddOverlayLayer(this.nativeHandle,
      style.fillColor, style.outlineColor, style.outlineWidth,
      style.lineColor, style.lineWidth, style.pointColor, style.pointRadiusDp,
      style.labelColor, style.labelSize, style.labelOutline, style.labelOutlineColor,
      iconArgb, iconW, iconH);
    this.requestRender();
    return index;
  }

  /** 更新叠加点要素（覆盖式）：lonlat 摊平 [lon,lat,...]；fids/labels 与点一一对应 */
  updateOverlayPoints(index: number, lonlat: number[],
    fids: number[] | undefined = undefined, labels: string[] | undefined = undefined): void {
    NativeLib.nativeUpdateOverlayPoints(this.nativeHandle, index, lonlat, fids, labels);
    this.requestRender();
  }

  /** 更新叠加线要素（覆盖式）：vertexCounts[f] 为第 f 条线顶点数 */
  updateOverlayLines(index: number, lonlat: number[], vertexCounts: number[],
    fids: number[] | undefined = undefined, labels: string[] | undefined = undefined): void {
    NativeLib.nativeUpdateOverlayLines(this.nativeHandle, index, lonlat, vertexCounts, fids, labels);
    this.requestRender();
  }

  /** 更新叠加面要素（覆盖式）：ringVertexCounts 每环顶点数；ringsPerFeature 每面环数（首环外环余洞） */
  updateOverlayPolygons(index: number, lonlat: number[], ringVertexCounts: number[],
    ringsPerFeature: number[], fids: number[] | undefined = undefined,
    labels: string[] | undefined = undefined): void {
    NativeLib.nativeUpdateOverlayPolygons(this.nativeHandle, index, lonlat,
      ringVertexCounts, ringsPerFeature, fids, labels);
    this.requestRender();
  }

  /** 移除叠加层（墓碑）：仅对叠加层生效，越界忽略 */
  removeOverlayLayer(index: number): void {
    NativeLib.nativeRemoveOverlayLayer(this.nativeHandle, index);
    this.requestRender();
  }

  /** 叠加层「不参与拾取」标志：noPick 层仍绘制但 pickVector 整层跳过（瞬态高亮层用） */
  setOverlayNoPick(index: number, noPick: boolean): void {
    NativeLib.nativeSetOverlayNoPick(this.nativeHandle, index, noPick);
  }

  /** 移除全部叠加层：文件矢量层不受影响 */
  clearOverlayLayers(): void {
    NativeLib.nativeClearOverlayLayers(this.nativeHandle);
    this.requestRender();
  }

  /**
   * 拾取矢量要素：屏幕点（px，与引擎视口同口径）命中可见矢量层，命中返回 [PickResult]，未命中 undefined。
   * 先动态叠加层后文件层，各阶段内后加入优先；noPick 层跳过。仅读 native CPU 几何，可主线程调用。
   */
  pickVector(sxPx: number, syPx: number): PickResult | undefined {
    const hit = NativeLib.nativePickVector(this.nativeHandle, sxPx, syPx);
    if (hit === null || hit.length < 2) {
      return undefined;
    }
    return new PickResult(Math.trunc(hit[0]), hit[1]);
  }

  /** 屏幕点（px，与引擎视口同口径）→ 地理坐标 [Position]；视口未就绪或句柄已释放返回 undefined */
  screenToGeo(sxPx: number, syPx: number): Position | undefined {
    const geo = NativeLib.nativeScreenToGeo(this.nativeHandle, sxPx, syPx);
    if (geo === null || geo.length < 2) {
      return undefined;
    }
    return new Position(geo[1], geo[0]);
  }

  /** 取指定层某 FID 要素的经纬度几何（供点击选中后画高亮叠加层）；未命中返回 undefined */
  featureGeometry(layerIndex: number, fid: number): FeatureGeometry | undefined {
    const res = NativeLib.nativeFeatureGeometry(this.nativeHandle, layerIndex, fid);
    if (res === null || res.length < 4 || res[0].length === 0) {
      return undefined;
    }
    const ringCounts: number[] = res[1].map((v: number) => Math.trunc(v));
    const ringsPerFeature: number[] = res[2].map((v: number) => Math.trunc(v));
    return new FeatureGeometry(Math.trunc(res[0][0]), res[3], ringCounts, ringsPerFeature);
  }

  /** 设置定位标记（蓝点）地理坐标、可见性与方位角（heading<0 不画箭头）；所有图层之上屏幕固定尺寸 */
  setLocationMarker(position: Position, visible: boolean, headingDeg: number = -1.0): void {
    NativeLib.nativeSetLocationMarker(this.nativeHandle,
      position.longitude, position.latitude, visible, headingDeg);
    this.requestRender();
  }

  /** 设置定位标记罗盘图标（ARGB 像素 + 尺寸）：有图标时画纹理四边形替代蓝点 */
  setLocationMarkerIcon(iconArgb: number[], iconW: number, iconH: number): void {
    NativeLib.nativeSetLocationMarkerIcon(this.nativeHandle, iconArgb, iconW, iconH);
    this.requestRender();
  }

  /** 请求一帧重绘（surface 未就绪时 native 忽略；对齐 Kotlin requestRender WHEN_DIRTY 语义） */
  requestRender(): void {
    NativeLib.nativeRequestRender(this.nativeHandle);
  }

  // ── 组件生命周期与渲染接线 ──

  aboutToAppear(): void {
    // 创建 native GlobeEngine 实例（对齐 Kotlin init 中 nativeCreate；surface 就绪前即可设置相机/图层）
    this.nativeHandle = NativeLib.nativeCreate();
    if (this.nativeHandle !== 0) {
      NativeLib.nativeSetDisplayDensity(this.nativeHandle, this.density);
    }
    if (this.onReady !== undefined) {
      this.onReady(this);
    }
  }

  aboutToDisappear(): void {
    this.gestures.cancelFling();
    if (this.nativeHandle !== 0) {
      // detach：注销 XComponent 回调 + 停 EGL 渲染线程（GL 资源线程内释放）；
      // destroy：删除 C++ 实例。数据态在此过程中安全收尾（对齐 Android onDetachedFromWindow + destroy）
      NativeLib.nativeDetach(this.nativeHandle);
      const h = this.nativeHandle;
      this.nativeHandle = 0;
      NativeLib.nativeDestroy(h);
    }
  }

  /** XComponent onLoad：surface 已建，取 surfaceId + 尺寸交给 native 直建 OHNativeWindow 起渲染线程 */
  private onXComponentLoad(): void {
    if (this.nativeHandle === 0) {
      return;
    }
    const surfaceId: number = Number(this.xcController.getXComponentSurfaceId());
    const rect: SurfaceRect = this.xcController.getXComponentSurfaceRect();
    const w: number = rect !== undefined && rect !== null ? Math.round(rect.surfaceWidth) : 0;
    const h: number = rect !== undefined && rect !== null ? Math.round(rect.surfaceHeight) : 0;
    if (!NativeLib.nativeAttach(this.nativeHandle, surfaceId, w, h)) {
      console.error(`NativeMapView: nativeAttach failed (surfaceId=${surfaceId} ${w}x${h})`);
      return;
    }
    this.requestRender(); // 触发首帧（对齐 Kotlin init 尾部 requestRender）
  }

  /** 触摸转发：ArkUI TouchObject.x/y 已是组件内局部坐标，直接透传给手势识别器 */
  private handleTouch(event: TouchEvent): boolean {
    if (this.nativeHandle === 0) {
      return false;
    }
    return this.gestures.onTouchEvent(event);
  }

  build() {
    XComponent({
      id: this.xcId,
      type: XComponentType.SURFACE,
      libraryname: 'globecore',
      controller: this.xcController,
    })
      .width('100%')
      .height('100%')
      .onLoad(() => {
        this.onXComponentLoad();
      })
      .onTouch((event: TouchEvent) => {
        this.handleTouch(event);
      })
      .onAreaChange((oldValue: Area, newValue: Area) => {
        // 组件在窗口内的偏移（vp）：TouchEvent 视口坐标 → 本地坐标换算用（全屏布局恒 0）
        const offset = newValue.position as Position2D;
        this.viewOffsetX = typeof offset.x === 'number' ? offset.x : 0;
        this.viewOffsetY = typeof offset.y === 'number' ? offset.y : 0;
        // 组件尺寸（vp）：tilt 灵敏度 180°/视图高 换算用
        this.viewHeight = typeof newValue.height === 'number' ? newValue.height : 0;
      })
  }
}
