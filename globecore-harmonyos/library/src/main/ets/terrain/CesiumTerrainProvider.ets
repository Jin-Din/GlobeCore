// @Jin CesiumTerrainProvider.ets —— HarmonyOS HAR 通用 Cesium quantized-mesh 地形提供器。
// @Jin
// @Jin 职责：把引擎的 Web Mercator 瓦片网格映射到 Cesium GeographicTilingScheme，
// @Jin 读取 layer.json 的可用范围与瓦片模板，完成下载、磁盘缓存和流式交付。
// @Jin 本类位于 @zys/globecore HAR，不硬编码任何数据源地址或厂商 token；
// @Jin 数据源、缓存目录、并发数与请求头均由宿主通过 TerrainSourceOptions 注入。
// @Jin
// @Jin 当前支持的数据约定：
// @Jin   projection = EPSG:4326，scheme = tms，format = quantized-mesh-1.0；
// @Jin   列数 cols = 2^(z+1)，行数 rows = 2^z；y 自南向北。
// @Jin   available 按 layer.json 原样用于请求前检查，避免越界 404 风暴。
// @Jin
// @Jin 引擎网格与服务网格不重合：一个引擎墨卡托瓦片可关联多块服务 DEM，
// @Jin native 按每块服务瓦片的真实 bbox 做覆盖采样，避免单块粗 DEM 外推造成山脊错位。
// @Jin
// @Jin P2-1 阶段 B：layer.json、available、EPSG:4326/TMS 网格换算已下沉 C++ 核心；
// @Jin ArkTS 只保留平台 HTTP、磁盘缓存与流式交付，后续阶段 C 再下沉视口调度和下载。

import { http } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { NativeLib } from '../NativeLib';

// @Jin 地形提供器日志标签；宿主可通过 hilog 过滤该标签观察元数据与失败统计。
const TAG: string = 'CesiumTerrain';
// @Jin layer.json 缺失 maxzoom 或宿主未传值时的保守默认上限。
const DEFAULT_TERRAIN_MAX_ZOOM: number = 15;
// @Jin 默认并发保持真机已验证的 4；宿主仍可按服务端限流情况覆盖。
const DEFAULT_TERRAIN_CONCURRENCY: number = 4;
// @Jin 默认请求头只保留通用值，不在 HAR 内绑定 Referer、Token 或具体服务域名。
const DEFAULT_TERRAIN_USER_AGENT: string =
  'Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36';

/**
 * @Jin 地形数据源配置：由宿主提供，HAR 不保存任何具体服务地址或授权信息。
 */
export interface TerrainSourceOptions {
  /** @Jin layer.json 所在目录，例如 https://host/path/terrain。 */
  baseUrl: string;
  /** @Jin 服务 DEM 的独立磁盘缓存目录，必须与影像瓦片目录隔离。 */
  cacheDir: string;
  /** @Jin 可选的最大服务级别；layer.json 中有 maxzoom 时以元数据为准。 */
  maxZoom?: number;
  /** @Jin 可选并发数；默认 4，服务端限流时应由宿主下调。 */
  concurrency?: number;
  /** @Jin 可选附加请求头；会覆盖默认 Accept/User-Agent 中的同名项。 */
  headers?: Record<string, string>;
}

/** @Jin layer.json 的 available 段：一段矩形瓦片范围（闭区间）。 */
interface AvailableRange {
  startX: number;
  startY: number;
  endX: number;
  endY: number;
}

/** @Jin layer.json 中地形提供器使用的字段；部分服务允许省略可选字段。 */
interface LayerJsonMeta {
  format?: string;
  scheme?: string;
  projection?: string;
  minzoom?: number;
  maxzoom?: number;
  tiles?: string[];
  available?: AvailableRange[][];
}

/** @Jin 服务瓦片的经纬度四至（度）。 */
export interface GeoBounds {
  lonMin: number;
  latMin: number;
  lonMax: number;
  latMax: number;
}

