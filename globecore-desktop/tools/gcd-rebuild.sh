#!/bin/bash
# Reconfigure + build globecore-desktop in the VS2019 x64 MSVC environment.
# Qt5.15.2 (msvc2019_64): Qt6 dropped Windows ANGLE (AA_UseOpenGLES dead), so the
# desktop Windows build uses Qt5+ANGLE for a real GLES2 context.
source "$(dirname "${BASH_SOURCE[0]}")/vs2019-env.sh"
source "$(dirname "${BASH_SOURCE[0]}")/gcd-paths.sh"
vcvars_import || { echo "vcvars failed"; exit 2; }

THIRDP_WIN="$(to_win "$THIRDP")"
"$CMAKE" -S "$PROJECT_ROOT" -B "$BUILD" \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_PREFIX_PATH="$QT5_PREFIX;$VCPKG_PREFIX" \
  -DGLOBECORE_DESKTOP_GL=gles2 \
  -DGLOBECORE_ANGLE_DIR="$ANGLE_DIR" \
  -DGLOBECORE_3RDP_INC="$THIRDP_WIN" 2>&1 | tail -6
echo "=== CONFIGURE DONE ==="
"$CMAKE" --build "$BUILD" -j 8
echo "=== BUILD EXIT $? ==="
