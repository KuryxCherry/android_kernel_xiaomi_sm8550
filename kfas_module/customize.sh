#!/system/bin/sh
ui_print "- Installing KFAS Controller v1.2.2 (Game-Only Frame-Aware Controller + WebUI)"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/kfas_ctl.sh" 0 0 0755
set_perm "$MODPATH/kfas_resolve_probes.sh" 0 0 0755
set_perm "$MODPATH/kfas_watcher.sh" 0 0 0755
set_perm "$MODPATH/vtools/powercfg.sh" 0 0 0755
if [ -d /sys/kernel/kfas ]; then
    ui_print "- Detected KFAS kernel interface (/sys/kernel/kfas)"
else
    ui_print "! Note: Flash a KFAS-enabled kernel for /sys/kernel/kfas to activate"
fi
