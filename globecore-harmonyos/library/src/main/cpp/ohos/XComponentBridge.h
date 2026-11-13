// XComponentBridge —— OH_NativeXComponent  surface 回调 ↔ MapRenderHost 渲染线程接线。
//
// ArkTS 侧 XComponent({ type: XComponentType.SURFACE, libraryname: 'globecore' }) 在 onLoad 中
// 经 getContext() 拿到 native 上下文对象，NAPI 层（napi_globe_engine.cpp 的 nativeAttach）把它传给
// 本类 attach()：napi_unwrap 解出 OH_NativeXComponent*，注册 surface 创建/变化/销毁回调，
// 并绑定既有的 GlobeEngine 句柄（ArkTS 先 nativeCreate 再 attach，一个视图一个实例）。
//
// 回调时序（均在主线程投递，与 NAPI 调用同线程，host 指针无需加锁）：
//   OnSurfaceCreated  → 取 OHNativeWindow → 新建 MapRenderHost（起 EGL 渲染线程）
//   OnSurfaceChanged  → GetXComponentSize → host.setSurfaceSize(w, h)
//   OnSurfaceDestroyed → host 析构（quit+join，GL 线程内 releaseGl/EGL 销毁），GlobeEngine 保留
//   detach（节点销毁/页面退出）→ UnregisterCallback + 停 host，桥随之销毁。
// 触摸手势经本桥 DispatchTouchEvent 在 native 侧识别（SURFACE 型 XComponent 不投递 ArkTS
// .onTouch，实证见决策记录）：平移/捏合/旋转/俯仰直接驱动 engine_ 相机；惯性滑动、单击确认与
// 长按检测委托 host 渲染线程自驱动（见 EglContext.h startFling/postPendingTap/postPendingLongPress）。
#ifndef GLOBECORE_OHOS_XCOMPONENTBRIDGE_H
#define GLOBECORE_OHOS_XCOMPONENTBRIDGE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <napi/native_api.h>

#include "EglContext.h"

namespace globecore { class GlobeEngine; }

namespace gcohos {

/// 从 napi 模块 exports 的 OH_NATIVE_XCOMPONENT_OBJ 属性取 OH_NativeXComponent*
/// （实现见 napi_init.cpp；官方取法，onLoad 传入的 context 对象在 API 12+ 已不承载组件指针）。
OH_NativeXComponent *ResolveNativeXComponent(napi_env env);

class XComponentBridge {
public:
    /// 绑定 XComponent surface 并起 EGL 渲染线程。
    /// surfaceId 由 ArkTS XComponentController.getXComponentSurfaceId() 取得：native 直接用
    /// OH_NativeWindow_CreateNativeWindowFromSurfaceId 造 window，不依赖 OnSurfaceCreated（规避
    /// SURFACE 型在 onLoad 前 surface 已创建、注册回调错过该事件的竞态）。
    /// width/height 为 surface 像素尺寸（<=0 时 native 兜底取 GetXComponentSize）。
    /// 失败返回 nullptr，调用方不接管所有权。
    static XComponentBridge *attach(napi_env env, globecore::GlobeEngine *engine,
                                    uint64_t surfaceId, int32_t width, int32_t height);

    /// 注销回调并停止渲染线程（幂等）。之后对象可安全 delete。
    void shutdown();

    ~XComponentBridge();

    XComponentBridge(const XComponentBridge &) = delete;
    XComponentBridge &operator=(const XComponentBridge &) = delete;

    /// 请求一帧重绘（surface 未就绪时忽略，就绪后由 host 脏标记通路驱动）。供 NAPI 层唤醒。
    void requestRender();

    /// @Jin 单击确认回调（px 物理像素坐标，渲染线程触发；NAPI 层注册 TSFN 转投 ArkTS 主线程；旧注释误标 vp，2026-11 勘误）。
    /// 可在 attach 前后任意时刻调用：早于 attach 则缓存，attach 建 host 后补装。
    void setSingleTapCallback(std::function<void(float, float)> cb) {
        tapCb_ = std::move(cb);
        if (host_) host_->setSingleTapCallback(tapCb_);
    }

    /// @Jin 长按确认回调（px 物理像素坐标，渲染线程长按候选到点触发；NAPI 层注册 TSFN 转投 ArkTS 主线程；旧注释误标 vp）。
    /// 补装时机同 setSingleTapCallback：早于 attach 则缓存，attach 建 host 后补装。
    void setLongPressCallback(std::function<void(float, float)> cb) {
        longPressCb_ = std::move(cb);
        if (host_) host_->setLongPressCallback(longPressCb_);
    }

private:
    XComponentBridge(OH_NativeXComponent *component, globecore::GlobeEngine *engine);

    /// @Jin 长按到点回调（渲染线程经 host 触发，坐标为 px 物理像素；旧注释误标 vp）：本按下序列标记为长按，后续 UP 不再
    /// 当惯性处理（单击不否决，口径同 Android MapGestures.handleLongPress），并透传给
    /// longPressCb_（NAPI 层 TSFN 转投 ArkTS 主线程，供宿主弹落点操作菜单等）。
    void handleLongPress(float xPx, float yPx); // @Jin 参数旧名 xVp/yVp 勘误为 px

