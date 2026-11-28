#!/system/bin/sh
# Event-driven zero-poll foreground watcher for KFAS (Game Mode + Daily App Thread Assist).
# Sleeps at 0% CPU inside logcat -b events until top activity or screen state changes.
# Logs errors only to keep storage I/O at zero during normal operation.

MODDIR="${0%/*}"
CFG="/sdcard/Android/kfas/games.conf"
KFAS_DIR="/sys/kernel/kfas"
KFAS_TARGET="$KFAS_DIR/target"
KFAS_DAILY_PID="$KFAS_DIR/daily_pid"
KFAS_DAILY_EN="$KFAS_DIR/daily_enable"
ERRLOG="/sdcard/Android/kfas/kfas_error.log"

CUR_KIND=""
CUR_PKG=""
CUR_PID="0"

log_err() {
    [ -d /sdcard/Android/kfas ] && echo "[$(date '+%m-%d %H:%M:%S')] $*" >> "$ERRLOG" 2>/dev/null
}

lookup_game_fps() {
    pkg="$1"
    [ -f "$CFG" ] || return 1
    awk -F'=' -v p="$pkg" '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        {
            k = $1; v = $2
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", k)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
            gsub(/"/, "", k)
            if (k == p && v != "") {
                print v
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$CFG"
}

# Strictly exclude banking, payment, wallet, and critical system packages from daily assist.
is_daily_excluded() {
    case "$1" in
        android|com.android.systemui|com.android.settings|com.android.phone|\
        com.android.incallui|com.android.permissioncontroller|\
        com.android.packageinstaller|com.google.android.packageinstaller|\
        com.omarea.vtools|me.weishu.kernelsu|org.rouyashiki.yukisu|\
        *yukisu*|*kernelsu*|*magisk*|com.myagy.agent|com.foldagy.agent|\
        com.eg.android.AlipayGphone|com.unionpay*|cn.gov.pbc.*|\
        com.icbc*|com.ccb*|com.chinamworld.*|com.cmbchina*|com.abchina*|\
        com.boc*|com.psbc*|com.pingan*|com.citicbank*|com.spdb*|\
        com.cgbchina*|com.mybank*|com.webank*|com.cib.*|com.cebbank*|\
        com.hxb.*|com.cmbc.*|com.paypal.*|com.google.android.apps.walletnfcrel|\
        com.samsung.android.spay*|com.mipay.*|com.miui.tsmclient|\
        *bank*|*Bank*|*wallet*|*Wallet*|*alipay*|*Alipay*|*unionpay*|*UnionPay*|*payment*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

detach_all() {
    if [ "$CUR_KIND" = "game" ] && [ "$CUR_PID" != "0" ]; then
        echo 0 > "$KFAS_TARGET" 2>/dev/null
    elif [ "$CUR_KIND" = "daily" ] && [ "$CUR_PID" != "0" ]; then
        [ -w "$KFAS_DAILY_PID" ] && echo 0 > "$KFAS_DAILY_PID" 2>/dev/null
    fi
    CUR_KIND=""
    CUR_PID="0"
    CUR_PKG=""
}

wait_pid() {
    pkg="$1"
    tries=0
    while [ "$tries" -lt 8 ]; do
        pid=$(pidof "$pkg" 2>/dev/null | awk '{print $1}')
        if [ -n "$pid" ]; then
            printf '%s\n' "$pid"
            return 0
        fi
        tries=$((tries + 1))
        sleep 0.25
    done
    return 1
}

attach_game() {
    pkg="$1"
    fps="$2"
    [ "$CUR_KIND" = "daily" ] && detach_all
    pid=$(wait_pid "$pkg") || { log_err "watcher: pidof failed for $pkg"; return; }

    if [ "$CUR_KIND" = "game" ] && [ "$pid" = "$CUR_PID" ]; then
        return
    fi

    if echo "$pid $fps" > "$KFAS_TARGET" 2>/dev/null; then
        CUR_KIND="game"
        CUR_PKG="$pkg"
        CUR_PID="$pid"
    else
        log_err "watcher: attach failed pkg=$pkg pid=$pid fps=$fps"
    fi
}

attach_daily() {
    pkg="$1"
    [ "$CUR_KIND" = "game" ] && detach_all
    [ -w "$KFAS_DAILY_PID" ] || { detach_all; return; }
    [ "$(cat "$KFAS_DAILY_EN" 2>/dev/null)" = "1" ] || { detach_all; return; }

    if is_daily_excluded "$pkg"; then
        detach_all
        return
    fi

    pid=$(wait_pid "$pkg") || return
    if [ "$CUR_KIND" = "daily" ] && [ "$pid" = "$CUR_PID" ]; then
        return
    fi

    if echo "$pid" > "$KFAS_DAILY_PID" 2>/dev/null; then
        CUR_KIND="daily"
        CUR_PKG="$pkg"
        CUR_PID="$pid"
    fi
}

handle_pkg() {
    pkg="$1"
    [ -n "$pkg" ] || return
    [ "$pkg" = "$CUR_PKG" ] && [ "$CUR_PID" != "0" ] && [ -d "/proc/$CUR_PID" ] && return

    fps=$(lookup_game_fps "$pkg")
    if [ -n "$fps" ]; then
        attach_game "$pkg" "$fps"
    else
        attach_daily "$pkg"
    fi
}

while true; do
    [ -d "$KFAS_DIR" ] || { sleep 5; continue; }

    logcat -b events -v brief -T 1 -s \
        wm_set_resumed_activity:I \
        wm_resumed_activity:I \
        screen_toggled:I 2>/dev/null | while IFS= read -r line; do
        case "$line" in
            *screen_toggled*:*0*)
                detach_all
                ;;
            *screen_toggled*:*1*)
                if [ "$CUR_PID" = "0" ]; then
                    pkg=$(dumpsys activity activities 2>/dev/null | awk '
                        /mResumedActivity|topResumedActivity/ {
                            for (i = 1; i <= NF; i++) {
                                if ($i ~ /^[a-zA-Z0-9._-]+\//) {
                                    sub(/\/.*/, "", $i)
                                    print $i
                                    exit
                                }
                            }
                        }')
                    [ -n "$pkg" ] && handle_pkg "$pkg"
                fi
                ;;
            *wm_set_resumed_activity*:*|*wm_resumed_activity*:*)
                # Format: I/wm_set_resumed_activity( 2007): [0,com.pkg/.Activity,...]
                pkg=$(printf '%s\n' "$line" | sed -n 's/.*\[[0-9]*,\([^\/,]*\)\/.*/\1/p')
                [ -n "$pkg" ] && handle_pkg "$pkg"
                ;;
        esac
    done

    sleep 2
done
