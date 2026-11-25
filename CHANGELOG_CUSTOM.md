# Custom Kernel Changelog (`android_kernel_xiaomi_sm8550`)

> **文档说明**
> - 本文件专门用于记录本仓库（`lineage-24.0` 分支）相对 LineageOS 上游的全部自定义内核优化、驱动修复、配置改动与 CI 构建架构变更，便于日后排错、对比和回退。
> - **对编译零影响**：纯 Markdown 文档，不参与 Kbuild 编译，不影响内核镜像、`ccache` 命中率与后续上游合并（`sync_upstream.yml`），也不会触发任何自动构建。
> - **维护约定**：后续每次向本仓库提交优化或配置变更时，请在下方「详细变更日志」顶部按时间倒序同步追加记录（注明日期、Commit SHA、改动文件、目的与回退方法）。

---

## 一、当前生效定制特性速查

- **目标主力机型**：Redmi K60 Pro (`socrates` / SM8550)，兼容 `fuxi` / `nuwa` / `vermeer`
- **内核基线**：`Linux 5.15.225`（LineageOS 24.0 + `android13-5.15-lts`）
- **工具链与安全特性**：AOSP Clang 22.0.2 (`r596125`) + `ThinLTO` + 完整保留 `CONFIG_CFI_CLANG` / `CONFIG_DEBUG_INFO_BTF`（无缝兼容 YukiSU / KernelSU LKM）
- **打包方式**：三合一 AnyKernel3（同步刷入 `vendor_dlkm` EROFS + `boot` Image + `vendor_boot` ramdisk 模块，**不触碰 `init_boot`**，刷内核不掉 LKM Root）

| 分类 | 已启用定制项 | 核心入口 / 配置 |
| :--- | :--- | :--- |
| **网络** | TCP **BBRv3** + 默认 **FQ** qdisc；TTL/HL 修改支持 | `CONFIG_TCP_CONG_BBR3=y`、`CONFIG_DEFAULT_TCP_CONG="bbr3"`、`CONFIG_DEFAULT_NET_SCH="fq"`、`CONFIG_IP_NF_TARGET_TTL=y` |
| **内存与 ZRAM** | **MGLRU** 默认启用且 `min_ttl_ms=1000`；**LZ4 1.10.0 + ARM64 NEON** 汇编加速；默认 ZRAM 压缩 `lz4`；**ZRAM Writeback** 与内存追踪接口（默认不挂载 `backing_dev`，按需启用） | `CONFIG_LRU_GEN_ENABLED=y`、`mm/vmscan.c`、`lib/lz4/`、`CONFIG_ZRAM_DEF_COMP="lz4"`、`CONFIG_ZRAM_WRITEBACK=y`、`CONFIG_ZRAM_MEMORY_TRACKING=y` |
| **省电与唤醒** | **RCU_LAZY** 默认启用；**省电工作队列**默认开启；唤醒/定时/PCI PME 省电微调；静默 AVC audit 与 Binder 调试日志 | `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set`、`CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`、`security/lsm_audit.c`、`drivers/android/binder.c` |
| **存储与文件系统** | 块设备队列 `read_ahead_kb` 默认上限 **128 KB**（防 `post_boot` 拉高至 512 KB）；内置 **Samsung SSG**（含 cgroup 前后台浅队列限流适配）与 **ADIOS v3.3.0** I/O 调度器（与默认 `bfq` 共存，可热切换）；F2FS 拥塞停顿缓解、增大最小 fsync 块数、缩短紧急 GC 休眠；NTFS3 读写支持 | `block/blk-sysfs.c`、`block/ssg-*.{c,h}`、`CONFIG_MQ_IOSCHED_SSG(_CGROUP)=y`、`block/adios.c`、`CONFIG_MQ_IOSCHED_ADIOS=y`、`fs/f2fs/`、`CONFIG_NTFS3_FS=y` |
| **调度（Melt 极简路线）** | 保持高通原生 `sched_walt` EAS 主干，仅合入纯主线精准修复：去除 `do_idle()` 多余 `rmb()`、跳过 `rto_next_cpu()` 空推送、`migrate_misfit` 不污染均衡间隔、`_nohz_idle_balance` 优先小核升序扫描 | `kernel/sched/idle.c`、`kernel/sched/rt.c`、`kernel/sched/fair.c` |
| **进程冻结（墓碑）** | 内建 **Re-Kernel v11.7**（Generic Netlink） | `CONFIG_REKERNEL=y` |
| **架构与指令微调** | ARM64 `clear_page` 16 字节对齐、`copy_template` 流式预取、`list_lru` 无锁快路径 | `arch/arm64/lib/`、`mm/list_lru.c` |

