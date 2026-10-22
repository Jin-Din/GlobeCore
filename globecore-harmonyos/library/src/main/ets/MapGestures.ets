// MapGestures.ets —— 地图手势识别器（对应 Android MapGestures.kt，行为逐项对齐）。
//
// Android 版基于 GestureDetector/ScaleGestureDetector/OverScroller/Choreographer；
// ArkUI 无对应检测器，本类基于 TouchEvent 多指坐标手写识别，输出与 GestureSink 接口不变：
//  - 单指拖动平移（起手 8vp 内不触发，对齐 touchSlop）；
//  - 双指捏合缩放（逐帧距离比，以两指质心为锚）；
//  - 双指旋转（两指连线 atan2 角逐帧增量，3D 门控在 sink 侧，同 Android）；
//  - 双指纵向俯仰 tilt（质心 Y 增量按 参考实现 口径 180°/视图高 换算，仅 3D；3D 下双指拖动让位 tilt 不平移）；
//  - 双击放大 2 倍（300ms/30vp 窗口）；单击透出（300ms 确认期避开双击，供拾取）；
//  - 松手惯性滑行（近 80ms 采样求初速，setTimeout 逐帧 ×0.95 帧衰减平移，
//    对齐 OverScroller 默认摩擦的减速手感）。
// 只负责「识别 + 调度」，不持有 native 句柄：全部动作经 sink 回调落地。

/** 手势识别结果的接收端（NativeMapView 实现，转调 native 相机操作并请求重绘） */
export interface GestureSink {
  /** native 实例是否可用（句柄已释放时手势不再驱动 native） */
  isReady(): boolean;

  /** 平移 dx/dy（vp，符号已折算为内容移动方向） */
  panBy(dxVp: number, dyVp: number): void;

  /** 以 (focusX, focusY) 为锚缩放：scaleFactor > 1 为放大 */
  zoomBy(scaleFactor: number, focusX: number, focusY: number): void;

  /** 双指旋转：相机 heading 累加增量（度），仅 3D 由实现方门控消费 */
  rotateBy(deltaDeg: number): void;

  /** 当前是否 3D 透视模式（双指俯仰/旋转门控 + 3D 下双指拖动让位 tilt） */
  is3DMode(): boolean;

  /** 视图高（vp）：供 tilt 按 参考实现 口径 180°·Δcy/视图高 换算俯仰量（与 TouchEvent 坐标同单位） */
  viewHeightVp(): number;

  /** 双指纵向拖拽：相机 tilt 累加增量（度，正=向地平线倾视），仅 3D 由实现方门控消费 */
  tiltBy(deltaDeg: number): void;

  /** 请求重绘一帧 */
  requestRender(): void;
}

/** 触点本地坐标（vp） */
interface Position2D {
  x: number;
  y: number;
}

interface FingerSample {
  t: number;
  x: number;
  y: number;
}

export class MapGestures {
  /** 单击（非双击/拖拽）确认回调：透出视图内屏幕坐标（vp），供宿主矢量要素拾取 */
  onTap: ((x: number, y: number) => void) | undefined = undefined;

  /** 长按（单指保持按住、未越过拖动阈值达长按时长）回调：透出视图内坐标（vp），
   *  供宿主弹落点操作菜单等；口径对齐 Android MapGestures.onLongPress（兜底通路，
   *  主路径在 native XComponentBridge 长按候选→NAPI 回调） */
  onLongPress: ((x: number, y: number) => void) | undefined = undefined;

  private sink: GestureSink;