/** @Jin 一块已解析好的地形瓦片，供宿主注入 NativeMapView。 */
export interface TerrainTile {
  /** @Jin 引擎瓦片网格编号，注入时作为缓存键。 */
  z: number;
  x: number;
  y: number;
  /** @Jin quantized-mesh 原始字节。 */
  bytes: Uint8Array;
  /** @Jin 该字节对应的真实经纬度四至，由服务网格计算。 */
  bounds: GeoBounds;
}

// @Jin ── 网格数学 ─────────────────────────────────────────────

/**
 * @Jin
 * 服务（Cesium 地理网格）瓦片的经纬度四至。
 * cols = 2^(z+1)，rows = 2^z，x 自西、y 自南。
 */
export function geoTileBounds(z: number, x: number, y: number): GeoBounds {
  // @Jin 正常路径由 C++ 统一计算；native 加载失败时保留纯 JS 公式，避免 Har 初始化异常直接中断地形。
  const nativeBounds: number[] | null = NativeLib.nativeCesiumTerrainTileBounds(z, x, y);
  if (nativeBounds !== null) {
    return {
      lonMin: nativeBounds[0],
      latMin: nativeBounds[1],
      lonMax: nativeBounds[2],
      latMax: nativeBounds[3],
    };
  }
  const cols: number = Math.pow(2, z + 1);
  const rows: number = Math.pow(2, z);
  return {
    lonMin: -180.0 + 360.0 * x / cols,
    latMin: -90.0 + 180.0 * y / rows,
    lonMax: -180.0 + 360.0 * (x + 1) / cols,
    latMax: -90.0 + 180.0 * (y + 1) / rows,
  };
}

/** @Jin * 经纬度 → 服务瓦片号（列 2^(z+1)、行 2^z、y 自南） */
export function lonLatToGeoTile(z: number, lon: number, lat: number): number[] {
  const cols: number = Math.pow(2, z + 1);
  const rows: number = Math.pow(2, z);
  let tx: number = Math.floor((lon + 180.0) / 360.0 * cols);
  let ty: number = Math.floor((lat + 90.0) / 180.0 * rows);
  tx = Math.max(0, Math.min(cols - 1, tx));
  ty = Math.max(0, Math.min(rows - 1, ty));
  return [tx, ty];
}

/** @Jin * 引擎（Web Mercator）瓦片中心经纬度：用于按中心点挑服务瓦片 */
export function mercatorTileCenter(z: number, x: number, y: number): number[] {
  const n: number = Math.pow(2, z);
  const lon: number = -180.0 + 360.0 * (x + 0.5) / n;
  const yNorm: number = (y + 0.5) / n;             // 0 = 北极
  const latRad: number = Math.atan(Math.sinh(Math.PI * (1.0 - 2.0 * yNorm)));
  return [lon, latRad * 180.0 / Math.PI];
}

/** @Jin * 经纬度 → 引擎（Web Mercator）瓦片号 */
export function lonLatToMercatorTile(z: number, lon: number, lat: number): number[] {
  const n: number = Math.pow(2, z);
  let tx: number = Math.floor((lon + 180.0) / 360.0 * n);
  const latRad: number = Math.max(-1.4844, Math.min(1.4844, lat * Math.PI / 180.0));
  const yNorm: number = (1.0 - Math.log(Math.tan(latRad) + 1.0 / Math.cos(latRad)) / Math.PI) / 2.0;
  let ty: number = Math.floor(yNorm * n);
  tx = Math.max(0, Math.min(n - 1, tx));
  ty = Math.max(0, Math.min(n - 1, ty));
  return [tx, ty];
}

/**
 * @Jin 引擎瓦片级别映射到服务瓦片级别。
 * @Jin native 支持多 DEM 覆盖后使用同级服务瓦片，避免 sz=z-1 粗 DEM 插值造成山脊位置偏差。
 */
export function serviceZoomForEngineZoom(z: number, maxZoom: number = DEFAULT_TERRAIN_MAX_ZOOM): number {
  return Math.max(0, Math.min(maxZoom, z));
}