---

## 二、详细变更日志（按时间倒序）

### 2026-11-25

- **`1c3905a812`** — `block: port Samsung SSG I/O scheduler with cgroup support for Linux 5.15`
  - **改动文件**：`block/ssg-iosched.c`、`block/ssg-cgroup.c`、`block/ssg-cgroup.h`、`block/Kconfig.iosched`、`block/Makefile`、`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：
    - 移植三星 S23 Ultra（SM8550 / `S918BXXU3CXCF`）同源 **SSG**（SamSung Generic）I/O 调度器及 `ssg-cgroup` 模块至 Linux `5.15.225`（零 `include/linux/` 结构体改动，完整保持 GKI KMI/CFI/BTF 兼容）；
    - 适配 Linux 5.15 的 3 参数 `blk_mq_sched_try_insert_merge(..., &free)` + `blk_mq_free_requests(&free)` 与 `trace_block_rq_insert()`；
    - 针对 LineageOS 原生 `/dev/blkio` 结构优化 `ssg_blkcg_cpd_init()`：根组（前台与系统核心）默认 `max_available_ratio=100` 不限流，子组（`/dev/blkio/background` 后台任务）默认 `25` 浅队列限流，无需任何用户态模块即可开箱生效；
    - 内置启用 `CONFIG_MQ_IOSCHED_SSG=y` 与 `CONFIG_MQ_IOSCHED_SSG_CGROUP=y`，默认调度器仍保持 `bfq`，支持随时热切换对比。
  - **运行时控制 / 回退**：`echo ssg > /sys/block/sda/queue/scheduler` 即时切换（可在 `/dev/blkio/background/blkio.ssg.max_available_ratio` 查看/调整后台队列占比）；源码回退直接 `git revert 1c3905a812`。

- **`4b54adb8d8` / `7377105e85`** — `block: backport ADIOS v3.3.0 I/O scheduler for Linux 5.15`
  - **改动文件**：`block/adios.c`、`block/Kconfig.iosched`、`block/Makefile`、`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：
    - 借鉴 Melt Kernel 引入独立单文件 **ADIOS v3.3.0**（Adaptive Deadline I/O Scheduler，Masahito Suzuki），零结构体/KMI 改动，并针对 Linux 5.15 适配 `blk_mq_sched_try_insert_merge(..., &free)` + `blk_mq_free_requests(&free)` 与 `-std=gnu11` 局部编译参数；
    - 以内置方式启用 `CONFIG_MQ_IOSCHED_ADIOS=y`，保持默认 `bfq` 不变，支持随时热切换测试。
  - **运行时控制 / 回退**：`echo adios > /sys/block/sda/queue/scheduler` 可即时切换为 ADIOS（写 `bfq` 秒切回原厂）；源码回退直接 `git revert 4b54adb8d8`。

- **`16841ec045`** — `sched: backport mainline idle/rt/misfit and asym-idle balance fixes`
  - **改动文件**：`kernel/sched/idle.c`、`kernel/sched/rt.c`、`kernel/sched/fair.c`
  - **内容**：
    - 遵循 Melt Kernel 主干克制原则（完整保留高通原生 `sched_walt` EAS，不引入破坏 ABI 的 BORE/CASS/Uclamp Assist 魔改），仅合入 4 项纯上游精准修复：
      1. `idle.c`：移除 `do_idle()` 循环内多余 `rmb()` 读内存屏障（高通 Zhongqiu Han / Peter Zijlstra 主线提交）；
      2. `rt.c`：`rto_next_cpu()` 跳过无任务可推的 CPU，减少无效 RT 核间中断（Sebastian Siewior 主线提交）；
      3. `fair.c`：`migrate_misfit` 迁移失败时不污染 `nr_balance_failed` 也不翻倍拉长 `balance_interval`（Qais Yousef / Ingo Molnar 主线提交）；
      4. `fair.c`：`_nohz_idle_balance()` 改按 CPU 编号升序扫描空闲核，优先让小核承接轻负载均衡。
  - **回退**：直接 `git revert 16841ec045`。