  // ── 拖动 / 单击 / 双击 ──
  private fingers: Map<number, Position2D> = new Map();
  private downX: number = 0;
  private downY: number = 0;
  /** @Jin 单指拖动的“上一帧位置”，独立于速度采样 samples，专用于消除多指→单指切换残留大位移 */
  private lastSingleX: number = 0;
  private lastSingleY: number = 0;
  private hasLastSingle: boolean = false;
  /** @Jin 多指→单指切换后的“宽限帧”计数：>0 期间用更严的单步阈值拦截切换残留，逐帧递减到 0 */
  private singleGraceFrames: number = 0;
  /** @Jin 单帧平移跳变阈值(vp)：一帧(~16ms)真实手指位移不可能超过此值；超过即判定为多指切换残留，丢弃该帧 */
  // @Jin 单帧跟手拖动的物理上限(vp)：60fps 下快速拖动一般 <80vp；100 以上基本是多指切换残留。
  // 仅作用于手动拖动路径；惯性 fling 走独立 sink.panBy（另有速度 6000vp/s + dt 32ms 双钳），不受此值影响。
  private readonly panJumpVp: number = 100;
  private downTime: number = 0;
  private dragging: boolean = false;
  private lastTapX: number = 0;
  private lastTapY: number = 0;
  private lastTapTime: number = 0;
  private singleTapTimer: number = -1;

  // ── 长按（单指保持按住未移动达时长）──
  // 长按触发后不另设消费标志：单击仅对 duration<doubleTapTimeout 的短按生效（长按后的
  // 抬指天然不过单击门槛），与 Android GestureDetector（长按后不再发 onSingleTapConfirmed）同口径。
  private longPressTimer: number = -1;

  // ── 捏合 / 旋转 / 俯仰（双指）──
  private pinchInProgress: boolean = false;
  /** @Jin 序列级「本次触摸是否发生过双指捏合」：在双指同时抬起（无降单指中间帧）时，
   *  pinchInProgress 会随手指<2 立即清零，导致全抬判定误以为是单指手势而触发 fling，
   *  把张手/捏手末速度当成平移惯性 → 放大上跳、缩小下跳。用本标志在全抬处门控 fling/点击。 */
  private pinchHappened: boolean = false;
  private lastPinchDist: number = 0;
  /** 两指连线 atan2 角（弧度）；非双指态 NaN，POINTER_DOWN 建基准、UP 复位，防丢帧跳变 */
  private lastPinchAngleRad: number = Number.NaN;
  /** 两指质心 Y（vp）：tilt 手势纵向基准 */
  private lastPinchCentroidY: number = Number.NaN;
  /** 俯仰灵敏度采用 参考实现 口径：tiltDelta = 180° · ΔcyVp / 视图高（见 onPinchMove），不再用自定的
   * 固定「度/vp」常量（旧 0.15/0.25 为拍脑袋值，与 参考实现的 180°/屏高不一致） */
  /** 旋转/俯仰解耦阈值（度/事件）：双指连线角逐帧增量绝对值超此值判为「旋转手势」，本帧不据质心 Y 调仰角 */
  private readonly rotateSuppressTiltDeg: number = 0.8;

  // ── 惯性滑行 ──
  private samples: FingerSample[] = [];
  private flingRunning: boolean = false;
  private flingVx: number = 0;
  private flingVy: number = 0;
  private lastFlingTs: number = 0;

  // 识别阈值（vp/ms）
  private readonly touchSlop: number = 8;
  private readonly doubleTapTimeout: number = 300;
  /** 长按时长（ms）：对齐 native XComponentBridge 的 kLongPressTimeoutNs（500ms）与系统默认 */
  private readonly longPressTimeout: number = 500;
  private readonly doubleTapSlop: number = 30;
  private readonly flingMinVelocity: number = 150; // vp/s
  private readonly flingFrictionPerFrame: number = 0.95;

  constructor(sink: GestureSink) {
    this.sink = sink;
  }

  /**
   * 触摸事件入口（NativeMapView 的 onTouchEvent 转发，坐标已折算为视图本地 vp）。
   * 双指旋转/俯仰在分发前自行识别（与捏合/平移共用同一事件序列互不排斥），
   * 行为对齐 Android 版 onTouchEvent 的手动检测段。
   */
  onTouchEvent(event: TouchEvent): boolean {
    switch (event.type) {
      case TouchType.Down:
        this.onFingerDown(event);
        break;
      case TouchType.Move:
        this.onFingerMove(event);
        break;
      case TouchType.Up:
      case TouchType.Cancel:
        this.onFingerUp(event, event.type === TouchType.Cancel);
        break;
      default:
        break;
    }
    return true;
  }