/** @Jin: 墨卡托引擎瓦片的真实经纬度范围；用于和 EPSG:4326 Cesium 地理网格做精确 bbox 求交。 */
function mercatorTileBounds(z: number, x: number, y: number): GeoBounds {
  const n: number = Math.pow(2, z);
  const lonMin: number = -180.0 + 360.0 * x / n;
  const lonMax: number = -180.0 + 360.0 * (x + 1) / n;
  const latAtNorm = (yNorm: number): number => {
    const clamped: number = Math.max(0.0, Math.min(1.0, yNorm));
    const latRad: number = Math.atan(Math.sinh(Math.PI * (1.0 - 2.0 * clamped)));
    return latRad * 180.0 / Math.PI;
  };
  // @Jin: Web Mercator 的 y=0 在北侧；yNorm 越小纬度越高，因此 latMax 对应 y/n。
  return {
    lonMin: lonMin,
    latMin: latAtNorm((y + 1) / n),
    lonMax: lonMax,
    latMax: latAtNorm(y / n),
  };
}

/** @Jin: 地理服务网格中与给定 bbox 真正有面积重叠的瓦片行列范围。 */
function intersectingGeoTileRange(sz: number, bounds: GeoBounds): number[] {
  const cols: number = Math.pow(2, sz + 1);
  const rows: number = Math.pow(2, sz);
  const lonSpan: number = 360.0 / cols;
  const latSpan: number = 180.0 / rows;
  // @Jin: 对理论上的共边向内收一个极小 epsilon，避免浮点误差把只共边、无面积重叠的邻块也请求回来。
  const epsilon: number = 1e-10;
  let x0: number = Math.floor((bounds.lonMin + 180.0 + epsilon) / lonSpan);
  let x1: number = Math.floor((bounds.lonMax + 180.0 - epsilon) / lonSpan);
  let y0: number = Math.floor((bounds.latMin + 90.0 + epsilon) / latSpan);
  let y1: number = Math.floor((bounds.latMax + 90.0 - epsilon) / latSpan);
  x0 = Math.max(0, Math.min(cols - 1, x0));
  x1 = Math.max(0, Math.min(cols - 1, x1));
  y0 = Math.max(0, Math.min(rows - 1, y0));
  y1 = Math.max(0, Math.min(rows - 1, y1));
  return [x0, y0, x1, y1];
}

// @Jin ── 提供器 ───────────────────────────────────────────────

/** @Jin 并发工作者共享的任务游标；ArkTS 不支持跨协程修改局部变量，故显式成类。 */
class Cursor {
  i: number = 0;
}

export class CesiumTerrainProvider {
  // @Jin 宿主注入的数据源根地址；HAR 内不存在任何默认服务地址。
  private baseUrl: string = '';
  // @Jin 宿主注入的独立 DEM 磁盘缓存目录。
  private cacheDir: string = '';
  // @Jin layer.json 解析出的瓦片路径模板，例如 {z}/{x}/{y}.terrain。
  private tileTemplate: string = '{z}/{x}/{y}.terrain';
  // @Jin 服务最大级别；元数据加载后会被 layer.json 的 maxzoom 修正。
  private maxZoom: number = DEFAULT_TERRAIN_MAX_ZOOM;
  // @Jin 下载并发数；由宿主按服务端限流能力配置。
  private concurrency: number = DEFAULT_TERRAIN_CONCURRENCY;
  // @Jin 每个请求复用的请求头配置。
  private headers: Record<string, string> = {};
  private metaLoaded: boolean = false;
  // @Jin C++ 元数据句柄；非 0 时 available 判断、瓦片映射和 bbox 全走共享核心。
  private coreHandle: number = 0;
  // @Jin native 不可用或 layer.json 解析失败时的 ArkTS 兼容数据，仅作为降级路径。
  private available: AvailableRange[][] = [];
  // @Jin 防止页面销毁时仍挂起的 layer.json 请求在 dispose 后重新创建 C++ 句柄。
  private disposed: boolean = false;
  /** @Jin 已下载或已确认不可用的瓦片，避免同一会话内重复请求。 */
  private failed: Set<string> = new Set<string>();
  /** @Jin 本轮已成功交付给 native 的服务 DEM；分阶段扩半径时避免重复读盘与重复解码。 */
  private delivered: Set<string> = new Set<string>();
  /** @Jin 统计成功、跳过（不可用）和失败数量。 */
  public okCount: number = 0;
  public skipCount: number = 0;
  public failCount: number = 0;

