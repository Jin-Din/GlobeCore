// napi_globe_engine.cpp —— GlobeEngine 渲染引擎 NAPI 导出（对应 Android jni/GlobeEngineJni.cpp）。
//
// 移植口径：函数名、参数顺序、返回值语义与 NativeLib.kt 的 external fun 逐一对齐，
// ArkTS 门面（NativeMapView.ets）与 Kotlin 门面同构；差异仅在 GL 宿主：
//   - Android 的 surfaceCreated/Changed/drawFrame/releaseGl 由 Kotlin GLSurfaceView 转发，
//     鸿蒙无对应视图宿主 → 不导出，改由 nativeAttach/nativeDetach + XComponentBridge/EGL 渲染线程管理；
//   - nativeSetRenderCallback(view) 的反射 requestRender → 鸿蒙在 MapRenderHost 构造时
//     内部接线（脏标记唤醒），ArkTS 手势侧只需 nativeRequestRender(handle) 显式刷帧。
//
// 句柄模型同 Android：nativeCreate 返回 GlobeEngine* 地址（number），后续调用透传；0 直接返回。
// attach 的 XComponent 上下文对象经 napi_unwrap 解出 OH_NativeXComponent*（ArkTS 侧
// XComponent.getContext(ContextType.OpenGL, ...) 返回值原样传入）。
#include <napi/native_api.h>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/GlobeEngine.h"
#include "geom/Camera.h"
#include "layer/VectorLayer.h"
#include "net/HttpClient.h"
#include "util/JsonParser.h"
#include "util/Log.h"

#include "XComponentBridge.h"
#include "napi_util.h"

using globecore::GlobeEngine;

namespace {

inline GlobeEngine *toGlobeEngine(int64_t handle) {
    return reinterpret_cast<GlobeEngine *>(handle);
}

/// handle → XComponentBridge 注册表（attach 建立、detach/destroy 拆除）；
/// 引擎重绘回调经 MapRenderHost 在 attach 内部接线，此表仅服务 requestRender/detach 反查。
std::mutex g_bridgesMtx;
std::unordered_map<int64_t, gcohos::XComponentBridge *> g_bridges;

// ── 单击/长按拾取：渲染线程→JS 主线程的 TSFN 投递（native 手势确认后回调 ArkTS）──
// SURFACE 型 XComponent 触摸只进 native（ArkTS .onTouch 不触发），故单击/长按事件源在渲染线程，
// 经 threadsafe function 转投 JS 线程触发用户回调（坐标 vp，与手势通路同口径）。
struct TapPack {
    float x;
    float y;
};

/// 手势确认回调注册表：单击与长按各持一条 tsfn（同一 JS 线程投递通路，互不干扰）
struct GestureListener {
    napi_threadsafe_function tsfn = nullptr;
    napi_threadsafe_function longPressTsfn = nullptr;
};

std::mutex g_tapMtx;
std::unordered_map<int64_t, GestureListener> g_tapListeners;

// @Jin 地形事件跨线程投递：Provider 调度/worker 线程只入队，真正 JS 回调在 ArkTS 线程执行。
struct TerrainEventListener {
    std::mutex mtx;
    napi_threadsafe_function tsfn = nullptr;

    void emit(const std::string &json) {
        std::lock_guard<std::mutex> lock(mtx);
        if (tsfn == nullptr) return;
        auto *pack = new std::string(json);
        if (napi_call_threadsafe_function(tsfn, pack, napi_tsfn_nonblocking) != napi_ok) delete pack;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mtx);
        if (tsfn == nullptr) return;
        napi_release_threadsafe_function(tsfn, napi_tsfn_release);
        tsfn = nullptr;
    }
};

std::mutex g_terrainEventMtx;
std::unordered_map<int64_t, std::shared_ptr<TerrainEventListener>> g_terrainEventListeners;

void TerrainEventCallJs(napi_env env, napi_value jsCb, void * /*context*/, void *data) {
    std::unique_ptr<std::string> json(static_cast<std::string *>(data));
    if (env == nullptr || jsCb == nullptr || json == nullptr) return;
    napi_value arg = nullptr;
    napi_create_string_utf8(env, json->c_str(), NAPI_AUTO_LENGTH, &arg);
    napi_value thisArg = nullptr;
    napi_get_undefined(env, &thisArg);
    napi_call_function(env, thisArg, jsCb, 1, &arg, nullptr);
}

napi_threadsafe_function MakeTerrainEventTsfn(napi_env env, napi_value cb) {
    napi_value asyncName = nullptr;
    napi_create_string_utf8(env, "globecore-terrain-event", NAPI_AUTO_LENGTH, &asyncName);
    napi_threadsafe_function tsfn = nullptr;
    const napi_status st = napi_create_threadsafe_function(
        env, cb, nullptr, asyncName, 32 /*max_queue_size*/, 1 /*initial_thread_count*/,
        nullptr, nullptr, nullptr, TerrainEventCallJs, &tsfn);
    if (st != napi_ok) {
        LOGE("globecore-terrain-event: create tsfn failed: %d", st);
        return nullptr;
    }
    return tsfn;
}

void ReleaseTerrainEventListener(int64_t handle) {
    std::shared_ptr<TerrainEventListener> listener;
    {
        std::lock_guard<std::mutex> lock(g_terrainEventMtx);
        auto it = g_terrainEventListeners.find(handle);
        if (it == g_terrainEventListeners.end()) return;
        listener = it->second;
        g_terrainEventListeners.erase(it);
    }
    if (listener) listener->close();
}

void TapCallJs(napi_env env, napi_value jsCb, void * /*context*/, void *data) {
    std::unique_ptr<TapPack> pack(static_cast<TapPack *>(data)); // 销毁广播时 data 为 null
    if (env == nullptr || jsCb == nullptr || pack == nullptr) return;
    napi_value args[2];
    napi_create_double(env, static_cast<double>(pack->x), &args[0]);
    napi_create_double(env, static_cast<double>(pack->y), &args[1]);
    napi_value thisArg = nullptr;
    napi_get_undefined(env, &thisArg);
    napi_call_function(env, thisArg, jsCb, 2, args, nullptr);
}

/// 把 handle 对应注册表里的指定 tsfn 包成投递器（渲染线程调用、非阻塞入队转投 JS 主线程）；
/// tsfn 未注册则返回空 function（桥侧留待注册后补装）。
std::function<void(float, float)> MakeGestureDispatcher(int64_t handle, bool longPress) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end()) return nullptr;
    napi_threadsafe_function tsfn = longPress ? it->second.longPressTsfn : it->second.tsfn;
    if (tsfn == nullptr) return nullptr;
    return [tsfn](float x, float y) {
        auto *pack = new TapPack{x, y};
        // 非阻塞入队：队列满/已销毁则丢弃本次事件（宁缺勿悬），转投 JS 主线程
        if (napi_call_threadsafe_function(tsfn, pack, napi_tsfn_nonblocking) != napi_ok) {
            delete pack;
        }
    };
}

