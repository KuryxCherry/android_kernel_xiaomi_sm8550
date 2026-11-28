#!/system/bin/sh
# Resolve exact ELF file offsets for Surface::queueBuffer / Surface::dequeueBuffer
# from /system/lib64/libgui.so and configure /sys/kernel/kfas/probes.

LIB="/system/lib64/libgui.so"
KFAS_PROBES="/sys/kernel/kfas/probes"

[ -f "$LIB" ] || exit 1
[ -w "$KFAS_PROBES" ] || exit 1

# Compute file_offset = st_value - p_vaddr + p_offset using the R E PT_LOAD segment
resolve_sym_offset() {
    sym="$1"
    vaddr_hex=$(readelf -Ws "$LIB" 2>/dev/null | awk -v s="$sym" '$8 == s && $4 == "FUNC" { print $2; exit }')
    [ -n "$vaddr_hex" ] || return 1

    # Find matching executable PT_LOAD segment (p_vaddr <= vaddr < p_vaddr + p_filesz)
    readelf -lW "$LIB" 2>/dev/null | awk -v vh="$vaddr_hex" '
        function h2d(h,   i, c, n, d) {
            gsub(/^0x/, "", h)
            h = tolower(h)
            n = 0
            for (i = 1; i <= length(h); i++) {
                c = substr(h, i, 1)
                d = index("0123456789abcdef", c) - 1
                if (d < 0) return -1
                n = n * 16 + d
            }
            return n
        }
        $1 == "LOAD" {
            poff = h2d($2)
            pvad = h2d($3)
            pfsz = h2d($5)
            v = h2d(vh)
            if (v >= pvad && v < pvad + pfsz) {
                printf "0x%x\n", (v - pvad + poff)
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    '
}

Q_OFF=""
for sym in \
    "_ZN7android7Surface16hook_queueBufferEP13ANativeWindowP19ANativeWindowBufferi" \
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi" \
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE" \
    "_ZN7android7Surface15queueBufferImplERKNS_2spINS_13GraphicBufferEEERKNS1_INS_5FenceEEEPKNS_23SurfaceQueueBufferInputEPNS_24SurfaceQueueBufferOutputE"; do
    off=$(resolve_sym_offset "$sym")
    if [ -n "$off" ]; then
        Q_OFF="$off"
        break
    fi
done

DQ_OFF="-"
for sym in \
    "_ZN7android7Surface18hook_dequeueBufferEP13ANativeWindowPP19ANativeWindowBufferPi" \
    "_ZN7android7Surface13dequeueBufferEPNS_2spINS_13GraphicBufferEEEPi" \
    "_ZN7android7Surface13dequeueBufferEPP19ANativeWindowBufferPi"; do
    off=$(resolve_sym_offset "$sym")
    if [ -n "$off" ]; then
        DQ_OFF="$off"
        break
    fi
done

[ -n "$Q_OFF" ] || exit 1
echo "$LIB $Q_OFF $DQ_OFF" > "$KFAS_PROBES"
echo "kfas: configured probes -> $LIB queue=$Q_OFF dequeue=$DQ_OFF"