  /** 停掉惯性滑行的逐帧回调（视图销毁/手势重按时调用） */
  cancelFling(): void {
    this.flingRunning = false;
    this.clearLongPressTimer(); // 组件销毁等终止场景同步撤销长按候选，防定时器泄漏
  }

  // ── 内部实现 ──

  private onFingerDown(event: TouchEvent): void {
    this.registerFingers(event);
    this.cancelFling(); // 按下即停住惯性滑行，交回手动控制
    if (this.fingers.size === 1) {
      this.pinchHappened = false; // @Jin 新触摸序列：清上一手势遗留的捏合标记
      const f = this.firstFinger();
      this.downX = f.x;
      this.downY = f.y;
      this.downTime = event.timestamp;
      this.dragging = false;
      this.samples = [];
      this.pushSample(event.timestamp, f.x, f.y);
      this.resetSingleBaseline(f.x, f.y); // @Jin 同步单指拖动基准
      this.scheduleLongPress(f.x, f.y); // 单指按下装载长按候选（移动/抬指/双指时取消）
    } else if (this.fingers.size === 2) {
      // 第二指落下：以本帧两指连线角/距离/质心 Y 为旋转、捏合与俯仰基准
      this.clearLongPressTimer(); // 双指不是长按
      this.startPinch(event);
    }
  }

  private onFingerMove(event: TouchEvent): void {
    const prevCount: number = this.fingers.size; // @Jin registerFingers 之前的在手指数
    this.registerFingers(event);
    if (this.fingers.size >= 2) {
      this.onPinchMove(event);
      return;
    }
    // @Jin 【缩放抬手跳变修复·关键】多指捏合降为单指后的第一帧 Move：剩余手指相对其捏合前
    // 位置可能已移动数百 vp，若此刻参与单指平移会产生一次横跨整个捏合过程的大 pan（真机实测
    // dx=125 dy=525，纬度瞬间跳 0.18°，即“放大上跳/缩小下跳”）。这里在“移动事件”处（坐标以
    // 当前剩余手指为准，最可靠）原地重建基准并直接返回，首帧绝不产生平移。
    if (prevCount >= 2 && this.fingers.size === 1) {
      // @Jin 以事件实际给出的唯一触点为准（不依赖 Map 迭代序），统一重建全部基准，首帧不 pan
      const rf0: Position2D = event.touches.length > 0
        ? { x: event.touches[0].x, y: event.touches[0].y } as Position2D
        : this.firstFinger();
      this.downX = rf0.x;
      this.downY = rf0.y;
      this.downTime = event.timestamp;
      this.dragging = false;
      this.samples = [];
      this.pushSample(event.timestamp, rf0.x, rf0.y);
      this.resetSingleBaseline(rf0.x, rf0.y, 4); // @Jin 转换后4帧严阈值，拦第一帧残留
      return;
    }
    // 单指：平移
    const f = this.firstFinger();
    if (!this.dragging) {
      const slop = Math.abs(f.x - this.downX) + Math.abs(f.y - this.downY);
      if (slop < this.touchSlop) {
        this.pushSample(event.timestamp, f.x, f.y);
        return;
      }
      this.dragging = true;
      this.clearLongPressTimer(); // 已越过拖动阈值，不再是长按
    }
    // @Jin 拖动位移以独立的上一帧单指位置 lastSingle 计算，不使用可能在捏合期陈旧的 samples；
    // 并做单帧跳变钳制：多指→单指切换若残留一次大位移（真机实测可达 600+vp，即缩放后猛跳），
    // 直接丢弃该帧、把基准钉到当前点，下一帧恢复正常，保证任何事件时序下都不会猛跳。
    if (!this.hasLastSingle) {
      this.resetSingleBaseline(f.x, f.y);
    }
    let dx: number = f.x - this.lastSingleX;
    let dy: number = f.y - this.lastSingleY;
    // @Jin 刚从多指切来的前几帧用更严阈值（60vp）：真机日志显示残留步 120~196px 会穿过
    // 常规 100vp 与 native 200px 之间的空档；正常跟手一帧仅几~几十 vp，不会误杀。
    const stepLimit: number = this.singleGraceFrames > 0 ? 60 : this.panJumpVp;
    if (this.singleGraceFrames > 0) {
      this.singleGraceFrames -= 1;
    }
    const jump: boolean = Math.abs(dx) > stepLimit || Math.abs(dy) > stepLimit;
    if (jump) {
      // @Jin 判定为多指切换残留/异常跳帧：不产生平移，重建基准与速度采样
      this.downX = f.x;
      this.downY = f.y;
      this.downTime = event.timestamp;
      this.samples = [];
      this.pushSample(event.timestamp, f.x, f.y);
      this.resetSingleBaseline(f.x, f.y, 3); // @Jin 拦到异常后再保3帧严阈值
      return;
    }
    this.lastSingleX = f.x;
    this.lastSingleY = f.y;
    this.pushSample(event.timestamp, f.x, f.y);
    if ((dx !== 0 || dy !== 0) && this.sink.isReady()) {
      this.sink.panBy(dx, dy);
      this.sink.requestRender();
    }
  }

