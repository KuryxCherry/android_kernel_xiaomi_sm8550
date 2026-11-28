#!/system/bin/sh
wpids=$(ps -ef 2>/dev/null | awk '$NF ~ /kfas_watcher\.sh$/ {print $2}')
for p in $wpids; do
    kill "$p" 2>/dev/null || true
done
[ -w /sys/kernel/kfas/target ] && echo 0 > /sys/kernel/kfas/target 2>/dev/null
[ -w /sys/kernel/kfas/daily_pid ] && echo 0 > /sys/kernel/kfas/daily_pid 2>/dev/null
rm -rf /sdcard/Android/kfas