/// 反注册单击回调（仅拆 tsfn，长按注册保留）
void ReleaseTapListener(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end()) return;
    if (it->second.tsfn != nullptr) {
        napi_release_threadsafe_function(it->second.tsfn, napi_tsfn_release); // finalizer 在 JS 线程跑
        it->second.tsfn = nullptr;
    }
}

/// 反注册长按回调（仅拆 tsfn，单击注册保留）
void ReleaseLongPressListener(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end()) return;
    if (it->second.longPressTsfn != nullptr) {
        napi_release_threadsafe_function(it->second.longPressTsfn, napi_tsfn_release);
        it->second.longPressTsfn = nullptr;
    }
}

/// detach/destroy 时清空 handle 的全部手势 tsfn（join 之后调用：渲染线程已停，不会再碰 tsfn）
void ReleaseGestureListeners(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end()) return;
    if (it->second.tsfn != nullptr) {
        napi_release_threadsafe_function(it->second.tsfn, napi_tsfn_release);
    }
    if (it->second.longPressTsfn != nullptr) {
        napi_release_threadsafe_function(it->second.longPressTsfn, napi_tsfn_release);
    }
    g_tapListeners.erase(it);
}

/// 在 JS 线程为指定回调建 tsfn 并返回；失败返回 nullptr（调用方负责写入注册表）。
napi_threadsafe_function MakeGestureTsfn(napi_env env, napi_value cb, const char *name) {
    napi_value asyncName = nullptr;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &asyncName);
    // JS 回调函数直接交给 tsfn（napi 内部持持久引用，CallJs 的 jsCb 参数即此函数）；
    // 队列上限 16：单击/长按是低频事件，积压只出现在快速连点，满了丢弃新事件而非无限排队。
    // 注意：OHOS 签名的 context 在 call_js_cb 之前，与 Node 参数序相反。
    napi_threadsafe_function tsfn = nullptr;
    const napi_status st = napi_create_threadsafe_function(
        env, cb, nullptr, asyncName, 16 /*max_queue_size*/, 1 /*initial_thread_count*/,
        nullptr /*thread_finalize_data*/, nullptr /*thread_finalize_cb*/, nullptr /*context*/,
        TapCallJs, &tsfn);
    if (st != napi_ok) {
        LOGE("%s: create tsfn failed: %d", name, st);
        return nullptr;
    }
    return tsfn;
}

/// #AARRGGBB 整型颜色拆为 [0,1] 浮点 RGBA（口径同 JNI unpackArgb）
inline void unpackArgb(int32_t c, float &r, float &g, float &b, float &a) {
    a = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
    r = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    g = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(c & 0xFF) / 255.0f;
}

/// 0xAARRGGBB int32 像素 → RGBA 字节（口径同 JNI iconArgbToRgba；尺寸不匹配返回空 → 点回退画圆）
inline std::vector<uint8_t> iconArgbToRgba(const std::vector<int32_t> &argb, int32_t w, int32_t h) {
    std::vector<uint8_t> out;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (argb.empty() || w <= 0 || h <= 0 || argb.size() != n) return out;
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = static_cast<uint32_t>(argb[i]);
        out[i * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF); // R
        out[i * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);  // G
        out[i * 4 + 2] = static_cast<uint8_t>(p & 0xFF);         // B
        out[i * 4 + 3] = static_cast<uint8_t>((p >> 24) & 0xFF); // A
    }
    return out;
}

/// double[] → long long 向量（FID 以 double 传输，<2^53 精确；口径同 toLongVec）
inline std::vector<long long> toLongVec(const std::vector<double> &v) {
    std::vector<long long> out;
    out.reserve(v.size());
    for (double d : v) out.push_back(static_cast<long long>(d));
    return out;
}

/// double[] → int 向量（顶点/环计数；口径同 toIntVec）
inline std::vector<int> toIntVec(const std::vector<double> &v) {
    std::vector<int> out;
    out.reserve(v.size());
    for (double d : v) out.push_back(static_cast<int>(d));
    return out;
}

/// 解析单要素样式覆盖串（来自 app 文档层 featureStyles）为 fid → FeatureStyleOverride，
/// 逻辑与 Android GlobeEngineJni.cpp parseFeatureOverrides 逐字节一致（共用同一 native 引擎，仅桥接层不同）。
/// 条目以 ';' 分隔，每条目 8 字段逗号分隔：
///   fid,fillArgb,lineArgb,labelFieldName,labelArgb,labelSize,labelOutline,labelOutlineArgb
/// - fillArgb/lineArgb/labelArgb/labelOutlineArgb 为 #AARRGGBB 无符号十进制，'-' 或空 = 该通道未显式设过；
/// - labelFieldName：'-' 或缺省 = 无逐要素标注覆盖（继承整层）；非 '-' = 该要素标注字段名（空串 = 显式关闭）；
/// - labelSize：'-' = 继承，否则浮点；labelOutline：'-' = 继承，否则 1/0。向后兼容旧 3 字段串。空串返回空表（零回归）。
inline std::unordered_map<long long, globecore::FeatureStyleOverride> parseFeatureOverrides(const std::string &s) {
    std::unordered_map<long long, globecore::FeatureStyleOverride> out;
    if (s.empty()) return out;
    auto unpackTo = [](unsigned int c, float (&dst)[4]) {
        dst[3] = static_cast<float>((c >> 24) & 0xFF) / 255.0f; // A
        dst[0] = static_cast<float>((c >> 16) & 0xFF) / 255.0f; // R
        dst[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;  // G
        dst[2] = static_cast<float>(c & 0xFF) / 255.0f;         // B
    };
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t semi = s.find(';', pos);
        const std::string entry = s.substr(pos, (semi == std::string::npos) ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;
        if (entry.empty()) continue;
        std::vector<std::string> tk;
        size_t p = 0;
        while (true) {
            const size_t cm = entry.find(',', p);
            tk.push_back(entry.substr(p, (cm == std::string::npos) ? std::string::npos : cm - p));
            if (cm == std::string::npos) break;
            p = cm + 1;
        }
        if (tk.empty()) continue;
        auto get = [&](size_t i, std::string &v) -> bool {  // true=字段存在且非 '-'（即显式设过）
            if (i >= tk.size()) return false;
            const std::string &t = tk[i];
            if (t.empty() || t == "-") return false;
            v = t;
            return true;
        };
        globecore::FeatureStyleOverride ov;
        std::string v;
        if (get(1, v)) { ov.hasFill = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.fill); }
        if (get(2, v)) { ov.hasLine = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.line); }
        // labelFieldName（索引 3）：区分 '-'（缺字段）与显式关闭（字段存在但为空串），故不走 get()。
        if (tk.size() > 3 && tk[3] != "-") {
            ov.hasLabel = true;
            ov.labelField = tk[3];  // 空串 = 显式关闭；非空 = 标注字段名（可含非 ASCII，原样传递）
        }
        if (get(4, v)) { ov.hasLabelColor = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.labelColor); }
        if (get(5, v)) { ov.hasLabelSize = true; ov.labelSize = static_cast<float>(strtod(v.c_str(), nullptr)); }
        if (get(6, v)) { ov.hasLabelOutline = true; ov.labelOutlineVal = (v[0] == '1'); }
        if (get(7, v)) { ov.hasLabelOutlineColor = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.labelOutlineColor); }
        if (ov.hasFill || ov.hasLine || ov.hasLabel || ov.hasLabelColor || ov.hasLabelSize ||
            ov.hasLabelOutline || ov.hasLabelOutlineColor)
            out[strtoll(tk[0].c_str(), nullptr, 10)] = ov;
    }
    return out;
}

