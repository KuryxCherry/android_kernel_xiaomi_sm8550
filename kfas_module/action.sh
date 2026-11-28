#!/system/bin/sh
if [ ! -d /sys/kernel/kfas ]; then
    echo "KFAS kernel interface (/sys/kernel/kfas) is not active on this kernel."
    exit 1
fi
echo "=== KFAS STATUS ==="
cat /sys/kernel/kfas/status
echo "=== KFAS PROBES ==="
cat /sys/kernel/kfas/probes
echo "=== KFAS THREADS ==="
cat /sys/kernel/kfas/threads