  private onFingerUp(event: TouchEvent, canceled: boolean): void {
    // @Jin 【依次抬仍轻微跳·根因】绝不能用 Up 事件的 event.touches 全量重建手指表：
    // 真机上 Up 事件的 touches 常常不含“仍按住的另一指”（只回传变化的指），全量重建会把
    // 剩余指丢掉，导致 remaining=null、降单指基准不重置，下一帧用捏合期陈旧坐标算出残余平移。
    // Down/Move 已持续把每指最新坐标维护进 fingers，故 Up 只需从中删除本次抬起的 id。
    const up = event.changedTouches.length > 0 ? event.changedTouches[0] : null;
    if (up !== null) {
      for (let k = 0; k < event.changedTouches.length; k++) {
        this.fingers.delete(event.changedTouches[k].id);
      }
    } else {
      this.fingers.clear();
    }
    // @Jin 剩余指直接取删除后的手指表（位置为最近一次 Move 的可靠坐标）；显式迭代器规避
    // ArkTS 严格流分析把 forEach 闭包内赋值锁成 never。
    let remPos: Position2D | null = null;
    const fit = this.fingers.values();
    for (let step = fit.next(); !step.done; step = fit.next()) {
      remPos = step.value as Position2D;
      break;
    }
    const remaining: Position2D | null = remPos;
    if (this.fingers.size < 2) {
      // @Jin 先记下“本次抬起是否正在结束一次双指手势”，下面置 false 后仍要用它判降单指重置。
      const pinchJustEnded: boolean = this.pinchInProgress;
      this.pinchInProgress = false;
      this.lastPinchAngleRad = Number.NaN;
      this.lastPinchCentroidY = Number.NaN;
      // @Jin 【缩放后视点自动下移修复】双指捏合期间 onPinchMove 不更新单指平移采样(samples)。
      // 抬起一指、由双指降为单指的瞬间，剩余手指通常已相对其“捏合前”的位置移动了一段，
      // 若不重置基准，剩余手指的第一帧 Move 会拿捏合前的旧 lastSample 求 dx/dy，
      // 产生一次横跨整个捏合过程的大 panBy，把地图猛推（常见握持下表现为视点自动向下）。
      // 处理：把剩余手指视为一次“新的按下”，以其当前位置重建平移基准与速度采样，丢弃捏合期位移。
      if (pinchJustEnded && remaining !== null) {
        // @Jin 已进入新的单指阶段：用“真正仍按住”的手指当前坐标重置基准；随后若用户明确
        // 做单指甩动可正常 fling。真正的首帧大位移拦截在 onFingerMove 的多→单转换处完成。
        this.pinchHappened = false;
        this.downX = remaining.x;
        this.downY = remaining.y;
        this.downTime = event.timestamp;
        this.dragging = false; // 新起手须重新越过 touchSlop 才算拖动
        this.samples = [];
        this.pushSample(event.timestamp, remaining.x, remaining.y);
        this.resetSingleBaseline(remaining.x, remaining.y, 4); // @Jin 依次抬：4帧严阈值
      }
    }
    if (this.fingers.size !== 0 || canceled) {
      // 仍有手指按着（双指降单指）或事件取消：长按候选不再成立
      this.clearLongPressTimer();
      return;
    }
    // 全指抬起：先撤未触发的长按候选（抬指即取消，对齐系统 GestureDetector）
    this.clearLongPressTimer();
    // 全指抬起：判定单击/双击/惯性
    const f = up !== null ? up : this.firstFinger();
    const moved = Math.abs(f.x - this.downX) + Math.abs(f.y - this.downY);
    const duration = event.timestamp - this.downTime;
    if (!this.dragging && !this.pinchInProgress && !this.pinchHappened
      && moved < this.touchSlop && duration < this.doubleTapTimeout) {
      const sinceLast = event.timestamp - this.lastTapTime;
      const near = Math.abs(f.x - this.lastTapX) < this.doubleTapSlop
        && Math.abs(f.y - this.lastTapY) < this.doubleTapSlop;
      if (this.lastTapTime > 0 && sinceLast < this.doubleTapTimeout && near) {
        // 双击放大：以点击点为锚放大 2 倍；撤销待确认的单击
        this.clearSingleTapTimer();
        this.lastTapTime = 0;
        if (this.sink.isReady()) {
          this.sink.zoomBy(2.0, f.x, f.y);
          this.sink.requestRender();
        }
      } else {
        this.lastTapX = f.x;
        this.lastTapY = f.y;
        this.lastTapTime = event.timestamp;
        this.scheduleSingleTap(f.x, f.y);
      }
    } else {
      this.lastTapTime = 0;
      // 惯性滑动：以松手速度为初速度减速滑行（缩放进行中/3D 双指收尾不触发，对齐 Android 门控）
      const v = this.computeFlingVelocity();
      // @Jin 关键修复：刚结束双指捏合（尤其两指几乎同时抬起、无降单指中间帧）时禁止 fling，
      // 否则张手/捏手的残余末速度被当成平移惯性，造成放大上跳、缩小下跳。
      if (!this.pinchInProgress && !this.pinchHappened && v !== null && this.sink.isReady()) {
        this.startFling(v[0], v[1]);
      }
    }
    this.dragging = false;
    this.samples = [];
  }