// ── 生命周期 ──

napi_value NativeCreate(napi_env env, napi_callback_info /*info*/) {
    return gcohos::NapiMakeInt64(env, reinterpret_cast<int64_t>(new GlobeEngine()));
}

napi_value NativeDestroy(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    // 安全网：未 detach 的桥先拆除（停渲染线程再删 GlobeEngine，防 EGL 线程悬挂）
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) {
            it->second->shutdown();
            delete it->second;
            g_bridges.erase(it);
        }
    }
    ReleaseGestureListeners(handle); // join 之后：渲染线程已停，不会再碰 tsfn
    ReleaseTerrainEventListener(handle); // @Jin Provider 线程随后由 GlobeEngine 析构 join。
    delete toGlobeEngine(handle);
    return nullptr;
}

/// nativeAttach(handle, surfaceId, width, height) → bool：用 surfaceId 直接绑定 surface 并起 EGL 渲染线程
napi_value NativeAttach(napi_env env, napi_callback_info info) {
    napi_value args[4];
    gcohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) {
        return gcohos::NapiMakeBool(env, false);
    }
    int64_t surfaceId = 0;
    gcohos::NapiGetInt64(env, args[1], surfaceId);
    int32_t width = 0;
    int32_t height = 0;
    gcohos::NapiGetInt32(env, args[2], width);
    gcohos::NapiGetInt32(env, args[3], height);
    auto *bridge = gcohos::XComponentBridge::attach(env, toGlobeEngine(handle),
                                                    static_cast<uint64_t>(surfaceId), width, height);
    if (bridge == nullptr) return gcohos::NapiMakeBool(env, false);
    std::lock_guard<std::mutex> lock(g_bridgesMtx);
    auto it = g_bridges.find(handle);
    if (it != g_bridges.end()) { // 重挂：旧桥拆除（ ArkTS 侧应先 detach，此为防御路径）
        it->second->shutdown();
        delete it->second;
    }
    g_bridges[handle] = bridge;
    // attach 前已注册的单击/长按回调补装进桥（桥内部再转 host）
    bridge->setSingleTapCallback(MakeGestureDispatcher(handle, false));
    bridge->setLongPressCallback(MakeGestureDispatcher(handle, true));
    return gcohos::NapiMakeBool(env, true);
}

/// nativeDetach(handle)：注销 surface 回调 + 停渲染线程（GlobeEngine 数据态保留，可再 attach）
napi_value NativeDetach(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    gcohos::XComponentBridge *bridge = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) {
            bridge = it->second;
            g_bridges.erase(it);
        }
    }
    if (bridge != nullptr) {
        bridge->shutdown();
        delete bridge;
    }
    ReleaseGestureListeners(handle); // 与桥同生命周期：detach 后重 attach 需重设回调（或由上层重调）
    return nullptr;
}

/// nativeSetTapCallback(handle, cb)：注册/更新（cb=null 反注册）单击拾取回调。
/// 事件源：native 手势识别确认的单击（短按未移动且未触发长按），坐标 vp。
napi_value NativeSetTapCallback(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;

    ReleaseTapListener(handle); // 替换语义：先拆旧投递器

    napi_valuetype vt = napi_undefined;
    if (args[1] != nullptr) napi_typeof(env, args[1], &vt);
    if (vt != napi_function) return nullptr; // 传 null/undefined = 反注册

    napi_threadsafe_function tsfn = MakeGestureTsfn(env, args[1], "globecore-tap");
    if (tsfn == nullptr) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tapMtx);
        g_tapListeners[handle].tsfn = tsfn;
    }
    // 装进当前桥（若已 attach）；未 attach 则 NativeAttach 末尾补装
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) it->second->setSingleTapCallback(MakeGestureDispatcher(handle, false));
    }
    return nullptr;
}

/// nativeSetLongPressCallback(handle, cb)：注册/更新（cb=null 反注册）长按拾取回调。
/// 事件源：native 手势识别确认的长按（单指保持按住、未越过拖动阈值达长按时长，对齐 Android
/// GestureDetector.onLongPress），坐标 vp；供宿主弹落点操作菜单等。口径同 nativeSetTapCallback。
napi_value NativeSetLongPressCallback(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;

    ReleaseLongPressListener(handle); // 替换语义：先拆旧投递器

    napi_valuetype vt = napi_undefined;
    if (args[1] != nullptr) napi_typeof(env, args[1], &vt);
    if (vt != napi_function) return nullptr; // 传 null/undefined = 反注册

    napi_threadsafe_function tsfn = MakeGestureTsfn(env, args[1], "globecore-long-press");
    if (tsfn == nullptr) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tapMtx);
        g_tapListeners[handle].longPressTsfn = tsfn;
    }
    // 装进当前桥（若已 attach）；未 attach 则 NativeAttach 末尾补装
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) it->second->setLongPressCallback(MakeGestureDispatcher(handle, true));
    }
    return nullptr;
}

/// 手势/属性变更后显式刷帧（对应 Kotlin view.requestRender()；surface 未就绪时忽略）
napi_value NativeRequestRender(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_bridgesMtx);
    auto it = g_bridges.find(handle);
    if (it != g_bridges.end()) it->second->requestRender();
    return nullptr;
}

// ── 相机 / 视图模式 / 手势 ──

