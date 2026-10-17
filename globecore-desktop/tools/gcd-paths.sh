# shellcheck shell=bash
# Shared paths for the globecore-desktop Windows build scripts.
# PROJECT-LOCAL paths are derived from this file's own location (relocatable).
# SHARED toolchains (Qt / vcpkg / ANGLE / cmake.exe) stay absolute — they live
# outside the project and other projects reference them too.

# .../globecore-desktop/tools/gcd-paths.sh  ->  PROJECT_ROOT=.../globecore-desktop
GC_PATHS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PROJECT_ROOT="$(dirname "$GC_PATHS_DIR")"
export BUILD="$PROJECT_ROOT/build"        # CMake binary dir (was /d/gcd-build)
export THIRDP="$PROJECT_ROOT/thirdp"      # gdal/proj/curl/sqlite include junctions (was /d/gcd-3rdp)

# Shared toolchains (absolute, unchanged by this relocation)
export CMAKE="/d/vcpkg/downloads/tools/cmake-4.4.3-windows/cmake-4.4.3-windows-x86_64/bin/cmake.exe"
export QT5_BIN="/d/Qt/5.15.2/msvc2019_64/bin"
export QT5_PREFIX="D:/Qt/5.15.2/msvc2019_64"
export VCPKG_BIN="/d/vcpkg-2023/installed/x64-windows/bin"
export VCPKG_PREFIX="D:/vcpkg-2023/installed/x64-windows"
export ANGLE_DIR="D:/angle-msvc"
export NINJA="/c/Program Files (x86)/Microsoft Visual Studio/2019/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

# to_win /d/foo -> D:/foo  (CMake cache vars on Windows prefer drive-letter form)
to_win() { printf '%s' "$1" | sed -E 's#^/([a-zA-Z])(/|$)#\U\1:\2#'; }
