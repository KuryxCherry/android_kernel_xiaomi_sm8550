#!/system/bin/sh
# Unified control backend for KFAS Controller WebUI & CLI.

MODDIR="${0%/*}"
KFAS_DIR="/sys/kernel/kfas"
USER_DIR="/sdcard/Android/kfas"
GAMES_CONF="$USER_DIR/games.conf"
TUNING_CONF="$USER_DIR/tuning.conf"
LOG_FILE="$USER_DIR/kfas_error.log"

mkdir -p "$USER_DIR"
[ -f "$GAMES_CONF" ] || [ ! -f "$MODDIR/games.conf" ] || cp -af "$MODDIR/games.conf" "$GAMES_CONF"

read_node() {
    if [ -r "$KFAS_DIR/$1" ]; then
        cat "$KFAS_DIR/$1" 2>/dev/null
    fi
}

persist_kv() {
    key="$1"
    val="$2"
    tmp="$TUNING_CONF.tmp"
    touch "$TUNING_CONF"
    awk -v k="$key" '$1 != k { print }' "$TUNING_CONF" > "$tmp"
    printf '%s %s\n' "$key" "$val" >> "$tmp"
    mv -f "$tmp" "$TUNING_CONF"
}

persist_tune() {
    mode="$1"
    param="$2"
    val="$3"
    key="tune:${mode}:${param}"
    tmp="$TUNING_CONF.tmp"
    touch "$TUNING_CONF"
    awk -v k="$key" '$1 != k { print }' "$TUNING_CONF" > "$tmp"
    printf '%s %s\n' "$key" "$val" >> "$tmp"
    mv -f "$tmp" "$TUNING_CONF"
}

cmd="$1"
shift

case "$cmd" in
snapshot)
    echo "===META==="
    echo "kernel=$(uname -r)"
    if [ -d "$KFAS_DIR" ]; then
        echo "kfas_present=1"
    else
        echo "kfas_present=0"
    fi
    wpid=$(ps -ef 2>/dev/null | awk '/kfas_watcher\.sh/ && !/awk/ {print $2; exit}')
    echo "watcher_pid=${wpid:-0}"
    echo "enable=$(read_node enable)"
    echo "mode=$(read_node mode)"
    echo "placement_enable=$(read_node placement_enable)"
    echo "contain_workers=$(read_node contain_workers)"
    echo "unpause_big=$(read_node unpause_big)"
    echo "daily_enable=$(read_node daily_enable)"
    echo "daily_pid=$(read_node daily_pid)"
    echo "dcvs_enable=$(read_node dcvs_enable)"
    echo "vip_comms=$(read_node vip_comms)"
    echo "probes=$(read_node probes)"
    echo "target=$(read_node target)"
    echo "===STATUS==="
    read_node status
    echo "===THREADS==="
    read_node threads
    echo "===TUNING==="
    read_node tuning
    echo "===GAMES==="
    [ -r "$GAMES_CONF" ] && cat "$GAMES_CONF"
    echo "===LOG==="
    [ -r "$LOG_FILE" ] && tail -n 30 "$LOG_FILE"
    echo "===END==="
    ;;

set)
    key="$1"
    val="$2"
    case "$key" in
        enable|mode|placement_enable|contain_workers|unpause_big|daily_enable|dcvs_enable|vip_comms)
            if [ -w "$KFAS_DIR/$key" ]; then
                printf '%s\n' "$val" > "$KFAS_DIR/$key" || exit 1
            fi
            persist_kv "$key" "$val"
            ;;
        *)
            echo "invalid key: $key" >&2
            exit 1
            ;;
    esac
    ;;