napi_value NativeSetCamera(napi_env env, napi_callback_info info) {
    napi_value args[9];
    gcohos::NapiGetArgs(env, info, 9, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double lat = 0, lon = 0, alt = 0, heading = 0, tilt = 0, roll = 0, fov = 0;
    int32_t altMode = 0;
    gcohos::NapiGetDouble(env, args[1], lat);
    gcohos::NapiGetDouble(env, args[2], lon);
    gcohos::NapiGetDouble(env, args[3], alt);
    gcohos::NapiGetDouble(env, args[4], heading);
    gcohos::NapiGetDouble(env, args[5], tilt);
    gcohos::NapiGetDouble(env, args[6], roll);
    gcohos::NapiGetDouble(env, args[7], fov);
    gcohos::NapiGetInt32(env, args[8], altMode);
    globecore::Camera cam;
    cam.latitude = lat;
    cam.longitude = lon;
    cam.altitude = alt;
    cam.heading = heading;
    cam.tilt = tilt;
    cam.roll = roll;
    cam.fieldOfView = fov;
    // int → 枚举：越界回退 ABSOLUTE（与 geom/Camera.h 默认一致，同 JNI 版）
    cam.altitudeMode = (altMode >= 0 && altMode <= 3)
                           ? static_cast<globecore::AltitudeMode>(altMode)
                           : globecore::AltitudeMode::ABSOLUTE;
    toGlobeEngine(handle)->setCamera(cam);
    return nullptr;
}


napi_value NativeFlyToCamera(napi_env env, napi_callback_info info) {
    napi_value args[10];
    gcohos::NapiGetArgs(env, info, 10, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double lat = 0, lon = 0, alt = 0, heading = 0, tilt = 0, roll = 0, fov = 0, durationMs = 1600.0;
    int32_t altMode = 0;
    gcohos::NapiGetDouble(env, args[1], lat);
    gcohos::NapiGetDouble(env, args[2], lon);
    gcohos::NapiGetDouble(env, args[3], alt);
    gcohos::NapiGetDouble(env, args[4], heading);
    gcohos::NapiGetDouble(env, args[5], tilt);
    gcohos::NapiGetDouble(env, args[6], roll);
    gcohos::NapiGetDouble(env, args[7], fov);
    gcohos::NapiGetInt32(env, args[8], altMode);
    gcohos::NapiGetDouble(env, args[9], durationMs);
    globecore::Camera cam;
    cam.latitude = lat;
    cam.longitude = lon;
    cam.altitude = alt;
    cam.heading = heading;
    cam.tilt = tilt;
    cam.roll = roll;
    cam.fieldOfView = fov;
    // @Jin 越界高度模式回退 ABSOLUTE，保持 nativeSetCamera 的参数容错口径。
    cam.altitudeMode = (altMode >= 0 && altMode <= 3)
                           ? static_cast<globecore::AltitudeMode>(altMode)
                           : globecore::AltitudeMode::ABSOLUTE;
    toGlobeEngine(handle)->flyToCamera(cam, durationMs);
    return nullptr;
}

napi_value NativeGetCamera(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    const globecore::Camera cam = toGlobeEngine(handle)->getCamera();
    const double buf[8] = {
        cam.latitude, cam.longitude, cam.altitude,
        cam.heading, cam.tilt, cam.roll, cam.fieldOfView,
        static_cast<double>(static_cast<int>(cam.altitudeMode)),
    };
    return gcohos::NapiMakeFloat64Array(env, buf, 8);
}

napi_value NativeSetViewMode(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t mode = 0;
    gcohos::NapiGetInt32(env, args[1], mode);
    toGlobeEngine(handle)->setViewMode(mode);
    return nullptr;
}

napi_value NativeGetViewMode(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, 0);
    return gcohos::NapiMakeInt32(env, toGlobeEngine(handle)->viewMode());
}

napi_value NativePanBy(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double dx = 0, dy = 0;
    gcohos::NapiGetDouble(env, args[1], dx);
    gcohos::NapiGetDouble(env, args[2], dy);
    toGlobeEngine(handle)->panByPixels(dx, dy);
    return nullptr;
}

napi_value NativeZoomBy(napi_env env, napi_callback_info info) {
    napi_value args[4];
    gcohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double factor = 1, fx = 0, fy = 0;
    gcohos::NapiGetDouble(env, args[1], factor);
    gcohos::NapiGetDouble(env, args[2], fx);
    gcohos::NapiGetDouble(env, args[3], fy);
    toGlobeEngine(handle)->zoomBy(factor, fx, fy);
    return nullptr;
}

napi_value NativeRotateHeading(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double delta = 0;
    gcohos::NapiGetDouble(env, args[1], delta);
    toGlobeEngine(handle)->rotateHeading(delta);
    return nullptr;
}

napi_value NativeRotateTilt(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double delta = 0;
    gcohos::NapiGetDouble(env, args[1], delta);
    toGlobeEngine(handle)->rotateTilt(delta);
    return nullptr;
}

// ── 瓦片 / 栅格图层 ──

napi_value NativeAddTileLayer(napi_env env, napi_callback_info info) {
    napi_value args[5];
    gcohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    const std::string cacheDir = gcohos::NapiGetString(env, args[1]);
    if (cacheDir.empty()) return nullptr;
    const std::string url = gcohos::NapiGetString(env, args[2]);
    int32_t maxLevel = 0;
    gcohos::NapiGetInt32(env, args[3], maxLevel);
    bool overlay = false;
    gcohos::NapiGetBool(env, args[4], overlay);
    toGlobeEngine(handle)->addTileLayer(cacheDir, url, maxLevel, overlay);
    return nullptr;
}

napi_value NativeSetLayerVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool visible = false;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetBool(env, args[2], visible);
    toGlobeEngine(handle)->setLayerVisible(index, visible);
    return nullptr;
}

napi_value NativeAddRasterLayer(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, -1);
    const std::string cacheDir = gcohos::NapiGetString(env, args[1]);
    const std::string path = gcohos::NapiGetString(env, args[2]);
    if (cacheDir.empty() || path.empty()) return gcohos::NapiMakeInt32(env, -1);
    return gcohos::NapiMakeInt32(env, toGlobeEngine(handle)->addRasterLayer(cacheDir, path));
}

// 移除一个瓦片图源图层（含栅格层）：墓碑—置 removed + 隐藏，不 erase 以保持其它层 index 稳定；
// 其纹理由 Renderer 下一帧在 GL 线程释放，后台加载队列即时清空。index 越界/句柄无效忽略。
// 用于宿主删除栅格图层时就地换层，免整界面 recreate（避免 GL 上下文销毁导致的底图重载闪动）。
napi_value NativeRemoveTileLayer(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    toGlobeEngine(handle)->removeTileLayer(index);
    return nullptr;
}

// 最近一次 nativeAddRasterLayer 失败原因（中文，含文件线索）；成功或从未失败返回 null。供宿主 Toast 展示。
napi_value NativeGetRasterAddError(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    const std::string err = toGlobeEngine(handle)->rasterAddError();
    if (err.empty()) return gcohos::NapiMakeNull(env);
    return gcohos::NapiMakeString(env, err);
}

// 添加一个「全幅加载」栅格图层：后台线程整幅直读 tif 为单张贴图，按其 WGS84 四至贴面（UI 线程零 GDAL）。
// cacheDir 非空时启用全幅 sidecar 磁盘缓存（与 nativeAddRasterLayer 共用同一目录）；空串禁用。
// 返回新图层索引（0 起，独立于栅格瓦片路的索引空间）；句柄/路径无效返回 -1。
napi_value NativeAddRasterImageLayer(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, -1);
    const std::string cacheDir = gcohos::NapiGetString(env, args[1]);
    const std::string path = gcohos::NapiGetString(env, args[2]);
    if (path.empty()) return gcohos::NapiMakeInt32(env, -1);
    return gcohos::NapiMakeInt32(env, toGlobeEngine(handle)->addRasterImageLayer(cacheDir, path));
}

