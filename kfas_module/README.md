# KFAS Controller (`kfas_ctrl` · KernelSU Module)

配套本仓库内置内核驱动 `drivers/kfas/`（Kernel Frame-Aware Scheduler）的独立用户态游戏控制模块，不参与内核编译，**不接管 Scene、不改动系统分区挂载、不修改 PowerHAL 或全局日用 WALT/core_ctl/cgroup 参数**。

## 设计边界
- **游戏与日用严格隔离**：默认仅对 `/sdcard/Android/kfas/games.conf` 名单内的游戏启用帧感知控帧与游戏线程精准放核；非游戏日常应用 100% 交还原生系统与 WALT 调度（`daily_enable` 默认关闭）。
- **独立控制**：档位、名单与开关全部由内置 KernelSU WebUI 直接管理，不依赖或桥接 Scene `powercfg`。

## 功能组成
- **`kfas_watcher.sh`**：基于单条 `logcat -b events` 流的前台事件哨兵（零轮询），按游戏名单自动挂载/卸载 KFAS 帧感知会话，息屏自动释放。
- **`kfas_resolve_probes.sh`**：开机动态解析本机 `/system/lib64/libgui.so` 的 `Surface::hook_queueBuffer` 与 `Surface::hook_dequeueBuffer` ELF 偏移并写入 `/sys/kernel/kfas/probes`，系统升级无需重编内核。
- **`kfas_ctl.sh` & `webroot/`**：KernelSU Manager 原生三栏极简 WebUI 控制面板（`状态` · `游戏` · `设置`），支持实时帧率/瓶颈/三簇能效拐点/L3 & DDR DCVS/线程落核表观测、游戏名单管理及分档热调持久化。

## 打包与刷入
直接将本目录内全部文件打包为 zip（根目录包含 `module.prop`），在 KernelSU Manager 中安装并重启即可生效。
