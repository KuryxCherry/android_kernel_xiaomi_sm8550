#!/system/bin/sh
MODDIR="${0%/*}"
KFAS_DIR="/sys/kernel/kfas"
USER_DIR="/sdcard/Android/kfas"
ERRLOG="$USER_DIR/kfas_error.log"

# 0. Apply tuned PowerHAL powerhint.json transiently without leaving any mount
#    PowerHAL (libperfmgr) parses /vendor/etc/powerhint.json once at startup
#    and keeps it in memory; unmounting immediately leaves zero mount traces
#    in PID 1 and Android 17 zygote_next isolated mount namespaces.
apply_powerhint_clean() {
    src="$MODDIR/powerhint.json"
    dst="/vendor/etc/powerhint.json"
    [ -f "$src" ] && [ -f "$dst" ] || return 0
    chmod 0644 "$src" 2>/dev/null || true
    chcon u:object_r:vendor_configs_file:s0 "$src" 2>/dev/null || true
    nsenter -t 1 -m -- umount "$dst" 2>/dev/null || true
    if nsenter -t 1 -m -- mount -o bind,ro "$src" "$dst" 2>/dev/null; then
        stop vendor.power-hal-aidl 2>/dev/null || true
        i=0
        while [ "$i" -lt 10 ]; do
            [ "$(getprop init.svc.vendor.power-hal-aidl)" = "stopped" ] && break
            sleep 0.1
            i=$((i + 1))
        done
        start vendor.power-hal-aidl 2>/dev/null || true
        i=0
        while [ "$i" -lt 30 ]; do
            if [ "$(getprop init.svc.vendor.power-hal-aidl)" = "running" ] && \
               dumpsys android.hardware.power.IPower/default 2>/dev/null | grep -q 'CPULittleClusterMinFreq'; then
                break
            fi
            sleep 0.1
            i=$((i + 1))
        done
        nsenter -t 1 -m -- umount "$dst" 2>/dev/null || nsenter -t 1 -m -- umount -l "$dst" 2>/dev/null || true
    fi
}
apply_powerhint_clean

# 1. Immediately expose Scene powercfg bridge & /dev/kfas/mode BEFORE boot_completed
if [ -d "$KFAS_DIR" ]; then
    mkdir -p /dev/kfas
    chmod 777 /dev/kfas
    cur_mode=$(cat "$KFAS_DIR/mode" 2>/dev/null)
    echo "${cur_mode:-balance}" > /dev/kfas/mode
    chmod 666 /dev/kfas/mode
    cp -f "$MODDIR/vtools/powercfg.json" /data/powercfg.json
    cp -f "$MODDIR/vtools/powercfg.sh" /data/powercfg.sh
    chmod 644 /data/powercfg.json
    chmod 755 /data/powercfg.sh
fi

# 2. Wait for boot completion & storage mount
until [ "$(getprop sys.boot_completed)" = "1" ] && [ -d /sdcard/Android ]; do
    sleep 2
done

# 2b. Apply verified SM8550 daily-use WALT / core_ctl / cgroup / min_freq_limit tuning
apply_daily_power_tuning() {
    [ -w /proc/sys/walt/sched_conservative_pl ] && echo 1 > /proc/sys/walt/sched_conservative_pl 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy0/walt/rtg_boost_freq ] && echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/rtg_boost_freq 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us ] && echo 10000 > /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy3/walt/down_rate_limit_us ] && echo 10000 > /sys/devices/system/cpu/cpufreq/policy3/walt/down_rate_limit_us 2>/dev/null
    [ -w /sys/devices/system/cpu/cpu3/core_ctl/min_cpus ] && echo 3 > /sys/devices/system/cpu/cpu3/core_ctl/min_cpus 2>/dev/null
    [ -w /sys/devices/system/cpu/cpu7/core_ctl/busy_up_thres ] && echo 80 > /sys/devices/system/cpu/cpu7/core_ctl/busy_up_thres 2>/dev/null
    [ -w /sys/devices/system/cpu/cpu7/core_ctl/busy_down_thres ] && echo 50 > /sys/devices/system/cpu/cpu7/core_ctl/busy_down_thres 2>/dev/null
    [ -w /dev/cpuctl/background/cpu.uclamp.max ] && echo 45 > /dev/cpuctl/background/cpu.uclamp.max 2>/dev/null
    [ -w /dev/cpuctl/system-background/cpu.uclamp.max ] && echo 45 > /dev/cpuctl/system-background/cpu.uclamp.max 2>/dev/null
    [ -w /dev/cpuset/restricted/cpus ] && echo 0-2 > /dev/cpuset/restricted/cpus 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq_limit ] && echo 1344000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq_limit 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy3/scaling_min_freq_limit ] && echo 1785600 > /sys/devices/system/cpu/cpufreq/policy3/scaling_min_freq_limit 2>/dev/null
    [ -w /sys/devices/system/cpu/cpufreq/policy7/scaling_min_freq_limit ] && echo 1843200 > /sys/devices/system/cpu/cpufreq/policy7/scaling_min_freq_limit 2>/dev/null
}
apply_daily_power_tuning

mkdir -p "$USER_DIR"
[ -f "$USER_DIR/games.conf" ] || cp -af "$MODDIR/games.conf" "$USER_DIR/games.conf"

if [ ! -d "$KFAS_DIR" ]; then
    echo "[$(date '+%m-%d %H:%M:%S')] KFAS sysfs (/sys/kernel/kfas) not present" >> "$ERRLOG"
    exit 0
fi

# 3. Configure libgui.so uprobe offsets dynamically (log errors only)
if ! sh "$MODDIR/kfas_resolve_probes.sh" >/dev/null 2>>"$ERRLOG"; then
    echo "[$(date '+%m-%d %H:%M:%S')] kfas_resolve_probes.sh failed" >> "$ERRLOG"
fi

# 4. Replay saved WebUI toggles & per-mode tuning overrides (errors only)
sh "$MODDIR/kfas_ctl.sh" apply-saved >/dev/null 2>>"$ERRLOG"
cur_mode=$(cat "$KFAS_DIR/mode" 2>/dev/null)
echo "${cur_mode:-balance}" > /dev/kfas/mode
chmod 666 /dev/kfas/mode

# 5. Ensure single watcher instance
wpids=$(ps -ef 2>/dev/null | awk '$NF ~ /kfas_watcher\.sh$/ {print $2}')
for p in $wpids; do
    [ "$p" != "$$" ] && [ "$p" != "$PPID" ] || continue
    for c in $(ps -ef 2>/dev/null | awk -v pp="$p" '$3 == pp {print $2}'); do
        kill "$c" 2>/dev/null || true
    done
    kill "$p" 2>/dev/null || true
done
sleep 0.2
for p in $(ps -ef 2>/dev/null | awk '$NF ~ /kfas_watcher\.sh$/ {print $2}'); do
    [ "$p" != "$$" ] && [ "$p" != "$PPID" ] && kill -9 "$p" 2>/dev/null || true
done
nohup sh "$MODDIR/kfas_watcher.sh" </dev/null >/dev/null 2>&1 &
