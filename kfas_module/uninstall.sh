#!/system/bin/sh
pkill -f "kfas_watcher.sh" 2>/dev/null || true
[ -w /sys/kernel/kfas/target ] && echo 0 > /sys/kernel/kfas/target 2>/dev/null

if [ -f /data/powercfg.json ] && grep -q '"kfas_ctrl"' /data/powercfg.json; then
    rm -f /data/powercfg.json
fi
if [ -f /data/powercfg.sh ] && grep -q '/sys/kernel/kfas/mode' /data/powercfg.sh; then
    rm -f /data/powercfg.sh
fi
rm -rf /dev/kfas
rm -rf /sdcard/Android/kfas