- **`71349d892e`** — `mm/block: set default MGLRU min_ttl_ms to 1000 and cap queue read_ahead_kb to 128`
  - **改动文件**：`mm/vmscan.c`、`block/blk-sysfs.c`
  - **内容**：
    - `mm/vmscan.c`：将 MGLRU 工作集保护时间 `lru_gen_min_ttl` 默认初始化为 `HZ`（`1000 ms`，Google 官方推荐防内存抖动默认值），避免瞬时高压下的页缓存 thrashing；
    - `block/blk-sysfs.c`：在 `queue_ra_store()` 新增可热调模块参数 `max_read_ahead_kb`（默认 `128`，`0` 表示不限制），防止高通 `init.kernel.post_boot.sh` 开机后将 UFS/dm 设备 `read_ahead_kb` 覆盖拉高到 `512 KB`，减少 UFS 4.0 随机小读的预读放大与 Page Cache 浪费。
  - **运行时控制 / 回退**：
    - `echo 0 > /sys/kernel/mm/lru_gen/min_ttl_ms` 可即时关闭 MGLRU 最小保护时间；
    - `echo 0 > /sys/module/blk_sysfs/parameters/max_read_ahead_kb` 可即时解除预读上限；
    - 源码回退直接 `git revert 71349d892e`。

- **`b6ae314955`** — `arm64: configs: socrates: enable WQ_POWER_EFFICIENT_DEFAULT and ZRAM writeback`
  - **改动文件**：`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：
    - 开启 `CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`（让内核工作队列优先派发到活跃 CPU 核，减少唤醒休眠核）；
    - 开启 `CONFIG_ZRAM_WRITEBACK=y` 与 `CONFIG_ZRAM_MEMORY_TRACKING=y`（预留 ZRAM 回写与块追踪能力，不配置 `backing_dev` 时零额外写入与功耗）。
  - **运行时控制 / 回退**：`echo N > /sys/module/workqueue/parameters/power_efficient` 可即时关闭省电工作队列；回退直接 `git revert b6ae314955`。

- **`898d43cf65` / `57a323239e`** — `ci: fix ccache path, deduplicate dist artifacts, and optimize runner setup`
  - **改动文件**：`.github/workflows/build_full_kernel.yml`、`.github/workflows/build_kernel.yml`
  - **内容**：
    - 修复 Ubuntu 24.04 (`ccache 4.9`) 默认缓存路径为 `~/.cache/ccache` 与此前缓存 `~/.ccache` 不一致导致缓存失效的问题；开启 `CCACHE_COMPRESS=1` (level 6)、`CCACHE_SLOPPINESS=time_macros,file_macro`、`CCACHE_NOHASHDIR=1`，缓存 key 补齐 `lto_mode`；
    - 消除 `dist/` 产物套娃：默认仅上传 `AnyKernel3-full-*.zip` + `defconfig_used` + `module_report.txt` + `SHA256SUMS.txt`（体积由 ~91 MB 降至 ~50 MB），新增 `export_raw_images` 可选开关；`upload-artifact` 设 `compression-level: 0`；
    - Runner 磁盘改为可用空间 `< 35 GB` 时按需清理（平时 0 秒跳过，省 ~90 秒）；
    - 消除打包期 `depmod` 警告，并在 `$GITHUB_STEP_SUMMARY` 自动生成构建汇总表。

- **`0a4a5ba1aa`** — `ci: fix AnyKernel3 vendor_dlkm flashing for Xiaomi SM8550 super layout`
  - **改动文件**：`.github/workflows/build_full_kernel.yml`
  - **内容**：修复 SM8550 动态分区 `super` 下 `vendor_dlkm` 刷入逻辑，当新镜像小于等于现有分区大小时采用原位 `dd` 写入 + SHA256 逐字节校验，避免 `lptools` 无谓 resize/unmap 引发报错。

- **`f855a05e6c`** — `arm64: configs: socrates: set LZ4 as default ZRAM compression algorithm`
  - **改动文件**：`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：将默认 ZRAM 压缩算法设为 `lz4`（配合已合入的 LZ4 1.10.0 + ARM64 NEON 加速，降低日常换页 CPU 消耗）。