  /**
   * @Jin 创建通用 Cesium quantized-mesh 提供器。
   * @Jin 数据源与缓存策略全部从 options 注入，方便 Harmony、Android、Qt 后续共享相同配置口径。
   */
  constructor(options: TerrainSourceOptions) {
    this.baseUrl = options.baseUrl.endsWith('/')
      ? options.baseUrl.substring(0, options.baseUrl.length - 1)
      : options.baseUrl;
    this.cacheDir = options.cacheDir;
    if (options.maxZoom !== undefined) {
      this.maxZoom = Math.max(0, Math.trunc(options.maxZoom));
    }
    if (options.concurrency !== undefined) {
      this.concurrency = Math.max(1, Math.trunc(options.concurrency));
    }
    this.headers = {
      'Accept': '*/*',
      'User-Agent': DEFAULT_TERRAIN_USER_AGENT,
    };
    if (options.headers !== undefined) {
      const headerNames: string[] = Object.keys(options.headers);
      for (let i = 0; i < headerNames.length; i++) {
        const name: string = headerNames[i];
        this.headers[name] = options.headers[name];
      }
    }
    if (this.baseUrl.length === 0 || this.cacheDir.length === 0) {
      throw new Error('TerrainSourceOptions.baseUrl/cacheDir 不能为空');
    }
  }

  /**
   * @Jin 拉取 layer.json 并交给 C++ 核心解析（幂等）。
   * @Jin 正常路径不再在 ArkTS 保存 available/网格规则；仅当 native 不可用时降级到 JSON 兼容解析。
   */
  async ensureMeta(): Promise<boolean> {
    if (this.disposed) {
      return false;
    }
    if (this.metaLoaded) {
      return this.coreHandle !== 0 || this.available.length > 0;
    }
    try {
      const text: string = await this.httpGetText(`${this.baseUrl}/layer.json`);
      if (this.disposed) {
        return false;
      }
      const handle: number = NativeLib.nativeCesiumTerrainCreate(text, this.maxZoom);
      if (handle !== 0) {
        if (this.disposed) {
          NativeLib.nativeCesiumTerrainDestroy(handle);
          return false;
        }
        this.coreHandle = handle;
        const nativeMaxZoom: number = NativeLib.nativeCesiumTerrainMaxZoom(handle);
        const nativeTemplate: string = NativeLib.nativeCesiumTerrainTileTemplate(handle);
        if (nativeMaxZoom >= 0) {
          this.maxZoom = nativeMaxZoom;
        }
        if (nativeTemplate.length > 0) {
          this.tileTemplate = nativeTemplate;
        }
        this.metaLoaded = true;
        console.info(`${TAG}: layer.json ok core=cpp maxzoom=${this.maxZoom} template=${this.tileTemplate}`);
        return true;
      }

      // @Jin C++ 核心不可用时才走兼容解析；正常构建不会进入该分支。
      const meta: LayerJsonMeta = JSON.parse(text) as LayerJsonMeta;
      if (meta.format !== undefined && meta.format !== 'quantized-mesh-1.0') {
        console.warn(`${TAG}: 非预期地形格式 ${meta.format}`);
      }
      if (meta.tiles !== undefined && meta.tiles.length > 0) {
        // @Jin 兼容路径仍使用服务声明模板，支持查询参数和带版本号的 Cesium 地形源。
        this.tileTemplate = meta.tiles[0];
      }
      if (meta.maxzoom !== undefined) {
        this.maxZoom = Math.max(0, Math.trunc(meta.maxzoom));
      }
      this.available = meta.available === undefined ? [] : meta.available;
      this.metaLoaded = true;
      console.warn(`${TAG}: C++ terrain core unavailable, fallback arkts projection=${meta.projection} ` +
        `scheme=${meta.scheme} maxzoom=${this.maxZoom} levels=${this.available.length}`);
      return true;
    } catch (e) {
      console.error(`${TAG}: layer.json 加载失败，将不查 available 直接下载: ${JSON.stringify(e)}`);
      this.metaLoaded = true; // @Jin 不再重试，避免每次刷新都重复请求大体积 layer.json。
      return false;
    }
  }

