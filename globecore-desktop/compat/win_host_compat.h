// win_host_compat.h —— 桌面宿主专用（仅 globecore-desktop，/FI 强制包含）。
// 引擎 geom/Camera.h 声明 `enum class AltitudeMode { ABSOLUTE = 0, ... }`；而宿主经 Qt 拉入
// <windows.h>，其 <wingdi.h> 把 ABSOLUTE/RELATIVE 定义为 1/2，遂把枚举成员写成 `1 = 0`，
// 语法报错并拖垮整个 namespace globecore 区块（Matrix4/Navigator/Tessellator 连带 FrustumPlane 等）。
// 引擎库自身不链 Qt、不含 windows.h，故此冲突纯属宿主编译期问题，修复只放宿主侧、不动共享引擎头。
//
// 手法：/FI 使本头最先解析——先「主动」包含 <windows.h>（置位 wingdi 的包含卫哨），随即 #undef
// 冲突宏。此后 Qt 再包含 windows.h/wingdi.h 因卫哨已置而为空操作，无法重设 ABSOLUTE，全 TU 保持干净。
// NOMINMAX 阻止 wingdi/windef 的 MIN/MAX 宏污染 std::min/std::max（引擎与 Qt 均大量使用）。
#ifndef GLOBECORE_DESKTOP_WIN_HOST_COMPAT_H
#define GLOBECORE_DESKTOP_WIN_HOST_COMPAT_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#ifdef ABSOLUTE
#undef ABSOLUTE
#endif
#ifdef RELATIVE
#undef RELATIVE
#endif

// 同理，windef.h 把 near/far/small/large 定义为（空）宏，会破坏引擎里以这些常见词
// 作标识符的代码（geom/Matrix4.h::perspective(..., float near, float far) 等）。
// 它们并非 C++ 关键字，引擎库与 Qt 都不依赖其为宏，undef 安全。
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef small
#undef small
#endif
#ifdef large
#undef large
#endif

#endif // _WIN32

#endif // GLOBECORE_DESKTOP_WIN_HOST_COMPAT_H