### 2026-11-24

- **`fe4c103e68`** — `drivers: rekernel: integrate built-in Re-Kernel v11.7`
  - **改动文件**：`drivers/rekernel/`、`arch/arm64/configs/vendor/socrates_GKI.config` 等
  - **内容**：源码内建 Re-Kernel v11.7（`CONFIG_REKERNEL=y`），用于配合 Freezer 等墓碑模块实现内核级网络/Binder 冻结解冻通信。

- **`c9ef802c43`** — `ci: disable automatic push trigger on legacy Image-only build workflow`
  - **改动文件**：`.github/workflows/build_kernel.yml`
  - **内容**：关闭旧版单刷 `Image` 工作流的自动 push 触发，改为纯手动 `workflow_dispatch`。

- **`9f4c1cb614`** — `net: usb: ax88179_178a: restore vendor ASIX driver compatible with ax_main`
  - **改动文件**：`drivers/net/usb/ax88179_178a.c`
  - **内容**：修复合并上游 `5.15-lts` 后 `ax88179_178a` 与高通/厂商 `ax_main` 驱动符号不兼容问题。

- **`a9f9040f4f`** — `Merge remote-tracking branch 'upstream/android13-5.15-lts' into lineage-24.0`
  - **内容**：合并 Google Android Common Kernel `android13-5.15-lts` 上游安全与稳定性补丁至 `5.15.225`。

- **`15bd99870d` / `135e36be94` / `4242ee0558` / `4eb300f9bf`** — 三合一完整内核构建与上游同步工作流
  - **改动文件**：`.github/workflows/build_full_kernel.yml`、`.github/workflows/sync_upstream.yml`
  - **内容**：建立完整内核 + 24 套高通/NXP 外置模块编译、EROFS `vendor_dlkm.img` 生成、三合一 AnyKernel3 打包与上游安全合并流水线。

- **`d3d7d6a5eb`** — `arm64: socrates: enable RCU_LAZY by default and apply mm/f2fs micro-optimizations`
  - **改动文件**：`arch/arm64/configs/vendor/socrates_GKI.config`、`mm/list_lru.c`、`fs/f2fs/gc.h`
  - **内容**：默认开启 `RCU_LAZY`（禁用 `RCU_LAZY_DEFAULT_OFF`）；`list_lru_count_one()` 快路径跳过多余 RCU 读锁；F2FS 紧急 GC 休眠由 500ms 缩至 50ms。

- **`49e2ddf0d9`** — `kernel: silence AVC audit and binder debug logs by default (Melt style)`
  - **改动文件**：`security/lsm_audit.c`、`drivers/android/binder.c`
  - **内容**：默认静默 SELinux AVC audit 刷屏日志（保留 `disable_audit_log` 运行时模块参数可随时恢复）及 Binder debug 日志。

- **`4678a3a783`** — `kernel: backport safe ARM64 micro-opts and power/wakeup cleanups`
  - **改动文件**：`arch/arm64/lib/clear_page.S`、`arch/arm64/lib/copy_template.S`、`drivers/base/power/wakeup.c`、`kernel/time/alarmtimer.c`、`drivers/pci/pci.c`、`kernel/irq/cpuhotplug.c`
  - **内容**：ARM64 `clear_page` 16B 对齐、`copy_template` 流式预取；消除冗余 `s2idle_wake()` 调用；最小化 `alarmtimer` 唤醒超时；PCI PME 轮询间隔由 1s 放宽至 4s；降级 IRQ affinity 日志级别。