    /// 静态 trampolines：OH_NativeXComponent 回调只给 component 指针，经组件注册表反查实例。
    static void onSurfaceCreatedCB(OH_NativeXComponent *component, void *window);
    static void onSurfaceChangedCB(OH_NativeXComponent *component, void *window);
    static void onSurfaceDestroyedCB(OH_NativeXComponent *component, void *window);
    static void dispatchTouchEventCB(OH_NativeXComponent *component, void *window);

    /// 本实例的手势识别（在主线程 dispatchTouchEventCB 内驱动 engine_ 相机；等价于原 ArkTS→NAPI 通路）
    void handleTouch(const OH_NativeXComponent_TouchEvent &ev);

    // @Jin ── 手势识别状态（对齐 MapGestures.ets 语义，坐标单位为物理像素 px；旧注释误标 vp，2026-11 真机实测勘误）──
    bool gDown_ = false;      // 是否处于一次按下序列
    bool gDragging_ = false;  // 单指已越过 touchSlop（进入平移）
    bool gPinching_ = false;  // 双指捏合进行中
    bool gHadMulti_ = false;  // 本按下序列出现过双指（结束后不触发单指惯性，防捏合收尾甩图）
    float gLastX_ = 0;        // 上一帧主触点 x（单指平移增量基准）
    float gLastY_ = 0;        // 上一帧主触点 y
    float gDownX_ = 0;        // 本次按下起点（touchSlop 判定）
    float gDownY_ = 0;        // 本次按下起点
    // @Jin T9-fix2（实验/可回退）：真机 SURFACE 触摸走本 native 桥（MapGestures.ets 仅 ArkTS 兜底）。
    // 捏合降单指后 gLastX_/gLastY_ 仍停在捏合前位置，首帧单指 MOVE 会算出横跨整个捏合过程的大平移
    // （真机日志：40~123px 孤立 pan，即“第 N 下收缩视野右下窜”）。以下两成员用于转换检测与 grace 严阈值。
    // 回退：删除 gLastNumPoints_/gPostPinchGraceFrames_ 及 cpp 中相关判断即恢复旧行为。
    uint32_t gLastNumPoints_ = 0;      // 上一触摸事件手指数（MOVE 中识别双指→单指转换）
    int gPostPinchGraceFrames_ = 0;    // 捏合结束后单指平移严阈值剩余帧数
    double gLastPinchDist_ = 0;   // 双指上一帧距离（捏合缩放增量基准）
    double gLastPinchAngleDeg_ = 0; // 双指上一帧连线角（旋转增量基准）
    double gLastCentroidY_ = 0;   // 双指上一帧质心 Y（俯仰增量基准）
    // @Jin T1（实验/可回退）：捏合会话缩放主导迟滞状态，修复纯缩放时质心 Y 漂移被误翻译成 tilt 导致视角回正。
    // 回退方式：把 cpp 中 gPinchZoomLed_ 的置位逻辑短路（恒 false）即恢复“每帧质心 Y 都驱动 rotateTilt”的旧行为。
    bool gPinchZoomLed_ = false;     // 本捏合会话是否已判定为缩放主导（主导期间抑制 rotateTilt）
    int gPinchTiltStreak_ = 0;       // 俯仰意图连续占优帧数，达阈值才解除缩放主导，防边界抖动
    // 惯性滑动速度估计：保留约 80ms 前的参考触点，UP 时据 (当前位置-参考)/Δt 求初速
    float gVelRefX_ = 0;
    float gVelRefY_ = 0;
    int64_t gVelRefTsNs_ = 0;     // 0 表示本序列尚无有效参考帧
    // 长按已触发标志：host 渲染线程到点置位，主线程 UP/MOVE 读取以抑制本次序列的单击/惯性
    // （跨线程读写，故用 atomic；每次 DOWN 复位）
    std::atomic<bool> gLongPressFired_{false};

    std::function<void(float, float)> tapCb_; // 单击回调缓存（attach 时补装进 host）
    std::function<void(float, float)> longPressCb_; // 长按回调缓存（attach 时补装进 host）

    static std::mutex s_registryMtx;
    static std::unordered_map<OH_NativeXComponent *, XComponentBridge *> s_byComponent;

    OH_NativeXComponent *component_ = nullptr; // 非拥有（XComponent 节点生命周期内有效）
    globecore::GlobeEngine *engine_ = nullptr;        // 非拥有（ArkTS 句柄模型管理）
    OHNativeWindow *window_ = nullptr;         // attach 经 surfaceId 新建，ownsWindow_ 为真时析构销毁
    bool ownsWindow_ = false;                  // window_ 是否由本桥 CreateNativeWindowFromSurfaceId 拥有
    // 回调结构体必须与注册同生命周期：作为成员持有，shutdown 时一并注销
    OH_NativeXComponent_Callback callbacks_{};
    std::unique_ptr<MapRenderHost> host_; // surface 创建后非空，销毁/detach 时置空
};

} // namespace gcohos

#endif // GLOBECORE_OHOS_XCOMPONENTBRIDGE_H
