#!/system/bin/sh
# Scene powercfg bridge for KFAS (/sys/kernel/kfas/mode + /dev/kfas/mode)
# Silent on normal switches; logs errors only to minimize I/O overhead.

KMODE="/sys/kernel/kfas/mode"
DMODE="/dev/kfas/mode"
ERRLOG="/sdcard/Android/kfas/kfas_error.log"

case "$1" in
"powersave" | "standby")
    m="powersave"
    ;;
"balance")
    m="balance"
    ;;
"performance")
    m="performance"
    ;;
"init")
    # Scene one-time init hook: preserve saved KFAS mode instead of forcing fast
    exit 0
    ;;
"fast" | "pedestal")
    m="fast"
    ;;
*)
    exit 0
    ;;
esac

# Fast-path skip if mode is already synced
if [ -r "$DMODE" ] && [ "$(cat "$DMODE" 2>/dev/null)" = "$m" ] &&
   [ -r "$KMODE" ] && [ "$(cat "$KMODE" 2>/dev/null)" = "$m" ]; then
    exit 0
fi

[ -d /dev/kfas ] || mkdir -p /dev/kfas 2>/dev/null
if [ -w "$KMODE" ]; then
    if ! echo "$m" > "$KMODE" 2>/dev/null; then
        [ -d /sdcard/Android/kfas ] && echo "[$(date '+%m-%d %H:%M:%S')] powercfg write failed: $m" >> "$ERRLOG" 2>/dev/null
    fi
fi
echo "$m" > "$DMODE" 2>/dev/null
chmod 666 "$DMODE" 2>/dev/null
exit 0