  /** 双指序列：把当前所有触点快照进 fingers（id → vp 坐标） */
  private registerFingers(event: TouchEvent): void {
    const next: Map<number, Position2D> = new Map();
    for (let i = 0; i < event.touches.length; i++) {
      const t = event.touches[i];
      next.set(t.id, { x: t.x, y: t.y } as Position2D);
    }
    this.fingers = next;
  }

  /** @Jin 重置单指拖动的上一帧位置基准。graceFrames>0 表示刚从多指切换来，
   *  随后若干帧采用更严单步阈值，专门拦截“依次抬/同时抬”后第一帧的坐标残留。 */
  private resetSingleBaseline(x: number, y: number, graceFrames: number = 0): void {
    this.lastSingleX = x;
    this.lastSingleY = y;
    this.hasLastSingle = true;
    this.singleGraceFrames = graceFrames > 0 ? graceFrames : 0;
  }

  private firstFinger(): Position2D {
    const it = this.fingers.values();
    const v = it.next();
    return v.done ? { x: 0, y: 0 } as Position2D : v.value;
  }

  private twoFingers(): Position2D[] | null {
    if (this.fingers.size !== 2) {
      return null;
    }
    const arr: Position2D[] = [];
    this.fingers.forEach((p: Position2D) => {
      arr.push(p);
    });
    return arr;
  }

  private startPinch(event: TouchEvent): void {
    const two = this.twoFingers();
    if (two === null) {
      return;
    }
    this.pinchInProgress = true;
    this.pinchHappened = true; // @Jin 标记本序列发生过捏合，供全抬时抑制误 fling
    this.dragging = true; // 双指期间不触发单击判定
    this.samples = [];    // @Jin 双指期不做单指速度采样，清空旧样本防降单指首帧误算位移
    this.lastPinchDist = MapGestures.distance(two[0], two[1]);
    this.lastPinchAngleRad = MapGestures.angleRad(two[0], two[1]);
    this.lastPinchCentroidY = (two[0].y + two[1].y) * 0.5;
  }