// 设置指定全幅栅格图层可见性（index 为 nativeAddRasterImageLayer 加入次序，0 起）：越界忽略
napi_value NativeSetRasterImageVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool visible = false;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetBool(env, args[2], visible);
    toGlobeEngine(handle)->setRasterImageVisible(index, visible);
    return nullptr;
}

// 可见全幅栅格层是否仍有未完成的整幅加载/纹理上传：纯查询不触发重绘，供「加载中」提示轮询。
napi_value NativeHasRasterLoading(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, toGlobeEngine(handle)->hasRasterImageLoading());
}

// 取指定全幅栅格层的加载失败原因（中文）；未失败/越界/已成功加载返回 null。供加载胶囊结束后 Toast 提示。
napi_value NativeGetRasterImageError(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    const std::string err = toGlobeEngine(handle)->rasterImageError(index);
    if (err.empty()) return gcohos::NapiMakeNull(env);
    return gcohos::NapiMakeString(env, err);
}

// ── 矢量图层 ──

napi_value NativeAddVectorLayer(napi_env env, napi_callback_info info) {
    napi_value args[27];
    gcohos::NapiGetArgs(env, info, 27, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, -1);
    const std::string path = gcohos::NapiGetString(env, args[1]);
    if (path.empty()) return gcohos::NapiMakeInt32(env, -1);

    int32_t fillColor = 0, outlineColor = 0, lineColor = 0, pointColor = 0;
    int32_t labelColor = 0, labelOutlineColor = 0;
    double outlineWidth = 0, lineWidth = 0, pointRadiusDp = 0, labelSize = 0;
    bool fillExplicit = false, lineExplicit = false, labelOutline = false;
    gcohos::NapiGetInt32(env, args[2], fillColor);
    gcohos::NapiGetInt32(env, args[3], outlineColor);
    gcohos::NapiGetDouble(env, args[4], outlineWidth);
    gcohos::NapiGetInt32(env, args[5], lineColor);
    gcohos::NapiGetDouble(env, args[6], lineWidth);
    gcohos::NapiGetInt32(env, args[7], pointColor);
    gcohos::NapiGetDouble(env, args[8], pointRadiusDp);
    gcohos::NapiGetBool(env, args[9], fillExplicit);
    gcohos::NapiGetBool(env, args[10], lineExplicit);
    const std::string labelField = gcohos::NapiGetString(env, args[11]);
    gcohos::NapiGetInt32(env, args[12], labelColor);
    gcohos::NapiGetDouble(env, args[13], labelSize);
    gcohos::NapiGetBool(env, args[14], labelOutline);
    gcohos::NapiGetInt32(env, args[15], labelOutlineColor);
    // 单要素样式覆盖串（fid,fillArgb,lineArgb,labelFieldName,labelArgb,labelSize,labelOutline,labelOutlineArgb）
    const std::string featureStyleOverride = gcohos::NapiGetString(env, args[16]);
    const std::vector<int32_t> iconArgb = gcohos::NapiGetInt32Array(env, args[17]);
    int32_t iconW = 0, iconH = 0;
    gcohos::NapiGetInt32(env, args[18], iconW);
    gcohos::NapiGetInt32(env, args[19], iconH);
    bool hasExtent = false;
    gcohos::NapiGetBool(env, args[20], hasExtent);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    gcohos::NapiGetDouble(env, args[21], minLon);
    gcohos::NapiGetDouble(env, args[22], minLat);
    gcohos::NapiGetDouble(env, args[23], maxLon);
    gcohos::NapiGetDouble(env, args[24], maxLat);
    // @Jin maxFeatures 为第 26 个参数；vectorCacheDir 为第 27 个参数，供在线 GeoJSON 私有缓存使用。
    int32_t maxFeatures = 0;
    if (args[25] != nullptr) gcohos::NapiGetInt32(env, args[25], maxFeatures);
    // @Jin HarmonyOS NEXT 宿主传入应用私有目录；http/https GeoJSON 由共享 C++ 下载器落盘到这里。
    const std::string vectorCacheDir = gcohos::NapiGetString(env, args[26]);

    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = static_cast<float>(outlineWidth);
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = static_cast<float>(lineWidth);
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = static_cast<float>(pointRadiusDp);
    // 逐要素源文件配色门控：整层显式设了该通道则不被 KML 原色覆盖（图层管理改样式即时生效）。
    style.hasFillColor = fillExplicit;
    style.hasLineColor = lineExplicit;
    style.labelField = labelField; // 空则整层不标注（对齐主界面门控）
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = static_cast<float>(labelSize);
    style.labelOutline = labelOutline;
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB,
               style.labelOutlineA);
    // 单要素样式覆盖：解析下传串入 style.featureOverrides（建几何时逐要素按 fid 命中覆盖颜色/标注）。
    style.featureOverrides = parseFeatureOverrides(featureStyleOverride);

    std::vector<uint8_t> iconRgba = iconArgbToRgba(iconArgb, iconW, iconH);
    const int index = toGlobeEngine(handle)->addVectorLayer(
            path, style, std::move(iconRgba), iconW, iconH,
            hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures, vectorCacheDir);
    return gcohos::NapiMakeInt32(env, index);
}

napi_value NativeUpdateVectorExtent(napi_env env, napi_callback_info info) {
    napi_value args[8];
    gcohos::NapiGetArgs(env, info, 8, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1, maxFeatures = 0;
    bool hasExtent = false;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetBool(env, args[2], hasExtent);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    gcohos::NapiGetDouble(env, args[3], minLon);
    gcohos::NapiGetDouble(env, args[4], minLat);
    gcohos::NapiGetDouble(env, args[5], maxLon);
    gcohos::NapiGetDouble(env, args[6], maxLat);
    gcohos::NapiGetInt32(env, args[7], maxFeatures);
    toGlobeEngine(handle)->updateVectorExtent(index, hasExtent, minLon, minLat, maxLon, maxLat,
                                              maxFeatures);
    return nullptr;
}

napi_value NativeHasVectorLoading(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, toGlobeEngine(handle)->hasVectorLoading());
}

napi_value NativeSetVectorLayerVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool visible = false;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetBool(env, args[2], visible);
    toGlobeEngine(handle)->setVectorLayerVisible(index, visible);
    return nullptr;
}

napi_value NativeSetVectorMinLevel(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1, minLevel = 0;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetInt32(env, args[2], minLevel);
    toGlobeEngine(handle)->setVectorMinLevel(index, minLevel);
    return nullptr;
}

napi_value NativeGetCameraZoomLevel(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, 0);
    return gcohos::NapiMakeInt32(env, toGlobeEngine(handle)->currentZoomLevel());
}

napi_value NativeGetTileZoomLevel(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    int32_t maxLevel = 18;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, 0);
    gcohos::NapiGetInt32(env, args[1], maxLevel);
    // @Jin 返回当前图层目标瓦片 LOD，供 Demo 区分界面显示级别与实际纹理细分级别。
    return gcohos::NapiMakeInt32(env, toGlobeEngine(handle)->currentTileLevel(maxLevel));
}

