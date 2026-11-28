# KFAS Controller (`kfas_ctrl` · KernelSU Module)

配套本仓库内置内核驱动 `drivers/kfas/`（Kernel Frame-Aware Scheduler）的用户态控制模块，不参与内核编译。

## 功能组成
- **`kfas_watcher.sh`**：基于单条 `logcat -b events` 流的前台事件哨兵（零轮询），按游戏名单自动挂载/卸载 KFAS 帧感知会话，非游戏前台自动交由 KFAS 日常线程辅助引擎处理（排除银行/支付/钱包与关键系统包），息屏自动释放。
- **`kfas_resolve_probes.sh`**：开机动态解析本机 `/system/lib64/libgui.so` 的 `Surface::hook_queueBuffer` 与 `Surface::hook_dequeueBuffer` ELF 偏移并写入 `/sys/kernel/kfas/probes`，系统升级无需重编内核。
- **`vtools/powercfg.{sh,json}`**：与 Scene 档位同步桥接（`powersave` / `balance` / `performance` / `fast`）。
- **`kfas_ctl.sh` & `webroot/`**：KernelSU Manager 原生 WebUI 控制面板，支持实时帧率/瓶颈/三簇能效拐点/L3 & DDR DCVS/线程落核表观测、游戏名单管理及分档热调持久化。
- **`powerhint.json`**：针对 SM8550 LineageOS PowerHAL 的无损能效调优配置，收紧 `LAUNCH` 与 `INTERACTION` 盲目全簇顶频与 99ms 全核滞回（不限制最高频，重载仍由 WALT 即时拉升）；开机由 `service.sh` 瞬时加载进 PowerHAL 内存后立即卸载，零常驻挂载残留。

## 打包与刷入
直接将本目录内全部文件打包为 zip（根目录包含 `module.prop`），在 KernelSU Manager 中安装并重启即可生效。