  private onPinchMove(event: TouchEvent): void {
    const two = this.twoFingers();
    if (two === null) {
      return;
    }
    if (!this.pinchInProgress) {
      this.startPinch(event);
      return;
    }
    const dist = MapGestures.distance(two[0], two[1]);
    const cx = (two[0].x + two[1].x) * 0.5;
    const cy = (two[0].y + two[1].y) * 0.5;

    // 捏合缩放：逐帧距离比（factor>1 张开放大），以质心为锚
    if (dist > 0 && this.lastPinchDist > 0 && this.sink.isReady()) {
      const f: number = dist / this.lastPinchDist;
      this.sink.zoomBy(f, cx, cy);
      this.sink.requestRender();
    }
    this.lastPinchDist = dist;

    // 双指旋转：两指连线 atan2 角逐帧增量（内容跟随手指；3D 门控在 sink 侧）。先算旋转量，据其大小决定本帧是否让位仰角。
    let rotDeltaDeg = 0;
    const angle = MapGestures.angleRad(two[0], two[1]);
    if (Number.isNaN(this.lastPinchAngleRad)) {
      this.lastPinchAngleRad = angle;
    } else {
      let deltaDeg = (angle - this.lastPinchAngleRad) * 180 / Math.PI;
      this.lastPinchAngleRad = angle;
      // 跨 ±180° 环绕回跳：归一到 (-180, 180]
      if (deltaDeg > 180) {
        deltaDeg -= 360;
      }
      if (deltaDeg <= -180) {
        deltaDeg += 360;
      }
      rotDeltaDeg = deltaDeg;
      if (deltaDeg !== 0 && this.sink.isReady()) {
        this.sink.rotateBy(deltaDeg);
        this.sink.requestRender();
      }
    }

    // 双指俯仰（仅 3D）：质心 Y 逐帧增量换算倾角——上推（dy<0）向地平线倾视、下拉回正；
    // 本帧若为旋转手势（|Δ角|≥阈值）则抑制仰角（对齐 参考实现 旋转不改 tilt），仅更新基准免跳变误触发大仰角。
    if (this.sink.is3DMode()) {
      if (Number.isNaN(this.lastPinchCentroidY)) {
        this.lastPinchCentroidY = cy; // 未捕到基准（如多指转双指），本帧仅建基准
      } else {
        // 双指俯仰：质心 Y 逐帧增量按 参考实现 口径 180°·Δcy/视图高 换算（cy 与视图高同为 vp）
        const vh = this.sink.viewHeightVp();
        const tiltDelta = vh > 0 ? -(cy - this.lastPinchCentroidY) * 180.0 / vh : 0;
        this.lastPinchCentroidY = cy;
        if (Math.abs(rotDeltaDeg) < this.rotateSuppressTiltDeg && tiltDelta !== 0 && this.sink.isReady()) {
          this.sink.tiltBy(tiltDelta);
          this.sink.requestRender();
        }
      }
    }

    // 2D 下双指拖动平移让位（3D 让位 tilt）：与 Android onScroll 双指门控同口径，此处双指不平移
  }

  private pushSample(t: number, x: number, y: number): void {
    this.samples.push({ t: t, x: x, y: y } as FingerSample);
    // 仅保留近 100ms 采样
    while (this.samples.length > 2 && t - this.samples[0].t > 100) {
      this.samples.shift();
    }
  }

  private lastSample(): FingerSample {
    if (this.samples.length === 0) {
      const f = this.firstFinger();
      return { t: 0, x: f.x, y: f.y } as FingerSample;
    }
    return this.samples[this.samples.length - 1];
  }

