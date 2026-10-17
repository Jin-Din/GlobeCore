#!/usr/bin/env bash
# WSLg 冒烟：测量进程存活时长 + 退出码，判定 GDAL/PROJ 矢量通路是否不再段错误
# 用法：linux-smoke.sh [额外 SMOKE 环境变量由调用方在命令里设好]
set -u
BUILD=/home/dtalp/globecore-linux/GlobeCore/globecore-desktop/build-linux
GEOJ=/home/dtalp/globecore-linux/GlobeCore/globecore-desktop/tools/sample.geojson
cd "$BUILD" || exit 90

S=$(date +%s.%N)
DISPLAY=:0 GLOBECORE_SMOKE_BASEMAP=1 GLOBECORE_SMOKE_3D=1 QT_FORCE_STDERR_LOGGING=1 \
    GLOBECORE_SMOKE_VECTOR="$GEOJ" \
    timeout 12 ./globecore-desktop > /home/dtalp/smoke_rt.log 2>&1
E=$?
N=$(date +%s.%N)
ELAPSED=$(echo "$N - $S" | bc)
# 退出码语义：124=被 timeout 正常到时杀（GUI 事件循环存活=good）；0/143/其它=自行结束或信号
# 段错误=139；127=找不到可执行
echo "EXIT=$E"
echo "ELAPSED=${ELAPSED}s"
echo "--- last 6 log lines ---"
tail -6 /home/dtalp/smoke_rt.log
echo "--- crash markers ---"
grep -c -iE 'segmentation|readVectorFile 完成|3D 流式上传完成|GL_MAX_TEXTURE_SIZE|ShaderProgram linked' /home/dtalp/smoke_rt.log || true
grep -iE 'segmentation|SIGSEGV|terminate|what\(\)' /home/dtalp/smoke_rt.log || echo "NO_CRASH_MARKERS"