  /** @Jin 判断服务瓦片是否可用；C++ 句柄存在时直接复用共享 available 解析结果。 */
  isAvailable(z: number, x: number, y: number): boolean {
    if (this.coreHandle !== 0) {
      return NativeLib.nativeCesiumTerrainIsAvailable(this.coreHandle, z, x, y);
    }
    if (z < 0 || z > this.maxZoom) {
      return false;
    }
    if (this.available.length === 0 || z >= this.available.length) {
      return true;
    }
    const ranges: AvailableRange[] = this.available[z];
    for (let i = 0; i < ranges.length; i++) {
      const r: AvailableRange = ranges[i];
      if (x >= r.startX && x <= r.endX && y >= r.startY && y <= r.endY) {
        return true;
      }
    }
    return false;
  }

  /** @Jin 按 layer.json 模板生成服务瓦片 URL，不绑定 .terrain 后缀或固定域名。 */
  tileUrl(z: number, x: number, y: number): string {
    const path: string = this.tileTemplate
      .replace('{z}', `${z}`)
      .replace('{x}', `${x}`)
      .replace('{y}', `${y}`);
    return `${this.baseUrl}/${path}`;
  }

  /**
   * @Jin 下载一块服务瓦片（带磁盘缓存与不可用记忆）。
   * @Jin 返回 null 表示服务范围外或下载失败。
   */
  async fetchTile(z: number, x: number, y: number): Promise<Uint8Array | null> {
    // @Jin 页面离开或宿主切换地形源后不再继续读取/下载旧服务瓦片。
    if (this.disposed) {
      return null;
    }
    const key: string = `${z}/${x}/${y}`;
    if (this.failed.has(key)) {
      this.skipCount++;
      return null;
    }
    if (!this.isAvailable(z, x, y)) {
      this.failed.add(key);
      this.skipCount++;
      return null;
    }
    // @Jin 先读独立磁盘缓存，命中时不产生网络请求。
    const cached: Uint8Array | null = this.readCache(z, x, y);
    if (cached !== null && cached.length > 0) {
      this.okCount++;
      return cached;
    }
    try {
      const bytes: Uint8Array | null =
        await this.httpGetBytes(this.tileUrl(z, x, y));
      if (bytes === null || bytes.length === 0) {
        this.failed.add(key);
        this.failCount++;
        return null;
      }
      this.writeCache(z, x, y, bytes);
      this.okCount++;
      return bytes;
    } catch (e) {
      this.failed.add(key);
      this.failCount++;
      return null;
    }
  }

  /**
   * @Jin 为一批引擎瓦片拉取地形（并发受控）。
   * @Jin 墨卡托渲染瓦片与 EPSG:4326 服务瓦片边界不完全重合，必须收集所有相交服务瓦片。
   * @Jin 否则 native 会把单块粗 DEM 钳制外推到错误区域。
   */
  async fetchForEngineTiles(tiles: number[][]): Promise<TerrainTile[]> {
    const out: TerrainTile[] = [];
    await this.fetchForEngineTilesStream(tiles, (tile: TerrainTile): void => {
      out.push(tile);
    });
    return out;
  }

