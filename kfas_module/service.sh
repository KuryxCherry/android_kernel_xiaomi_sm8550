#!/system/bin/sh
MODDIR="${0%/*}"
KFAS_DIR="/sys/kernel/kfas"
USER_DIR="/sdcard/Android/kfas"
ERRLOG="$USER_DIR/kfas_error.log"

# 1. Wait for boot completion & storage mount
until [ "$(getprop sys.boot_completed)" = "1" ] && [ -d /sdcard/Android ]; do
    sleep 2
done

mkdir -p "$USER_DIR"
[ -f "$USER_DIR/games.conf" ] || cp -af "$MODDIR/games.conf" "$USER_DIR/games.conf"

if [ ! -d "$KFAS_DIR" ]; then
    echo "[$(date '+%m-%d %H:%M:%S')] KFAS sysfs (/sys/kernel/kfas) not present" >> "$ERRLOG"
    exit 0
fi

# 2. Configure libgui.so uprobe offsets dynamically (log errors only)
if ! sh "$MODDIR/kfas_resolve_probes.sh" >/dev/null 2>>"$ERRLOG"; then
    echo "[$(date '+%m-%d %H:%M:%S')] kfas_resolve_probes.sh failed" >> "$ERRLOG"
fi

# 3. Keep KFAS strictly game-focused by default (zero interference with daily apps
#    unless explicitly opted in via WebUI / tuning.conf)
[ -w "$KFAS_DIR/daily_enable" ] && echo 0 > "$KFAS_DIR/daily_enable" 2>/dev/null
sh "$MODDIR/kfas_ctl.sh" apply-saved >/dev/null 2>>"$ERRLOG"

# 4. Ensure single watcher instance
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