napi_value NativeSetTileLodOffset(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    int32_t offset = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    gcohos::NapiGetInt32(env, args[1], offset);
    // @Jin 运行时实验档位：负数更粗、0 恢复；不持久化，重启应用自动回到标准 LOD。
    toGlobeEngine(handle)->setTileLodOffset(offset);
    return nullptr;
}

napi_value NativeSetTileLodAlignDisplay(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    bool align = false;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    gcohos::NapiGetBool(env, args[1], align);
    // @Jin 同级实验开关：瓦片目标 LOD 直接取相机显示级别，便于真机对比字号和请求数量。
    toGlobeEngine(handle)->setTileLodAlignDisplay(align);
    return nullptr;
}

napi_value NativeRemoveVectorLayer(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    toGlobeEngine(handle)->removeVectorLayer(index);
    return nullptr;
}

// ── 动态叠加层 ──

napi_value NativeAddOverlayLayer(napi_env env, napi_callback_info info) {
    napi_value args[15];
    gcohos::NapiGetArgs(env, info, 15, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeInt32(env, -1);
    int32_t fillColor = 0, outlineColor = 0, lineColor = 0, pointColor = 0;
    int32_t labelColor = 0, labelOutlineColor = 0;
    double outlineWidth = 0, lineWidth = 0, pointRadiusDp = 0, labelSize = 0;
    bool labelOutline = false;
    gcohos::NapiGetInt32(env, args[1], fillColor);
    gcohos::NapiGetInt32(env, args[2], outlineColor);
    gcohos::NapiGetDouble(env, args[3], outlineWidth);
    gcohos::NapiGetInt32(env, args[4], lineColor);
    gcohos::NapiGetDouble(env, args[5], lineWidth);
    gcohos::NapiGetInt32(env, args[6], pointColor);
    gcohos::NapiGetDouble(env, args[7], pointRadiusDp);
    gcohos::NapiGetInt32(env, args[8], labelColor);
    gcohos::NapiGetDouble(env, args[9], labelSize);
    gcohos::NapiGetBool(env, args[10], labelOutline);
    gcohos::NapiGetInt32(env, args[11], labelOutlineColor);
    const std::vector<int32_t> iconArgb = gcohos::NapiGetInt32Array(env, args[12]);
    int32_t iconW = 0, iconH = 0;
    gcohos::NapiGetInt32(env, args[13], iconW);
    gcohos::NapiGetInt32(env, args[14], iconH);

    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = static_cast<float>(outlineWidth);
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = static_cast<float>(lineWidth);
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = static_cast<float>(pointRadiusDp);
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = static_cast<float>(labelSize);
    style.labelOutline = labelOutline;
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB,
               style.labelOutlineA);
    std::vector<uint8_t> iconRgba = iconArgbToRgba(iconArgb, iconW, iconH);
    return gcohos::NapiMakeInt32(env,
                                 toGlobeEngine(handle)->addOverlayLayer(style, std::move(iconRgba), iconW, iconH));
}

napi_value NativeUpdateOverlayPoints(napi_env env, napi_callback_info info) {
    napi_value args[5];
    gcohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = gcohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> fids = gcohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<std::string> labels = gcohos::NapiGetStringArray(env, args[4]);
    toGlobeEngine(handle)->updateOverlayPoints(index, lonlat, toLongVec(fids), labels);
    return nullptr;
}