  /**
   * @Jin: 流式拉取地形：中心区域的服务 DEM 一旦完成就立即回调给宿主注入引擎。
   * @Jin: 旧实现等待全部 DEM 下载完成后才统一 setTerrainTile，首轮冷启动会长时间只有“加载中”没有立体反馈。
   * @Jin: 调用方应尽量按中心向外排序传入 engine tiles；本方法会保持该优先级去重和派发服务瓦片。
   */
  async fetchForEngineTilesStream(
    tiles: number[][],
    onReady: (tile: TerrainTile) => void
  ): Promise<void> {
    // @Jin Provider 销毁后不再派发新请求，避免页面退出时后台继续占用网络与 DEM 缓存。
    if (this.disposed) {
      return;
    }
    const wanted: Set<string> = new Set<string>();
    const serviceTiles: number[][][] = [];
    for (let i = 0; i < tiles.length; i++) {
      const t: number[] = tiles[i];
      const z: number = t[0];
      const x: number = t[1];
      const y: number = t[2];
      const engineTile: number[] = [z, x, y];
      if (this.coreHandle !== 0) {
        // @Jin 核心路径一次返回所有与引擎 bbox 真正相交、且在 available 内的服务 DEM。
        const serviceFlat: number[] = NativeLib.nativeCesiumTerrainEngineToServiceTiles(
          this.coreHandle, z, x, y);
        for (let j = 0; j + 2 < serviceFlat.length; j += 3) {
          const sz: number = serviceFlat[j];
          const sx: number = serviceFlat[j + 1];
          const sy: number = serviceFlat[j + 2];
          // @Jin native 按服务瓦片 bbox 全局共享 DEM；同阶段按 wanted 去重，跨阶段按 delivered 跳过已交付 DEM。
          const key: string = `${sz}/${sx}/${sy}`;
          if (!wanted.has(key) && !this.delivered.has(key)) {
            wanted.add(key);
            serviceTiles.push([engineTile, [sz, sx, sy]]);
          }
        }
        continue;
      }

      // @Jin C++ 核心不可用时保留原网格求交逻辑，确保 Har 降级后仍可加载地形。
      const sz: number = serviceZoomForEngineZoom(z, this.maxZoom);
      const engineBounds: GeoBounds = mercatorTileBounds(z, x, y);
      const range: number[] = intersectingGeoTileRange(sz, engineBounds);
      const x0: number = range[0];
      const y0: number = range[1];
      const x1: number = range[2];
      const y1: number = range[3];
      for (let sx = x0; sx <= x1; sx++) {
        for (let sy = y0; sy <= y1; sy++) {
          const key: string = `${sz}/${sx}/${sy}`;
          if (!wanted.has(key) && !this.delivered.has(key)) {
            wanted.add(key);
            serviceTiles.push([engineTile, [sz, sx, sy]]);
          }
        }
      }
    }

    const cursor: Cursor = new Cursor();
    const workers: Promise<void>[] = [];
    const n: number = Math.min(this.concurrency, serviceTiles.length);
    for (let w = 0; w < n; w++) {
      workers.push(this.pumpWorker(serviceTiles, cursor, onReady));
    }
    for (let i = 0; i < workers.length; i++) {
      await workers[i];
    }
  }

  /** @Jin: 单个并发工作者从共享游标取任务；任务数组已按中心向外排序，worker 空闲时优先拿中心 DEM。 */
  private async pumpWorker(
    tiles: number[][][],
    cursor: Cursor,
    onReady: (tile: TerrainTile) => void
  ): Promise<void> {
    while (cursor.i < tiles.length) {
      const pair: number[][] = tiles[cursor.i];
      cursor.i++;
      await this.fetchOne(pair[0], pair[1], onReady);
    }
  }

  private async fetchOne(
    engineTile: number[],
    serviceTile: number[],
    onReady: (tile: TerrainTile) => void
  ): Promise<void> {
    const z: number = engineTile[0];
    const x: number = engineTile[1];
    const y: number = engineTile[2];
    const sz: number = serviceTile[0];
    const sx: number = serviceTile[1];
    const sy: number = serviceTile[2];
    const bytes: Uint8Array | null = await this.fetchTile(sz, sx, sy);
    if (bytes === null) {
      return;
    }
    // @Jin: 每块 DEM 读盘/下载成功后立刻交付，UI 层可马上 setTerrainTile，不再等待整批任务结束。
    onReady({
      z: z,
      x: x,
      y: y,
      bytes: bytes,
      bounds: geoTileBounds(sz, sx, sy),
    });
    // @Jin: 仅在 bytes 成功且回调完成后标记交付；网络失败的 DEM 允许下一轮刷新重新尝试。
    this.delivered.add(`${sz}/${sx}/${sy}`);
  }

  // @Jin ── 磁盘缓存 ──────────────────────────────────────────

