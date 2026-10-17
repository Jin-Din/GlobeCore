#!/usr/bin/env bash
# 真实超大栅格压测：帽帽山.tif（~1.3GB）。验证引擎双帽是否把整幅直读降到有界尺寸、不 native OOM。
# 中文路径写在本脚本内（UTF-8），命令行只引用本脚本的 ASCII 路径，规避 wsl.exe 命令层中文编码破坏。
set -u
BUILD=/home/dtalp/globecore-linux/GlobeCore/globecore-desktop/build-linux
# 默认压 1.3GB 真实投影栅格；可用 TIF/WINDOW 环境变量覆盖（如跑小样张验证完成态）。
TIF="${TIF:-/mnt/d/Android/MobileMap/示例数据/帽帽山.tif}"
LOG="${LOG:-/home/dtalp/big_ras.log}"

echo "=== 目标文件 ==="
ls -la "$TIF" 2>&1 | head -1

cd "$BUILD" || { echo "no build dir"; exit 90; }
pkill -f 'globecore-desktop$' 2>/dev/null || true

# 后台起：只挂栅格（不设底图，减噪）。投影栅格需现场 VRT 重投影+抽稀，给足时间。
# 记 $! 为 timeout 包装作业 PID，末尾 wait 该 PID 才能拿到真实退出码（无参 wait 恒返回 0，不可信）。
WINDOW="${WINDOW:-240}"
T0=$(date +%s)
DISPLAY=:0 QT_FORCE_STDERR_LOGGING=1 GLOBECORE_SMOKE_RASTER="$TIF" \
    timeout "$WINDOW" ./globecore-desktop > "$LOG" 2>&1 &
PID=$!

# 采样进程组峰值 RSS（GDAL 解码/VRT warp + RGBA 缓冲的内存真相）+ 并发实例数
PEAK=0
PEAK_PROCS=0
for _ in $(seq 1 $((WINDOW * 2))); do
    PIDS=$(pgrep -f 'globecore-desktop$' 2>/dev/null)
    NP=$(echo "$PIDS" | grep -c . )
    [ "$NP" -gt "$PEAK_PROCS" ] 2>/dev/null && PEAK_PROCS=$NP
    R=$(echo "$PIDS" | while read -r p; do ps -o rss= -p "$p" 2>/dev/null; done | sort -n | tail -1 | tr -d ' ')
    [ -n "$R" ] && [ "$R" -gt "$PEAK" ] 2>/dev/null && PEAK=$R
    [ -n "$PIDS" ] || break
    sleep 0.5
done
wait "$PID"
EXIT=$?
ELAPSED=$(( $(date +%s) - T0 ))

echo "=== PEAK_RSS_KB=$PEAK (~$((PEAK/1024))MB) ==="
echo "=== PEAK_PROCS=$PEAK_PROCS (并发实例数，应=1) ==="
echo "=== ELAPSED=${ELAPSED}s (窗口 ${WINDOW}s) ==="
echo "=== free -m ==="; free -m | head -2
echo "=== 退出码（124=到点仍在读;0=读完自退;137=OOM-kill;139=段错误;143=SIGTERM）==="
echo "EXIT=$EXIT"

echo "=== 关键日志 ==="
grep -aiE 'GetRasterEntry|renderFullImage|Raster.*(ready|done|完成|upload)|ready|dst=|src=[0-9]|Killed|out of memory|bad_alloc|无 ?SRS|SIGSEGV|overview|VRT|texture' "$LOG" | head -40
echo "=== tail ==="
tail -4 "$LOG"
