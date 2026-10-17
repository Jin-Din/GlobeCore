#pragma once
// MSVC 无 <dirent.h>。引擎（RasterRenderer 扫描 sidecar）仅用 opendir/readdir/closedir
// 且只读 d_name，故用 Win32 FindFirstFileA/FindNextFileA 提供最小 POSIX 目录遍历。
#include "win_compat.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <string>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE hFind;
    WIN32_FIND_DATAA findData;
    struct dirent entry;
    bool first;
} DIR;

inline DIR *opendir(const char *name) {
    if (name == nullptr) return nullptr;
    DIR *d = static_cast<DIR *>(std::malloc(sizeof(DIR)));
    if (d == nullptr) return nullptr;
    const std::string pattern = std::string(name) + "/*";
    d->hFind = FindFirstFileA(pattern.c_str(), &d->findData);
    if (d->hFind == INVALID_HANDLE_VALUE) {
        std::free(d);
        return nullptr;
    }
    d->first = true;
    return d;
}

inline struct dirent *readdir(DIR *d) {
    if (d == nullptr) return nullptr;
    if (d->first) {
        d->first = false;
    } else if (!FindNextFileA(d->hFind, &d->findData)) {
        return nullptr;
    }
    std::strncpy(d->entry.d_name, d->findData.cFileName, MAX_PATH - 1);
    d->entry.d_name[MAX_PATH - 1] = '\0';
    return &d->entry;
}

inline int closedir(DIR *d) {
    if (d == nullptr) return -1;
    if (d->hFind != INVALID_HANDLE_VALUE) FindClose(d->hFind);
    std::free(d);
    return 0;
}