tune)
    mode="$1"
    param="$2"
    val="$3"
    case "$mode" in
        powersave|balance|performance|fast) ;;
        *) echo "invalid mode" >&2; exit 1 ;;
    esac
    case "$param" in
        margin_pct|up_gain|down_gain|down_hold|little_floor_khz|temp_mc|worker_umax|crit_umin|prime) ;;
        *) echo "invalid param" >&2; exit 1 ;;
    esac
    case "$val" in
        ''|*[!0-9]*) echo "invalid value" >&2; exit 1 ;;
    esac
    if [ -w "$KFAS_DIR/tuning" ]; then
        printf '%s %s %s\n' "$mode" "$param" "$val" > "$KFAS_DIR/tuning" || exit 1
    fi
    persist_tune "$mode" "$param" "$val"
    ;;

reset-tuning)
    rm -f "$TUNING_CONF"
    echo "cleared"
    ;;

game-set)
    pkg="$1"
    fps="$2"
    [ -n "$pkg" ] && [ -n "$fps" ] || { echo "missing args" >&2; exit 1; }
    case "$pkg" in
        *[!a-zA-Z0-9._-]*) echo "invalid package" >&2; exit 1 ;;
    esac
    case "$fps" in
        *[!0-9,]*) echo "invalid fps list" >&2; exit 1 ;;
    esac
    tmp="$GAMES_CONF.tmp"
    touch "$GAMES_CONF"
    awk -F'=' -v p="$pkg" '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { print; next }
        {
            k = $1
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", k)
            gsub(/"/, "", k)
            if (k != p) print
        }
    ' "$GAMES_CONF" > "$tmp"
    printf '%s = %s\n' "$pkg" "$fps" >> "$tmp"
    mv -f "$tmp" "$GAMES_CONF"
    ;;

game-del)
    pkg="$1"
    [ -n "$pkg" ] || exit 1
    [ -f "$GAMES_CONF" ] || exit 0
    tmp="$GAMES_CONF.tmp"
    awk -F'=' -v p="$pkg" '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { print; next }
        {
            k = $1
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", k)
            gsub(/"/, "", k)
            if (k != p) print
        }
    ' "$GAMES_CONF" > "$tmp"
    mv -f "$tmp" "$GAMES_CONF"
    ;;

list-apps)
    pm list packages -3 2>/dev/null | sed 's/^package://' | sort
    ;;

resolve-probes)
    [ -d "$KFAS_DIR" ] || { echo "KFAS kernel not active" >&2; exit 1; }
    sh "$MODDIR/kfas_resolve_probes.sh"
    ;;

detach)
    [ -w "$KFAS_DIR/target" ] && echo 0 > "$KFAS_DIR/target"
    [ -w "$KFAS_DIR/daily_pid" ] && echo 0 > "$KFAS_DIR/daily_pid"
    echo "detached"
    ;;

restart-watcher)
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
    if [ -d "$KFAS_DIR" ]; then
        nohup sh "$MODDIR/kfas_watcher.sh" </dev/null >/dev/null 2>&1 &
        echo "restarted"
    else
        echo "KFAS kernel not active" >&2
        exit 1
    fi
    ;;

apply-saved)
    [ -d "$KFAS_DIR" ] && [ -f "$TUNING_CONF" ] || exit 0
    while read -r k v rest; do
        [ -n "$k" ] || continue
        case "$k" in
            enable|mode|placement_enable|contain_workers|unpause_big|daily_enable|dcvs_enable|vip_comms)
                [ -w "$KFAS_DIR/$k" ] && printf '%s\n' "$v" > "$KFAS_DIR/$k" 2>/dev/null
                ;;
            tune:*)
                m=$(printf '%s' "$k" | cut -d: -f2)
                p=$(printf '%s' "$k" | cut -d: -f3)
                [ -w "$KFAS_DIR/tuning" ] && printf '%s %s %s\n' "$m" "$p" "$v" > "$KFAS_DIR/tuning" 2>/dev/null
                ;;
        esac
    done < "$TUNING_CONF"
    ;;

*)
    echo "usage: kfas_ctl.sh <snapshot|set|tune|reset-tuning|game-set|game-del|list-apps|resolve-probes|detach|restart-watcher|apply-saved>" >&2
    exit 1
    ;;
esac