napi_value NativeUpdateOverlayLines(napi_env env, napi_callback_info info) {
    napi_value args[6];
    gcohos::NapiGetArgs(env, info, 6, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = gcohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> vertexCounts = gcohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<double> fids = gcohos::NapiGetFloat64Array(env, args[4]);
    const std::vector<std::string> labels = gcohos::NapiGetStringArray(env, args[5]);
    toGlobeEngine(handle)->updateOverlayLines(index, lonlat, toIntVec(vertexCounts), toLongVec(fids),
                                              labels);
    return nullptr;
}

napi_value NativeUpdateOverlayPolygons(napi_env env, napi_callback_info info) {
    napi_value args[7];
    gcohos::NapiGetArgs(env, info, 7, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = gcohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> ringVertexCounts = gcohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<double> ringsPerFeature = gcohos::NapiGetFloat64Array(env, args[4]);
    const std::vector<double> fids = gcohos::NapiGetFloat64Array(env, args[5]);
    const std::vector<std::string> labels = gcohos::NapiGetStringArray(env, args[6]);
    toGlobeEngine(handle)->updateOverlayPolygons(index, lonlat, toIntVec(ringVertexCounts),
                                                 toIntVec(ringsPerFeature), toLongVec(fids), labels);
    return nullptr;
}

napi_value NativeRemoveOverlayLayer(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    gcohos::NapiGetInt32(env, args[1], index);
    toGlobeEngine(handle)->removeOverlayLayer(index);
    return nullptr;
}

napi_value NativeSetOverlayNoPick(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool noPick = false;
    gcohos::NapiGetInt32(env, args[1], index);
    gcohos::NapiGetBool(env, args[2], noPick);
    toGlobeEngine(handle)->setOverlayNoPick(index, noPick);
    return nullptr;
}

napi_value NativeClearOverlayLayers(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->clearOverlayLayers();
    return nullptr;
}

// ── 拾取 / 反算 / 要素几何 ──

napi_value NativePickVector(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    double sx = 0, sy = 0;
    gcohos::NapiGetDouble(env, args[1], sx);
    gcohos::NapiGetDouble(env, args[2], sy);
    int layerIndex = -1;
    long long fid = -1;
    if (!toGlobeEngine(handle)->pickVector(sx, sy, layerIndex, fid)) return gcohos::NapiMakeNull(env);
    const double buf[2] = {static_cast<double>(layerIndex), static_cast<double>(fid)};
    return gcohos::NapiMakeFloat64Array(env, buf, 2);
}

napi_value NativeScreenToGeo(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    double sx = 0, sy = 0;
    gcohos::NapiGetDouble(env, args[1], sx);
    gcohos::NapiGetDouble(env, args[2], sy);
    double lon = 0, lat = 0;
    if (!toGlobeEngine(handle)->screenToGeo(sx, sy, lon, lat)) return gcohos::NapiMakeNull(env);
    const double buf[2] = {lon, lat};
    return gcohos::NapiMakeFloat64Array(env, buf, 2);
}

/// 命中返回 Array<Float64Array> [ [type], ringCounts, ringsPerFeature, lonlat ]（口径同 JNI double[4][]）
napi_value NativeFeatureGeometry(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeNull(env);
    int32_t layerIndex = -1;
    double fidD = 0;
    gcohos::NapiGetInt32(env, args[1], layerIndex);
    gcohos::NapiGetDouble(env, args[2], fidD);
    int type = -1;
    std::vector<double> lonlat;
    std::vector<int> ringCounts, ringsPerFeature;
    if (!toGlobeEngine(handle)->featureGeometry(layerIndex, static_cast<long long>(fidD), type,
                                                lonlat, ringCounts, ringsPerFeature)) {
        return gcohos::NapiMakeNull(env);
    }
    std::vector<double> typeVec = {static_cast<double>(type)};
    std::vector<double> ringCountsD(ringCounts.begin(), ringCounts.end());
    std::vector<double> ringsPerFeatureD(ringsPerFeature.begin(), ringsPerFeature.end());
    napi_value result = nullptr;
    napi_create_array_with_length(env, 4, &result);
    napi_set_element(env, result, 0, gcohos::NapiMakeFloat64Array(env, typeVec));
    napi_set_element(env, result, 1, gcohos::NapiMakeFloat64Array(env, ringCountsD));
    napi_set_element(env, result, 2, gcohos::NapiMakeFloat64Array(env, ringsPerFeatureD));
    napi_set_element(env, result, 3, gcohos::NapiMakeFloat64Array(env, lonlat));
    return result;
}

// ── 显示 / 标记 / 字体 ──

napi_value NativeSetDisplayDensity(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double density = 1;
    gcohos::NapiGetDouble(env, args[1], density);
    toGlobeEngine(handle)->setDisplayDensity(density);
    return nullptr;
}

napi_value NativeSetLocationMarker(napi_env env, napi_callback_info info) {
    napi_value args[5];
    gcohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double lon = 0, lat = 0, heading = -1;
    bool visible = false;
    gcohos::NapiGetDouble(env, args[1], lon);
    gcohos::NapiGetDouble(env, args[2], lat);
    gcohos::NapiGetBool(env, args[3], visible);
    gcohos::NapiGetDouble(env, args[4], heading);
    toGlobeEngine(handle)->setLocationMarker(lon, lat, visible, heading);
    return nullptr;
}

napi_value NativeSetLocationMarkerIcon(napi_env env, napi_callback_info info) {
    napi_value args[4];
    gcohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    const std::vector<int32_t> argb = gcohos::NapiGetInt32Array(env, args[1]);
    int32_t w = 0, h = 0;
    gcohos::NapiGetInt32(env, args[2], w);
    gcohos::NapiGetInt32(env, args[3], h);
    std::vector<uint8_t> rgba = iconArgbToRgba(argb, w, h);
    toGlobeEngine(handle)->setLocationMarkerIcon(std::move(rgba), w, h);
    return nullptr;
}

napi_value NativeSetFontPath(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->setFontPath(gcohos::NapiGetString(env, args[1]));
    return nullptr;
}

/// nativeSetCaBundle(path)：设置 HTTPS 证书校验用的 CA 证书包（cacert.pem）绝对路径（全局、无句柄）。
/// 对应引擎 HttpClient::setCaBundle。OHOS 无 Android 的 /system/etc/security/cacerts，随包内置 CA 后
/// 由 ArkTS 启动时拷入沙箱并传入，使瓦片 HTTPS 首握手即校验通过，免逐块“校验失败→降级重试”的双程时延。
/// 传空串回退引擎默认行为（Android 系统证书目录）。
napi_value NativeSetCaBundle(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    globecore::HttpClient::setCaBundle(gcohos::NapiGetString(env, args[0]));
    return nullptr;
}

/// nativeSetTerrainTile(handle, z, x, y, bytes, lonMin, latMin, lonMax, latMax)
/// 注入一块 quantized-mesh 地形瓦片（feature/terrain-pre）。
///
/// @param z/x/y    【本引擎瓦片网格】的键 —— 渲染时按 tileKey(z,x,y) 查地形缓存，
///                 必须与底图瓦片编号一致，否则查不到。
/// @param bytes    quantized-mesh v1.0 原始字节（ArkTS 侧下载后传入；传空字节数组=删除该块）
/// @param lonMin/latMin/lonMax/latMax  该块数据真实覆盖的经纬度四至（度）。
///                 标准 header 不含 bbox，必须由调用方按所用服务的分块方案算出。
///                 当外部地形服务的分块方案与本引擎不同（如 EPSG:4326 等经纬网格）时，
///                 z/x/y 用引擎编号、四至用服务编号算出的真实范围，即可正确采样。
///
/// 线程：可任意线程调用。内部只入队，GL 线程在下一帧帧首落到 terrainCache_。
/// 仅 3D 球体模式消费地形（2D 平面网格不走高度采样）。
napi_value NativeSetTerrainTile(napi_env env, napi_callback_info info) {
    napi_value args[9];
    gcohos::NapiGetArgs(env, info, 9, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t z = 0, x = 0, y = 0;
    gcohos::NapiGetInt32(env, args[1], z);
    gcohos::NapiGetInt32(env, args[2], x);
    gcohos::NapiGetInt32(env, args[3], y);
    std::vector<uint8_t> bytes = gcohos::NapiGetUint8Array(env, args[4]);
    double lonMin = 0, latMin = 0, lonMax = 0, latMax = 0;
    gcohos::NapiGetDouble(env, args[5], lonMin);
    gcohos::NapiGetDouble(env, args[6], latMin);
    gcohos::NapiGetDouble(env, args[7], lonMax);
    gcohos::NapiGetDouble(env, args[8], latMax);
    toGlobeEngine(handle)->setTerrainTile(z, x, y, bytes, lonMin, latMin, lonMax, latMax);
    return nullptr;
}

/// nativeClearTerrain(handle)：清空全部地形缓存（换地形源时调用）。
napi_value NativeClearTerrain(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->clearTerrain();
    return nullptr;
}

/// nativeConfigureTerrainProvider(handle, baseUrl, cacheDir, maxZoom, concurrency, tileRadius, headersJson)
/// @Jin Harmony 只注入配置；C++ Provider 负责视口调度、HTTP、磁盘缓存、去重重试和 DEM 注入。
napi_value NativeConfigureTerrainProvider(napi_env env, napi_callback_info info) {
    napi_value args[7];
    gcohos::NapiGetArgs(env, info, 7, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeBool(env, false);
    const std::string baseUrl = gcohos::NapiGetString(env, args[1]);
    const std::string cacheDir = gcohos::NapiGetString(env, args[2]);
    int32_t maxZoom = 15;
    int32_t concurrency = 4;
    int32_t tileRadius = 3;
    gcohos::NapiGetInt32(env, args[3], maxZoom);
    gcohos::NapiGetInt32(env, args[4], concurrency);
    gcohos::NapiGetInt32(env, args[5], tileRadius);
    const std::string headersJson = gcohos::NapiGetString(env, args[6]);

    std::vector<std::pair<std::string, std::string>> headers;
    const globecore::JsonValue root = globecore::parseJson(headersJson);
    if (root.isObject()) {
        for (const auto &entry : root.obj) {
            if (entry.second.isString()) headers.emplace_back(entry.first, entry.second.strVal);
        }
    }
    const bool ok = toGlobeEngine(handle)->configureTerrainProvider(
        baseUrl, cacheDir, maxZoom, concurrency, tileRadius, headers);
    return gcohos::NapiMakeBool(env, ok);
}

/// @Jin 开/关 C++ 地形 Provider；关闭时同时清空 Renderer DEM。
napi_value NativeSetTerrainProviderEnabled(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    bool enabled = false;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeBool(env, false);
    gcohos::NapiGetBool(env, args[1], enabled);
    return gcohos::NapiMakeBool(env, toGlobeEngine(handle)->setTerrainProviderEnabled(enabled));
}

/// @Jin 请求 C++ Provider 按最新相机视口刷新。
napi_value NativeRefreshTerrainProvider(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->refreshTerrainProvider();
    return nullptr;
}

/// @Jin 开关 C++ 帧循环中的地形自动跟随；默认关闭，宿主开启后无需在手势结束时手动刷新。
napi_value NativeSetTerrainProviderAutoFollow(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    bool enabled = false;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    gcohos::NapiGetBool(env, args[1], enabled);
    toGlobeEngine(handle)->setTerrainProviderAutoFollow(enabled);
    return nullptr;
}

/// @Jin 后台预取 layer.json。
napi_value NativePrefetchTerrainProviderMetadata(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->prefetchTerrainProviderMetadata();
    return nullptr;
}

/// @Jin 返回缓存命中/网络请求/去重/失败等单行 JSON 诊断状态。
napi_value NativeTerrainProviderStatus(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return gcohos::NapiMakeString(env, "{}");
    return gcohos::NapiMakeString(env, toGlobeEngine(handle)->terrainProviderStatsJson());
}

/// @Jin 注册/反注册统一地形事件回调；cb=null 时清除 Provider 回调。
napi_value NativeSetTerrainProviderEventCallback(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!gcohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;

    ReleaseTerrainEventListener(handle);
    napi_valuetype vt = napi_undefined;
    if (args[1] != nullptr) napi_typeof(env, args[1], &vt);
    if (vt != napi_function) {
        toGlobeEngine(handle)->setTerrainProviderEventCallback(nullptr);
        return nullptr;
    }

    napi_threadsafe_function tsfn = MakeTerrainEventTsfn(env, args[1]);
    if (tsfn == nullptr) return nullptr;
    auto listener = std::make_shared<TerrainEventListener>();
    listener->tsfn = tsfn;
    {
        std::lock_guard<std::mutex> lock(g_terrainEventMtx);
        g_terrainEventListeners[handle] = listener;
    }
    toGlobeEngine(handle)->setTerrainProviderEventCallback([listener](const std::string &json) {
        listener->emit(json);
    });
    return nullptr;
}

} // namespace

namespace gcohos {

/// 由 napi_init.cpp 统一注册：挂全部 GlobeEngine 导出到 exports（模块名 libglobecore）
napi_value RegisterGlobeEngine(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeCreate", nullptr, NativeCreate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeDestroy", nullptr, NativeDestroy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAttach", nullptr, NativeAttach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeDetach", nullptr, NativeDetach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRequestRender", nullptr, NativeRequestRender, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetCamera", nullptr, NativeSetCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeFlyToCamera", nullptr, NativeFlyToCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetCamera", nullptr, NativeGetCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetViewMode", nullptr, NativeSetViewMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetViewMode", nullptr, NativeGetViewMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePanBy", nullptr, NativePanBy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeZoomBy", nullptr, NativeZoomBy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRotateHeading", nullptr, NativeRotateHeading, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRotateTilt", nullptr, NativeRotateTilt, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddTileLayer", nullptr, NativeAddTileLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLayerVisible", nullptr, NativeSetLayerVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddRasterLayer", nullptr, NativeAddRasterLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRemoveTileLayer", nullptr, NativeRemoveTileLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetRasterAddError", nullptr, NativeGetRasterAddError, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddRasterImageLayer", nullptr, NativeAddRasterImageLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetRasterImageVisible", nullptr, NativeSetRasterImageVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHasRasterLoading", nullptr, NativeHasRasterLoading, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetRasterImageError", nullptr, NativeGetRasterImageError, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddVectorLayer", nullptr, NativeAddVectorLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateVectorExtent", nullptr, NativeUpdateVectorExtent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHasVectorLoading", nullptr, NativeHasVectorLoading, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetVectorLayerVisible", nullptr, NativeSetVectorLayerVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetVectorMinLevel", nullptr, NativeSetVectorMinLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetCameraZoomLevel", nullptr, NativeGetCameraZoomLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        // @Jin 瓦片 LOD 诊断与运行时粗一级实验开关，默认偏移 0，不影响既有调用方。
        {"nativeGetTileZoomLevel", nullptr, NativeGetTileZoomLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTileLodOffset", nullptr, NativeSetTileLodOffset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTileLodAlignDisplay", nullptr, NativeSetTileLodAlignDisplay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRemoveVectorLayer", nullptr, NativeRemoveVectorLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddOverlayLayer", nullptr, NativeAddOverlayLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayPoints", nullptr, NativeUpdateOverlayPoints, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayLines", nullptr, NativeUpdateOverlayLines, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayPolygons", nullptr, NativeUpdateOverlayPolygons, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRemoveOverlayLayer", nullptr, NativeRemoveOverlayLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetOverlayNoPick", nullptr, NativeSetOverlayNoPick, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeClearOverlayLayers", nullptr, NativeClearOverlayLayers, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePickVector", nullptr, NativePickVector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTapCallback", nullptr, NativeSetTapCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLongPressCallback", nullptr, NativeSetLongPressCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeScreenToGeo", nullptr, NativeScreenToGeo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeFeatureGeometry", nullptr, NativeFeatureGeometry, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetDisplayDensity", nullptr, NativeSetDisplayDensity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLocationMarker", nullptr, NativeSetLocationMarker, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLocationMarkerIcon", nullptr, NativeSetLocationMarkerIcon, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetFontPath", nullptr, NativeSetFontPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetCaBundle", nullptr, NativeSetCaBundle, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ── 三维地形（feature/terrain-pre）──
        {"nativeSetTerrainTile", nullptr, NativeSetTerrainTile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeClearTerrain", nullptr, NativeClearTerrain, nullptr, nullptr, nullptr, napi_default, nullptr},
        // @Jin C++ 地形 Provider 配置、开关、刷新、预取与诊断状态。
        {"nativeConfigureTerrainProvider", nullptr, NativeConfigureTerrainProvider, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTerrainProviderEnabled", nullptr, NativeSetTerrainProviderEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRefreshTerrainProvider", nullptr, NativeRefreshTerrainProvider, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTerrainProviderAutoFollow", nullptr, NativeSetTerrainProviderAutoFollow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePrefetchTerrainProviderMetadata", nullptr, NativePrefetchTerrainProviderMetadata, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTerrainProviderStatus", nullptr, NativeTerrainProviderStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTerrainProviderEventCallback", nullptr, NativeSetTerrainProviderEventCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace gcohos
