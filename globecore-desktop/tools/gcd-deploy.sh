#!/bin/bash
# gcd-deploy.sh — turn build/globecore-desktop.exe into a standalone runnable artifact.
# Qt windeployqt copies Qt runtime + platforms/qwindows.dll + styles; then we add the two
# families Qt does NOT handle:
#   (a) ANGLE: Qt5 ships libEGL/libGLESv2/d3dcompiler_47 (GLES2 backend, AA_UseOpenGLES path)
#   (b) vcpkg GDAL runtime + its DLL imports: gdal/proj/sqlite3/libcurl + geotiff/tiff/jpeg/
#       turbojpeg/json-c/liblzma. NOTE gdal.dll and libcurl.dll both import zlib1.dll (missed once).
# proj.db is auto-copied beside the exe by CMake POST_BUILD into proj/; this only verifies it.
set -e
source "$(dirname "${BASH_SOURCE[0]}")/gcd-paths.sh"
cd "$BUILD"

"$QT5_BIN/windeployqt.exe" --release --no-translations --no-compiler-runtime globecore-desktop.exe >/dev/null

# ANGLE GLES2 provider (Qt5 build, matches AA_UseOpenGLES)
for d in libEGL.dll libGLESv2.dll d3dcompiler_47.dll; do
  [ -f "$d" ] || cp -f "$QT5_BIN/$d" .
done

# GDAL/PROJ/curl/sqlite runtime + image/compression libs they import
for d in gdal.dll proj.dll sqlite3.dll libcurl.dll zlib1.dll \
         geotiff.dll tiff.dll jpeg62.dll turbojpeg.dll json-c.dll liblzma.dll; do
  cp -f "$VCPKG_BIN/$d" .
done

[ -f proj/proj.db ] || { echo "WARN: proj/proj.db missing (POST_BUILD not fired?); manual copy"; mkdir -p proj && cp -f "$PROJECT_ROOT/../globecore/src/main/assets/proj/proj.db" proj/; }

echo "=== beside-exe runtime ==="; ls *.dll | tr '\n' ' '; echo
echo "=== platforms/styles ==="; ls platforms/ 2>/dev/null; ls styles/ 2>/dev/null
echo "DEPLOY OK"