  /** 近 80ms 位移/时长求初速（vp/s）；样本不足或过慢返回 null */
  private computeFlingVelocity(): number[] | null {
    const n = this.samples.length;
    if (n < 2) {
      return null;
    }
    const first = this.samples[0];
    const last = this.samples[n - 1];
    const dtMs = last.t - first.t;
    if (dtMs <= 0 || dtMs > 120) {
      return null;
    }
    let vx: number = (last.x - first.x) * 1000 / dtMs;
    let vy: number = (last.y - first.y) * 1000 / dtMs;
    const speed: number = Math.sqrt(vx * vx + vy * vy);
    if (speed < this.flingMinVelocity) {
      return null;
    }
    // @Jin fling 初速上限：双指手势收尾若 samples 混入了跨指/陈旧点，算出的速度会异常巨大
    // （真机日志曾出现单步平移 1643px）。物理上人手甩动速度有限，超过上限即等比压回，
    // 既消除巨幅单步跳变，也避免超长惯性。正常快速甩动（<6000vp/s）手感不受影响。
    const maxFlingSpeed: number = 6000.0;
    if (speed > maxFlingSpeed) {
      const k: number = maxFlingSpeed / speed;
      vx *= k;
      vy *= k;
    }
    return [vx, vy];
  }

  private startFling(vx: number, vy: number): void {
    this.flingVx = vx;
    this.flingVy = vy;
    this.lastFlingTs = 0;
    this.flingRunning = true;
    setTimeout(this.flingFrame, 16);
  }

  /** fling 逐帧：按帧间隔把衰减后的速度增量透传平移，直到速度低于阈值 */
  private flingFrame = (): void => {
    if (!this.flingRunning) {
      return;
    }
    const now = Date.now();
    if (this.lastFlingTs === 0) {
      this.lastFlingTs = now;
      setTimeout(this.flingFrame, 16);
      return;
    }
    let dt: number = (now - this.lastFlingTs) / 1000;
    this.lastFlingTs = now;
    // @Jin 单帧步长钳制：setTimeout 在切帧/页面节流时两次回调间隔可能达数百 ms，
    // v·dt 会一次性平移数百 vp（真机观测到孤立 dy≈620 的跳变）。把 dt 钳到 ≤32ms(≈2帧)，
    // 被节流时只少滑一点，绝不产生巨幅单步（这是缩放/拖动收尾“猛跳”的另一处直接成因）。
    if (dt > 0.032) {
      dt = 0.032;
    }
    // 摩擦衰减：按 16ms 基准帧归一（0.95/frame ≈ OverScroller 默认摩擦手感）
    const decay = Math.pow(this.flingFrictionPerFrame, dt / 0.016);
    this.flingVx *= decay;
    this.flingVy *= decay;
    if (Math.sqrt(this.flingVx * this.flingVx + this.flingVy * this.flingVy) < 20) {
      this.flingRunning = false;
      return;
    }
    if (this.sink.isReady()) {
      this.sink.panBy(this.flingVx * dt, this.flingVy * dt);
      this.sink.requestRender();
    }
    setTimeout(this.flingFrame, 16);
  };

  private scheduleSingleTap(x: number, y: number): void {
    this.clearSingleTapTimer();
    this.singleTapTimer = setTimeout(() => {
      this.singleTapTimer = -1;
      if (this.onTap !== undefined) {
        this.onTap(x, y);
      }
    }, this.doubleTapTimeout);
  }

  private clearSingleTapTimer(): void {
    if (this.singleTapTimer >= 0) {
      clearTimeout(this.singleTapTimer);
      this.singleTapTimer = -1;
    }
  }

  /** 装载长按候选：单指保持按住、未越过拖动阈值达 longPressTimeout 后触发透出 */
  private scheduleLongPress(x: number, y: number): void {
    this.clearLongPressTimer();
    this.longPressTimer = setTimeout(() => {
      this.longPressTimer = -1;
      if (this.onLongPress !== undefined) {
        this.onLongPress(x, y);
      }
    }, this.longPressTimeout);
  }

  private clearLongPressTimer(): void {
    if (this.longPressTimer >= 0) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
  }

  private static distance(a: Position2D, b: Position2D): number {
    return Math.sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
  }

  /** 两指连线 atan2 角（弧度）：屏幕 y 向下 → 该角随手指「视觉顺时针」旋转而增大 */
  private static angleRad(a: Position2D, b: Position2D): number {
    return Math.atan2(b.y - a.y, b.x - a.x);
  }
}