- **`47fa49c155` / `f639f9a721`** — `arm64: configs: socrates: set default FQ qdisc for BBRv3 and clean up config`
  - **改动文件**：`arch/arm64/configs/vendor/socrates_GKI.config`、`.github/workflows/build_kernel.yml`
  - **内容**：配合 BBRv3 将默认网络队列调度器设为 `fq`；修正 GKI AnyKernel3 打包与机型校验。

- **`01ab2815e3`** — `lib/lz4: upgrade to LZ4 1.10.0 with ARM64 NEON acceleration`
  - **改动文件**：`lib/lz4/`、`include/linux/lz4.h` 等
  - **内容**：从 `N1-k0-la1/AnyBaseKernel` 合并升级内核 LZ4 压缩库至 `1.10.0` 并合入 ARM64 NEON 汇编加速（保留原有 KMI 导出符号）。

- **`5ccf5ad45e` / `73d35f405f` / `54c1ff8226` / `6185ce2721`** — TCP BBRv3 与 F2FS 基础优化
  - **改动文件**：`net/ipv4/`、`fs/f2fs/`、`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：移植并默认启用 `TCP BBRv3`；合入 F2FS 缓解拥塞停顿与增大最小 `fsync` 块数补丁。

---

## 三、已评估但主动排除（未合入）的项（防重复踩坑）

1. **更换第三方魔改 Clang 工具链**：放弃。当前 ROM（LineageOS 24.0）与 `sm8550-modules` 及 YukiSU LKM（依赖 `vmlinux.btf` 与 `clang-cfi/.cfi_jt`）均针对 AOSP Clang 22.0.2 (`r596125`) 验证，换工具链易导致 CFI/BTF 不兼容不开机。
2. **源码内置 KernelSU/YukiSU**：放弃。保持主内核纯净，使用 `init_boot` LKM 方案实现内核与 Root 解耦，换内核不掉 Root 且便于上游 LTS 合并。
3. **高侵入内存回收魔改（如 `kshrink_slabd` / `kshrink_lruvecd` / `mi_rmap`）与 HyperOS 专属模块（`mi_sw_sync` / `unionpower` / `sew_audit`）**：放弃。侵入 `mm/` 核心代码极易引起上游合并冲突与记账/竞态问题，且 LineageOS 24.0 环境下原厂 MGLRU 已足够稳定。
4. **实验性调度大改（`launch_boost` / `sew_yield_penalty` / 强切 RT 的 Binder 调度 / 第三方覆盖式 sysfs 调度脚本）**：放弃。12 GB RAM + 骁龙 8 Gen 2 下原生 `vendor.power-hal-aidl` + 高通 `sched_walt` 已具备良好表现（原厂已默认 `core_ctl=0`、`page-cluster=0`、`irq_affinity=07`、`bfq`）。

---

## 四、排错与快速回退速查

- **按单项精确回退**：在 Git 仓库中直接执行 `git revert <Commit SHA>` 即可干净撤销对应特性。
- **常见运行时验证命令（手机 Root 终端）**：
  ```sh
  uname -a                                               # 核对内核版本与提交号
  cat /proc/sys/net/ipv4/tcp_congestion_control          # 应为 bbr3
  cat /proc/sys/net/core/default_qdisc                   # 应为 fq
  cat /sys/kernel/mm/lru_gen/enabled                     # 应为 0x0003
  cat /sys/kernel/mm/lru_gen/min_ttl_ms                  # 应为 1000
  cat /sys/block/sda/queue/read_ahead_kb                 # 应为 128
  cat /sys/block/sda/queue/scheduler                     # 应包含 ssg 与 adios（默认仍为 [bfq]，可热切）
  cat /sys/module/blk_sysfs/parameters/max_read_ahead_kb # 应为 128（设为 0 解除上限）
  cat /sys/module/workqueue/parameters/power_efficient   # 应为 Y
  cat /sys/block/zram0/comp_algorithm                    # 应包含 [lz4]
  ls /sys/block/zram0/backing_dev                        # 应存在（ZRAM Writeback 接口）
  ```
