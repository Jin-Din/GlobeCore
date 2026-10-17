#pragma once
// Windows(MSVC) 桌面移植兼容头：引擎源码为 Android/鸿蒙 POSIX 编写，含少量 POSIX-only
// API。此头经 CMake 用 /FI 强制包含，仅在 MSVC 下进入编译；移动端编译根本不包含本目录，
// 故零影响。集中处理 mkdir/access/unlink/rmdir/strcasecmp/clock_gettime 等缺口。
#include <direct.h>   // _mkdir, _rmdir
#include <io.h>       // _access, _unlink
#include <sys/stat.h> // struct stat, stat
#include <sys/types.h>
#include <cstring>
#include <ctime>
#include <chrono>

// POSIX 文件访问模式宏（MSVC 的 io.h 用 _access 但不提供 F_OK 等名）
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

// 名字差异：POSIX → MSVC 下划线版
#ifndef mkdir
#define mkdir(path, mode) _mkdir(path)
#endif
#ifndef access
#define access _access
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef rmdir
#define rmdir _rmdir
#endif
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

// clock_gettime：UCRT 未导出；timespec 由 <ctime> 提供。用宏把 POSIX 调用改接自实现，
// 避免与将来可能存在的 clock_gettime 声明冲突。
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
inline int gc_win_clock_gettime(int clock_id, struct timespec *tp) {
    using namespace std::chrono;
    long long ns;
    if (clock_id == CLOCK_REALTIME)
        ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    else
        ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    tp->tv_sec = static_cast<time_t>(ns / 1000000000LL);
    tp->tv_nsec = static_cast<long>(ns % 1000000000LL);
    return 0;
}
#ifndef clock_gettime
#define clock_gettime(cid, tp) gc_win_clock_gettime((cid), (tp))
#endif
