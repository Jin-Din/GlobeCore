#!/usr/bin/env bash
# 验证 Linux 地图标注字体：清旧实例 → 带标注 KML 跑 → 打印字体/标注相关日志
set -u
BUILD=/home/dtalp/globecore-linux/GlobeCore/globecore-desktop/build-linux
KML=/mnt/d/Android/MobileMap/GlobeCore/globecore-desktop/tools/KML_Samples.kml
pkill -f 'build-linux/globecore-desktop' 2>/dev/null || true
sleep 1
cd "$BUILD" || exit 90
DISPLAY=:0 QT_FORCE_STDERR_LOGGING=1 GLOBECORE_SMOKE_BASEMAP=1 GLOBECORE_SMOKE_VECTOR="$KML" \
    timeout 9 ./globecore-desktop > /home/dtalp/label_test.log 2>&1
echo "EXIT=$?"
echo "---- label/font lines ----"
grep -aiE 'font|atlas|label|标注|字形|glyph|msyh|simhei|noto|readVectorFile 完成|labels=' /home/dtalp/label_test.log | head -20
echo "---- full tail ----"
tail -8 /home/dtalp/label_test.log