  private cachePath(z: number, x: number, y: number): string {
    return `${this.cacheDir}/${z}/${x}_${y}.terrain`;
  }

  private readCache(z: number, x: number, y: number): Uint8Array | null {
    try {
      const p: string = this.cachePath(z, x, y);
      if (!fs.accessSync(p)) {
        return null;
      }
      const st: fs.Stat = fs.statSync(p);
      if (st.size <= 0) {
        return null;
      }
      const buf: ArrayBuffer = new ArrayBuffer(st.size);
      const fd: number = fs.openSync(p, fs.OpenMode.READ_ONLY).fd;
      fs.readSync(fd, buf);
      fs.closeSync(fd);
      return new Uint8Array(buf);
    } catch (e) {
      return null;
    }
  }

  private writeCache(z: number, x: number, y: number, bytes: Uint8Array): void {
    try {
      const dir: string = `${this.cacheDir}/${z}`;
      let dirExists: boolean = false;
      try {
        dirExists = fs.accessSync(dir);
      } catch (accessErr) {
        dirExists = false;
      }
      // @Jin: HarmonyOS 的 mkdirSync(..., true) 在目录已存在时仍可能抛 “File exists”；首屏批量写地形缓存时，
      // @Jin: 这些异常会产生大量 ArkCompiler 日志并拖慢地形加载，因此先 access，再只在缺失时递归建目录。
      if (!dirExists) {
        fs.mkdirSync(dir, true);
      }
      const p: string = this.cachePath(z, x, y);
      const fd: number = fs.openSync(p, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC).fd;
      fs.writeSync(fd, bytes.buffer);
      fs.closeSync(fd);
    } catch (e) {
      // @Jin: 缓存写失败不影响本次显示，下次启动或刷新时允许重新下载补齐。
    }
  }

  /** @Jin 清空统计；切换区域或级别后调用，避免旧数据干扰本轮日志。 */
  resetStats(): void {
    this.okCount = 0;
    this.skipCount = 0;
    this.failCount = 0;
  }

  /** @Jin: 新一轮地形刷新前清空已交付记忆；分阶段加载内部不会清空，避免 r2/r3 重复注入 r1 的共享 DEM。 */
  resetDeliveredTiles(): void {
    this.delivered.clear();
  }

  /**
   * @Jin 释放 C++ 元数据句柄；宿主页面销毁或释放 Provider 时必须调用。
   * @Jin 方法幂等，挂起的 layer.json 请求返回后会检测 disposed 并立即销毁迟到句柄。
   */
  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    const handle: number = this.coreHandle;
    this.coreHandle = 0;
    NativeLib.nativeCesiumTerrainDestroy(handle);
    this.delivered.clear();
    this.failed.clear();
  }

  // @Jin ── HTTP ───────────────────────────────────────────

  /** @Jin 发起文本 GET；请求头来自宿主配置，证书与超时口径与其他 HAR 网络能力保持一致。 */
  private async httpGetText(url: string): Promise<string> {
    const req: http.HttpRequest = http.createHttp();
    try {
      const res: http.HttpResponse = await req.request(url, {
        method: http.RequestMethod.GET,
        header: this.headers,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 15000,
        readTimeout: 30000,
      });
      if (res.responseCode !== 200) {
        throw new Error(`HTTP ${res.responseCode}`);
      }
      return String(res.result);
    } finally {
      req.destroy();
    }
  }

  /** @Jin 发起二进制 GET；仅 HTTP 200 且响应非空时返回 DEM 字节。 */
  private async httpGetBytes(url: string): Promise<Uint8Array | null> {
    const req: http.HttpRequest = http.createHttp();
    try {
      const res: http.HttpResponse = await req.request(url, {
        method: http.RequestMethod.GET,
        header: this.headers,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 15000,
        readTimeout: 30000,
      });
      if (res.responseCode !== 200) {
        return null;
      }
      const buf: ArrayBuffer = res.result as ArrayBuffer;
      if (buf.byteLength === 0) {
        return null;
      }
      return new Uint8Array(buf);
    } finally {
      req.destroy();
    }
  }
}
