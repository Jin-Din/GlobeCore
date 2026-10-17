#pragma once
// MSVC 无 <unistd.h>。引擎以 <unistd.h> 取 access/F_OK、unlink 等 POSIX 文件 API；
// 兼容宏集中于 win_compat.h，这里仅转发，使 `#include <unistd.h>` 在桌面命中本头。
#include "win_compat.h"
