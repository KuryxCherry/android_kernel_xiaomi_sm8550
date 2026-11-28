# Custom Kernel Changelog (`android_kernel_xiaomi_sm8550`)

> **文档说明**
> - 本文件记录主内核仓库（`KuryxCherry/android_kernel_xiaomi_sm8550`，`lineage-24.0` 分支）相对 LineageOS 上游的全部自定义内核优化、驱动修复、配置改动与 CI 架构变更，配套外置驱动仓库见 [`KuryxCherry/android_kernel_xiaomi_sm8550-modules`](https://github.com/KuryxCherry/android_kernel_xiaomi_sm8550-modules)（见其 `CHANGELOG_MODULES.md`）。
> - **对编译零影响**：纯 Markdown 文档，不参与 Kbuild 编译，不影响内核镜像、`ccache` 命中率与后续上游合并（`sync_upstream.yml`），也不会触发任何自动构建。
> - **维护约定**：后续每次向本仓库提交优化或配置变更时，请在下方「详细变更日志」顶部按时间倒序同步追加记录（注明日期、Commit SHA、改动文件、目的与回退方法）。

---

## 一、当前生效定制特性速查

- **目标机型**：Redmi K60 Pro (`socrates`) 为主力，同时通过共享配置片段 `vendor/sm8550_custom_GKI.config` 一视同仁支持 `fuxi` / `nuwa` / `vermeer`（四款机型原生 `*_GKI.config` 均保持 100% 纯净 LineageOS 上游，合并零冲突）
- **内核基线**：`Linux 5.15.225`（LineageOS 24.0 + `android13-5.15-lts`）
- **工具链与安全特性**：AOSP Clang 22.0.2 (`r596125`) + `ThinLTO` + 完整保留 `CONFIG_CFI_CLANG` / `CONFIG_DEBUG_INFO_BTF`（无缝兼容 YukiSU / KernelSU LKM）
- **CI 构建架构**：唯一规范构建入口 `.github/workflows/build_full_kernel.yml` + 独立脚本 `.github/scripts/{stage_modules.py,pack_anykernel.sh}` + 上游同步 `.github/workflows/sync_upstream.yml`
- **打包方式**：三合一 AnyKernel3（按安全顺序同步刷入 `vendor_dlkm` EROFS + `boot` Image + `vendor_boot` ramdisk 模块，**不触碰 `init_boot`**，刷内核不掉 LKM Root）

| 分类 | 已启用定制项 | 核心入口 / 配置 |
| :--- | :--- | :--- |
| **网络** | TCP **BBRv3** + 默认 **FQ** qdisc；TTL/HL 修改支持 | `vendor/sm8550_custom_GKI.config`（`CONFIG_TCP_CONG_BBR3=y`、`CONFIG_DEFAULT_TCP_CONG="bbr3"`、`CONFIG_DEFAULT_NET_SCH="fq"`、`CONFIG_IP_NF_TARGET_TTL=y`） |
| **内存与 ZRAM** | **MGLRU** 默认启用且 `min_ttl_ms=1000`；移植上游 **Linux 6.6 LTS `zsmalloc`**（12 级 fullness 分组、可配置页链 `CONFIG_ZSMALLOC_CHAIN_SIZE=8`、简化锁模型与 `local_lock_t` 每核映射锁）；纯上游 **LZ4 v1.11.1**（已移除树外 `lz4armv8` NEON，原地解压自动路由 `safe_decode`，兼顾 ZRAM 高速与 EROFS 零报错）；默认 ZRAM 压缩 `lz4`；**ZRAM Writeback** 与内存追踪接口（默认不挂载 `backing_dev`，按需启用） | `CONFIG_LRU_GEN_ENABLED=y`、`mm/vmscan.c`、`mm/zsmalloc.c`、`CONFIG_ZSMALLOC_CHAIN_SIZE=8`、`lib/lz4/`、`CONFIG_ZRAM_DEF_COMP="lz4"`、`CONFIG_ZRAM_WRITEBACK=y`、`CONFIG_ZRAM_MEMORY_TRACKING=y` |
| **省电与唤醒** | **RCU_LAZY** 默认启用；**省电工作队列**默认开启；唤醒/定时/PCI PME 省电微调；静默 AVC audit 与 Binder 调试日志 | `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set`、`CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`、`security/lsm_audit.c`、`drivers/android/binder.c` |
| **存储与文件系统** | Kconfig 无损直接指定默认 I/O 调度器（`CONFIG_DEFAULT_IOSCHED="ssg"`）与块预读（`CONFIG_DEFAULT_READ_AHEAD_KB=128`，仅规范开机 `init`/`vendor_init` 非零写入，开机后完全自由调整）；内置 **Samsung SSG**（含 cgroup 前后台浅队列限流适配与完整 RCU/refcount 保护）与 **ADIOS v3.3.0** I/O 调度器（默认开机启用 **SSG**，与 `bfq` / `adios` 共存且随时可热切换）；F2FS 拥塞停顿缓解、增大最小 fsync 块数、缩短紧急 GC 休眠；NTFS3 读写支持 | `block/blk.h`、`block/elevator.c`、`block/blk-sysfs.c`、`block/ssg-*.{c,h}`、`block/adios.c`、`fs/f2fs/`、`CONFIG_NTFS3_FS=y` |
| **调度优化** | 保持高通原生 `sched_walt` EAS 主干；合入 4 项 Linux 主线精准修复（去除 `do_idle()` 多余 `rmb()`、跳过 `rto_next_cpu()` 空推送、`migrate_misfit` 不污染均衡间隔、`_nohz_idle_balance` 升序小核优先）；移植高通上游 WALT 修复与增强（`scaling_min/max_freq` 立即生效、MVP `mutex` 锁队列插队、MVP 防饿死节流、跨核迁移无锁读 cycle counter 修复、`ttwu_pending` 跳过 `newidle` 扫描、`nohz` 定时器限定 active CPU、pipeline 负载均衡防碰撞与 RCU 修复、`uclamp` 频导回调）；扩展同步 Binder `WALT_BINDER_MVP` 低延迟继承覆盖 AOSP/LineageOS 关键 UI 渲染链路 | `kernel/sched/{idle,rt,fair}.c`、`kernel/sched/walt/{cpufreq_walt,mvp_locking,walt,walt_cfs,walt_lb,walt_halt,core_ctl}.c`、`kernel/sched/walt/walt.h` |
| **进程冻结（墓碑）** | 内建 **Re-Kernel v11.7**（Generic Netlink） | `drivers/rekernel/`、`CONFIG_REKERNEL=y` |
| **架构与指令微调** | ARM64 `clear_page` 16 字节对齐、`copy_template` 流式预取、`list_lru` 无锁快路径 | `arch/arm64/lib/`、`mm/list_lru.c` |

---

## 二、详细变更日志（按时间倒序）

### 2026-11-28

- **`c92a22bda8`** — `sched/walt: extend WALT_BINDER_MVP inheritance for sync UI IPC`（**可按需调整项**）
  - **改动文件**：`kernel/sched/walt/walt_cfs.c`
  - **内容**：针对 AOSP/LineageOS 中 `RenderThread`、`wmshell.*`、`ll.splashscreen` 与 `top-app` 高优 UI 线程运行在 `SCHED_NORMAL`（`nice -4..-10`）而非 `SCHED_FIFO` 导致原生 `walt_binder_low_latency_set()` 漏判同步 UI Binder IPC 的盲区，仅对同步事务（`sync == true`，不触碰 `oneway` 异步事务）扩展 `WALT_LOW_LATENCY_BINDER` 触发条件（覆盖任意 RT 线程本身及 `prio <= DEFAULT_PRIO - 4` 的 `top-app` RTG / 关键 UI 动画线程），复用高通原生 `WALT_BINDER_MVP`（自带 3ms `WALT_MVP_SLICE`、`task_util < 325` 与 500ms/100ms MVP 节流保护，不强改 `SCHED_FIFO`）；并在 `binder_restore_priority_hook()` 中当 Binder 线程结束全部事务回到空闲等待时精准清除借出的 `WALT_LOW_LATENCY_BINDER` 标签。
  - **回退**：`git revert c92a22bda8`。

- **`b2b9f26eb8`** — `sched/walt: uclamp based frequency guidance`（**已合入可选项，可单独回退/对比**）
  - **改动文件**：`kernel/sched/walt/walt.h`、`kernel/sched/walt/walt.c`
  - **上游来源**：Qualcomm Innovation Center（CLO WALT，原作者：Ashay Jaiswal `<quic_ashayj@quicinc.com>`，`Change-Id: I2535a4327cb602859f51808ae9a2583624f1b2ab`）
  - **内容**：在 `struct walt_rq` 中追踪每运行队列 `uclamp_limit[UCLAMP_CNT]`，当 `enqueue`/`dequeue` 导致队列 `uclamp_min` 或 `uclamp_max` 变化时立刻触发 `waltgov_run_callback(rq, WALT_CPUFREQ_UCLAMP)` 通知 `cpufreq_walt` 调频（仍受 `waltgov` 升降频限速约束），不必等待 16ms 窗口 rollover。若后续实测发现回调过密或功耗不理想，可直接单独回退本提交。
  - **回退**：`git revert b2b9f26eb8`。

- **`96c41e2a06`** — `sched/walt: fix RCU pd access and prevent pipeline task collisions`
  - **改动文件**：`kernel/sched/walt/walt.c`、`kernel/sched/walt/walt_cfs.c`、`kernel/sched/walt/walt_lb.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `9b17ab90d42e`、`0f8077d65503`、`ab41af2f79b1`）
  - **内容**：`walt_init()` 中检查 `rd->pd` 改用 `rcu_access_pointer()` 避免非 RCU 读锁上下文误用 `rcu_dereference()`；`PIPELINE_FASTPATH` 放行 `p == rq->curr`；主动负载均衡 `need_active_lb()` 避免将 pipeline 任务迁移到当前正在跑另一 pipeline 任务的目标核。
  - **回退**：`git revert 96c41e2a06`。

- **`9ea92c4a77`** — `sched/walt: always return active cpu as nohz timer target`
  - **改动文件**：`kernel/sched/walt/core_ctl.c`、`kernel/sched/walt/walt.h`、`kernel/sched/walt/walt_halt.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `949b34305580`、`53e81c1e703e`，原作者：Ashay Jaiswal `<quic_ashayj@quicinc.com>`）
  - **内容**：导出 `cpus_paused_by_us` 并在 `android_rvh_get_nohz_timer_target()` 中将未暂停核集合与 `cpu_active_mask` 求交集，杜绝将 nohz 定时器分派到 offline 核；在全活动核暂停时优先选择 `core_ctl` 暂停核而非外部冻结核。
  - **回退**：`git revert 9ea92c4a77`。

- **`5d9578f62e`** — `sched/walt: Skip newidle balance if a wakeup is pending on a cpu`
  - **改动文件**：`kernel/sched/walt/walt_lb.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `b8c74e807ad4`，并修正原提交 `done = 0` 指针笔误为 `*done = 0`）
  - **内容**：当 `this_rq->ttwu_pending` 为真（当前核已有待唤醒任务）时跳过跨核 `walt_newidle_balance()` 扫描，降低唤醒延迟与无效空转开销。
  - **回退**：`git revert 5d9578f62e`。

- **`36879c4901`** — `sched/walt: Fix updating of task cpu cycles without rq lock`
  - **改动文件**：`kernel/sched/walt/walt.c`
  - **上游来源**：Qualcomm Technologies（`LineageOS/android_kernel_qcom_sm8650` 提交 `1c7d1d3a6b60`）
  - **内容**：修复 `migrate_busy_time_subtraction()` 在未持有目标核 `rq->lock` 时调用 `update_task_cpu_cycles()` 并发修改目标核 `wrq->cycles` 与 `last_cc_update` 的竞态，改为在 `use_cycle_counter` 启用时直接调用 `qcom_cpufreq_get_cpu_cycle_counter(new_cpu)`。
  - **回退**：`git revert 36879c4901`。

- **`7da13eeff9`** — `sched/walt: add support for throttling MVP tasks`
  - **改动文件**：`kernel/sched/walt/walt.h`、`kernel/sched/walt/walt.c`、`kernel/sched/walt/walt_cfs.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `74ce9568e59f`，原作者：Ashay Jaiswal `<quic_ashayj@quicinc.com>`）
  - **内容**：为每运行队列引入 MVP 防饿死节流机制（单核连续 MVP 执行超 `500ms` 时暂缓 MVP 抢占 `100ms` 放行普通 CFS 任务），并在 `walt_cfs_replace_next_task_fair()` 补齐节流到期自动解除检查，避免空闲唤醒场景下节流状态残留。
  - **回退**：`git revert 7da13eeff9`。

- **`6083cc6cfb`** — `sched/walt: move MVP task at front of mutex waiter`
  - **改动文件**：`kernel/sched/walt/Makefile`、`kernel/sched/walt/mvp_locking.c`、`kernel/sched/walt/walt.h`、`kernel/sched/walt/walt.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `23f22d68b272`）
  - **内容**：挂载 `android_vh_alter_mutex_list_add` vendor hook，使 WALT MVP 任务（含 `WALT_BINDER_MVP` 与 `WALT_LL_PIPE_MVP`）在内核 `mutex` 阻塞等待队列中直接排到非 MVP 等待者之前，缓解锁竞争导致的优先级反转掉帧。
  - **回退**：`git revert 6083cc6cfb`。

- **`e13b58d6c9`** — `sched/walt: Honor scaling_min_freq/scaling_max_freq`
  - **改动文件**：`kernel/sched/walt/cpufreq_walt.c`
  - **上游来源**：Qualcomm Innovation Center（`LineageOS/android_kernel_qcom_sm8650` 提交 `c7920924ec33`，原作者：Shivnandan Kumar `<quic_shivnand@quicinc.com>`）
  - **内容**：拆分 `__waltgov_update_next_freq()`，使 `waltgov_limits()` 在 `scaling_min_freq`/`scaling_max_freq` 变化时绕过 `up/down_rate_delay_ns` 限速立即执行频率切换。
  - **回退**：`git revert e13b58d6c9`。

### 2026-11-26

- **`6efadb0182` / `64b404b583`** — `mm/zsmalloc: backport upstream Linux 6.6 LTS zsmalloc for 5.15`
  - **改动文件**：`mm/zsmalloc.c`、`include/linux/zsmalloc.h`、`mm/Kconfig`、`arch/arm64/configs/vendor/sm8550_custom_GKI.config`、`.github/workflows/build_full_kernel.yml`
  - **内容**：
    - 移植上游 **Linux 6.6 LTS（`linux-6.6.y @ 6183c6579356`）** `mm/zsmalloc.c` 与 `include/linux/zsmalloc.h`（上游主要作者：Sergey Senozhatsky、Minchan Kim、Nhat Pham、Harry Yoo）：
      - 引入可配置 `zspage` 页链长度 `CONFIG_ZSMALLOC_CHAIN_SIZE`（范围 `4..16`，默认 `8`，同步在共享片段 `sm8550_custom_GKI.config` 显式设为 `8`）及 `calculate_zspage_chain_size()`、`ISOLATED_BITS=5`；
      - 引入 12 级细粒度占用率分组（`ZS_INUSE_RATIO_0` ~ `ZS_INUSE_RATIO_100`）提升压缩页拼接密度与整理效率；
      - 同步 6.6 `pool->lock` + 每 `zspage->lock` 简化锁模型（移除旧句柄位自旋锁与迁移期 `zspage` 列表隔离）、`local_lock_t` 每核映射区锁、`zspage->huge` 位域、`compaction_in_progress` 并发整理防护及 `zs_lookup_class_index()` 导出，并保留 `!CONFIG_COMPACTION` 下清除 `__GFP_MOVABLE` 的修复；
    - 极简 5.15 兼容适配：保留 5.15 `zsmalloc_mnt` 伪文件系统 + 模块级共享 `zsmalloc_inode` 与 `address_space_operations` 替代 6.x `movable_operations`，适配单参 `register_shrinker()`、`CONFIG_ZPOOL` 四参 `zs_zpool_create()` 与 `migrate_write_lock_nested()` 的 `write_lock()` 回退（5.15 无 `write_lock_nested()`），保持全部既有 `zs_*` 导出符号与 `zram.ko` 100% 二进制接口兼容。
  - **回退**：`git revert 64b404b583 6efadb0182`。

- **`a52e82d93f`** — `ci: unify full kernel build workflow, modularize packaging scripts, and remove obsolete workflows`
  - **改动文件**：删除 `.github/workflows/apply_patches.yml` 与 `.github/workflows/build_kernel.yml`；新增 `.github/scripts/stage_modules.py` 与 `.github/scripts/pack_anykernel.sh`；重构 `.github/workflows/build_full_kernel.yml` 与 `.github/workflows/sync_upstream.yml`
  - **内容**：
    - 移除高危遗留脚本 `apply_patches.yml`（原检测 `lib/lz4/lz4armv8` 存在性，在升级纯上游 LZ4 v1.11.1 后若误触会覆盖破坏源码）与冗余的旧版单刷 `Image` 工作流 `build_kernel.yml`；
    - 将 800 行巨型 YAML 内嵌的 Python 模块分拣脚本与 AnyKernel3 打包脚本抽离至 `.github/scripts/`，构建前自动执行 `python3 -m py_compile` 与 `bash -n` 静态校验；
    - 补齐 `vermeer`（Redmi K70）在 `vendor_dlkm` 中缺失的 `aw882xx_dlkm.ko` 功放驱动，四款机型模块清单与 LineageOS 24.0 `BoardConfig.mk` 100% 对齐，并对 `recovery` 模块缺失同样严格报错拦截；
    - `build_full_kernel.yml` 串联加载 `vendor/sm8550_custom_GKI.config`，校验 `default_iosched` 对应的 Kconfig 确已内置，并在 Step Summary 与 Release 正文补全调度器、预读值及外置驱动仓信息；
    - `sync_upstream.yml` 补充 `concurrency` 并发锁、安全数组解析 `prefer_theirs_paths` 与 `$GITHUB_STEP_SUMMARY` 报告。
  - **回退**：`git revert a52e82d93f`。

- **`ca421f6401`** — `arm64: configs: extract shared sm8550_custom_GKI.config and keep socrates_GKI.config upstream-clean`
  - **改动文件**：新增 `arch/arm64/configs/vendor/sm8550_custom_GKI.config`；还原 `arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：将此前全部堆在 `socrates_GKI.config` 中的定制配置抽离至跨机型共享片段 `sm8550_custom_GKI.config`，使 `fuxi` / `nuwa` / `vermeer` 构建同样完整生效且 `socrates_GKI.config` 恢复为 100% 纯净 LineageOS 上游版本，避免日后合并冲突。
  - **回退**：`git revert ca421f6401`。

- **`ddc396dcb5`** — `block/rekernel: fix SSG and Re-Kernel RCU UAFs, harden default iosched/readahead, and tidy Kconfig`
  - **改动文件**：`block/blk.h`、`block/Kconfig`、`block/elevator.c`、`block/blk-sysfs.c`、`block/ssg-cgroup.h`、`block/ssg-cgroup.c`、`block/ssg-iosched.c`、`drivers/rekernel/rekernel_binder.c`、`include/linux/lz4.h`
  - **内容**：
    - 修复 `block/ssg-cgroup.c` 中 `ssg_blkcg_shallow_depth()` 在 `rcu_read_unlock()` 后解引用 `ssg_blkg` 的 RCU UAF 竞态，并将 `rqi->blkg` 的查找、引用计数（`blkg_tryget` / `blkg_put`）与增减计数完整封装进 `ssg-cgroup.c`，消除异步请求完成路径的 `blkg` UAF 隐患；同时补齐 `map_nr` / `root_blkg` / `rq_info_nr` 边界防御并将 `ssg_blkg_files` 设为 `static`；
    - 修复 `drivers/rekernel/rekernel_binder.c` 中 `line_binder_alloc_new_buf_locked()` 在 `rcu_read_unlock()` 后访问 `find_task_by_vpid()` 返回的 `task_struct` 的 RCU UAF 隐患；
    - 消除 `block/elevator.c` 与 `block/blk-sysfs.c` 中重复的 `is_init_or_init_child()`，统一抽至 `block/blk.h` 的 `blk_is_init_caller()`（使用内核标准 `is_global_init()`）；
    - 将 `block/elevator.c` 的 `default_iosched` 参数由 `charp` 改为固定缓冲区 `module_param_string` + `strim()` 去换行，消除运行时改写的并发 `kfree` UAF 与尾部换行匹配失败问题，顺手清除空 `#ifdef` 残留；
    - `block/blk-sysfs.c` 增加 `#ifdef CONFIG_DEFAULT_READ_AHEAD_KB` 保护及 `ra_kb > 0` 判定，避免 init 显式写 `0` 关闭预读（如 ZRAM）时被误拉高到 `128 KB`；
    - 将 `config DEFAULT_READ_AHEAD_KB` 移入 `block/Kconfig` 的 `if BLOCK` 块内，并为 `include/linux/lz4.h` 补齐文件末尾换行符。
  - **回退**：`git revert ddc396dcb5`。

### 2026-11-25

- **`1f96441463`** — `lib/lz4: route in-place decompression to safe_decode and disable LZ4_WILDCOPY64`
  - **改动文件**：`lib/lz4/lz4.c`、`lib/lz4/Makefile`
  - **内容**：修复上游 LZ4 `v1.11.1` 在 `aarch64` 默认启用 `LZ4_WILDCOPY64=1`（64B 快拷越界）超出 EROFS 48B 原地解压安全边距、导致 `/vendor` `/odm` 报 `z_erofs_lz4_decompress_mem: failed to decompress -3` 的问题；在 `LZ4_decompress_generic()` 中将重叠原地解压路由至 `safe_decode`（`8B wildcopy + memmove`）并设 `-DLZ4_WILDCOPY64=0`，同时保持 ZRAM/F2FS 满速 `LZ4_FAST_DEC_LOOP=1`。
  - **回退**：`git revert 1f96441463`。

- **`9b26dba603` / `3ae415be06`** — `block: elevator: match vendor_init subcontext (PPID 1) and expose default_iosched param`
  - **改动文件**：`block/elevator.c`
  - **内容**：暴露 `/sys/module/elevator/parameters/default_iosched` 参数消除 Clang 22 常量条件告警，并复用 `is_init_or_init_child()`（同时匹配 PID 1 与 Android Treble `vendor_init` PPID 1 子进程），确保开机无损直落 `[ssg]` 且开机后随时可自由热切。

- **`e28486f19a`** — `block: add Kconfig direct defaults for I/O scheduler and read_ahead_kb`
  - **改动文件**：`block/Kconfig.iosched`、`block/Kconfig`、`block/elevator.c`、`block/blk-sysfs.c`、`mm/backing-dev.c`、`block/blk-settings.c`、`arch/arm64/configs/vendor/socrates_GKI.config`、`.github/workflows/build_full_kernel.yml`
  - **内容**：
    - 新增 `CONFIG_DEFAULT_IOSCHED`（默认 `"ssg"`），用 Kconfig 通用无损指定默认调度器，替换原硬编码；`enforce_default_iosched_on_init` 仅对开机 init 写 `bfq` 生效，保留全部调度器随时热切；
    - 新增 `CONFIG_DEFAULT_READ_AHEAD_KB`（默认 `128`），在 `mm/backing-dev.c` 与 `block/blk-settings.c` 原生直接初始化 `ra_pages`，并将 `blk-sysfs.c` 由原先的全局硬上限 `max_read_ahead_kb` 改为仅规范 `init` 及 `init` 直属子服务（如 `kernel-post-boot`）的直接默认值 `default_read_ahead_kb`，开机后用户手动调整任意大小不再受限；
    - 在 GitHub Actions `build_full_kernel.yml` 暴露 `default_iosched` 与 `default_read_ahead_kb` 下拉菜单，构建时即可无损直选。
  - **运行时控制 / 回退**：`/sys/module/elevator/parameters/{default_iosched,enforce_default_iosched_on_init}`、`/sys/module/blk_sysfs/parameters/{default_read_ahead_kb,enforce_default_ra_on_init}`；源码回退 `git revert e28486f19a`。

- **`4e659c6a61`** — `block: elevator: default to Samsung SSG and keep it across init bfq override`
  - **改动文件**：`block/elevator.c`
  - **内容**：在 `elevator_get_default()` 与 `elv_iosched_store()` 初步引入 SSG 默认策略（后由 `e28486f19a` 通用化为 Kconfig 参数）。

- **`1cdd5b43f8` / `f429c9c707` / `4e7132d0d3`** — `lib/lz4: upgrade to pure upstream LZ4 v1.11.1 and drop lz4armv8 NEON`
  - **改动文件**：`lib/lz4/{Makefile,lz4.h,lz4.c,lz4hc.h,lz4hc.c}`、删除 `lib/lz4/lz4armv8/*`、`crypto/lz4.c`、`crypto/lz4hc.c`、`fs/f2fs/compress.c`、`fs/incfs/data_mgmt.c`
  - **内容**：切换为纯上游 **`lz4/lz4 v1.11.1`**（极简内核 `LZ4_FREESTANDING` / `wrkmem` / 19 个标准 `EXPORT_SYMBOL` 胶水与 `LZ4HC_HEAPMODE=1` 防内核栈溢出），彻底移除树外 `lz4armv8` NEON 汇编路径，消除 4KB ZRAM 解压的 FPU 切换/关抢占开销。

- **`1c3905a812`** — `block: port Samsung SSG I/O scheduler with cgroup support for Linux 5.15`
  - **改动文件**：`block/ssg-iosched.c`、`block/ssg-cgroup.c`、`block/ssg-cgroup.h`、`block/Kconfig.iosched`、`block/Makefile`、`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：移植三星 SM8550 同源 **SSG**（SamSung Generic）I/O 调度器及 `ssg-cgroup` 模块至 Linux `5.15.225`（零 `include/linux/` 结构体改动）；针对 LineageOS 原生 `/dev/blkio` 优化 `ssg_blkcg_cpd_init()`（根组 `100`，子组 `/dev/blkio/background` 默认 `25` 浅队列限流）。

- **`4b54adb8d8` / `7377105e85`** — `block: backport ADIOS v3.3.0 I/O scheduler for Linux 5.15`
  - **改动文件**：`block/adios.c`、`block/Kconfig.iosched`、`block/Makefile`、`arch/arm64/configs/vendor/socrates_GKI.config`
  - **内容**：引入单文件 **ADIOS v3.3.0**（Adaptive Deadline I/O Scheduler），适配 Linux 5.15 合并与比较回调，内置启用以供随时热切换对比。

- **`16841ec045`** — `sched: backport mainline idle/rt/misfit and asym-idle balance fixes`
  - **改动文件**：`kernel/sched/idle.c`、`kernel/sched/rt.c`、`kernel/sched/fair.c`
  - **内容**：保留高通原生 `sched_walt` EAS 主干，合入 4 项 Linux 主线精准修复（去除 `do_idle` 多余 `rmb()`、跳过 `rto_next_cpu` 空推送、`migrate_misfit` 不污染均衡计数与间隔、`_nohz_idle_balance` 升序小核优先）。

- **`71349d892e`** — `mm/block: set default MGLRU min_ttl_ms to 1000 and cap queue read_ahead_kb to 128`
  - **改动文件**：`mm/vmscan.c`、`block/blk-sysfs.c`
  - **内容**：将 MGLRU `lru_gen_min_ttl` 默认初始化为 `HZ`（`1000 ms`），并初步限制 `read_ahead_kb`（后由 `e28486f19a` 改为无损 init 默认值方案）。

- **`b6ae314955`** — `arm64: configs: socrates: enable WQ_POWER_EFFICIENT_DEFAULT and ZRAM writeback`
  - **改动文件**：`arch/arm64/configs/vendor/socrates_GKI.config`（现已迁移至 `sm8550_custom_GKI.config`）
  - **内容**：开启 `CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`、`CONFIG_ZRAM_WRITEBACK=y` 与 `CONFIG_ZRAM_MEMORY_TRACKING=y`。

- **`898d43cf65` / `57a323239e`** — `ci: fix ccache path, deduplicate dist artifacts, and optimize runner setup`
  - **内容**：修复 Ubuntu 24.04 `ccache` 路径、启用压缩与 sloppiness、消除 `dist/` 产物套娃、按需释放磁盘并输出 Step Summary。

- **`0a4a5ba1aa`** — `ci: fix AnyKernel3 vendor_dlkm flashing for Xiaomi SM8550 super layout`
  - **内容**：修复 SM8550 动态分区 `super` 下 `vendor_dlkm` 刷入逻辑，镜像不超分区大小时采用原位 `dd` + SHA256 逐字节校验。

- **`f855a05e6c`** — `arm64: configs: socrates: set LZ4 as default ZRAM compression algorithm`
  - **内容**：将默认 ZRAM 压缩算法设为 `lz4`。

### 2026-11-24

- **`fe4c103e68`** — `drivers: rekernel: integrate built-in Re-Kernel v11.7`
  - **内容**：源码内建 Re-Kernel v11.7（`CONFIG_REKERNEL=y`，Generic Netlink），配合墓碑模块实现内核级冻结通信。

- **`9f4c1cb614`** — `net: usb: ax88179_178a: restore vendor ASIX driver compatible with ax_main`
  - **内容**：修复合并上游 `5.15-lts` 后 `ax88179_178a` 与高通/厂商 `ax_main` 驱动符号不兼容问题。

- **`a9f9040f4f`** — `Merge remote-tracking branch 'upstream/android13-5.15-lts' into lineage-24.0`
  - **内容**：合并 Google Android Common Kernel `android13-5.15-lts` 上游安全与稳定性补丁至 `5.15.225`。

- **`15bd99870d` / `135e36be94` / `4242ee0558` / `4eb300f9bf`** — 三合一完整内核构建与上游同步工作流初版建立。

- **`d3d7d6a5eb`** — `arm64: socrates: enable RCU_LAZY by default and apply mm/f2fs micro-optimizations`
  - **内容**：默认开启 `RCU_LAZY`；`list_lru_count_one()` 快路径跳过多余 RCU 读锁；F2FS 紧急 GC 休眠由 500ms 缩至 50ms。

- **`49e2ddf0d9`** — 默认静默 AVC 审计与 Binder 调试日志
  - **内容**：默认静默 SELinux AVC audit 刷屏日志（保留 `disable_audit_log` 运行时模块参数可随时恢复）及 Binder debug 日志。

- **`4678a3a783`** — `kernel: backport safe ARM64 micro-opts and power/wakeup cleanups`
  - **内容**：ARM64 `clear_page` 16B 对齐、`copy_template` 流式预取；消除冗余 `s2idle_wake()`；最小化 `alarmtimer` 唤醒超时；PCI PME 轮询放宽至 4s；降级 IRQ affinity 日志级别。

- **`5ccf5ad45e` / `73d35f405f` / `54c1ff8226` / `6185ce2721` / `47fa49c155`** — TCP BBRv3 + FQ qdisc 与 F2FS 拥塞停顿 / 最小 fsync 块数优化。

---

## 三、可选项与后续可调整清单（支持单项快速回退/验证）

| 状态 | 特性 | Commit SHA | 说明与后续调整方式 |
| :--- | :--- | :--- | :--- |
| **已合入（可随时单撤）** | WALT `uclamp` 频导回调（`WALT_CPUFREQ_UCLAMP`） | `b2b9f26eb8` | 入队/出队 `rq` `uclamp` 变化即触发调频回调，响应更跟手；若实测功耗或抖频不理想，执行 `git revert b2b9f26eb8` 即可无损撤回 |
| **已合入（可随时微调）** | 同步 Binder UI `WALT_BINDER_MVP` 补盲 | `c92a22bda8` | 覆盖 AOSP/LineageOS 同步 UI/RT Binder 链路，受 3ms 时间片与 500ms/100ms 节流保护；如需收紧/放宽触发门槛或撤回，直接调整或 `git revert c92a22bda8` |
| **待观察（暂未合入）** | 同核竞争抢占优化（BORE / EEVDF 思路） | — | 仅影响同核 CFS 抢占顺序、不干涉 WALT 选核/调频；待当前版本实测确认重并发前台是否仍有痛点后再做 A/B 评估 |
| **待观察（暂未合入）** | `/vendor/etc/powerhint.json` `INTERACTION` 参数 | — | 仅在当前原生状态实测抓到明确耗电/掉帧 trace 证据后再评估，不做盲目覆盖 |

---

## 四、已评估但主动排除（未合入）的项（防重复踩坑）

1. **更换第三方魔改 Clang 工具链**：放弃。当前 ROM（LineageOS 24.0）与 `sm8550-modules` 及 YukiSU LKM（依赖 `vmlinux.btf` 与 `clang-cfi/.cfi_jt`）均针对 AOSP Clang 22.0.2 (`r596125`) 验证，换工具链易导致 CFI/BTF 不兼容不开机。
2. **源码内置 KernelSU/YukiSU**：放弃。保持主内核纯净，使用 `init_boot` LKM 方案实现内核与 Root 解耦，换内核不掉 Root 且便于上游 LTS 合并。
3. **高侵入内存回收魔改（如 `kshrink_slabd` / `kshrink_lruvecd` / `mi_rmap`）与 HyperOS 专属模块（`mi_sw_sync` / `unionpower` / `sew_audit`）**：放弃。侵入 `mm/` 核心代码极易引起上游合并冲突与记账/竞态问题，且 LineageOS 24.0 环境下原厂 MGLRU 已足够稳定。
4. **实验性调度大改（`launch_boost` / `sew_yield_penalty` / 强切 RT 的 Binder 调度 / 第三方覆盖式 sysfs 调度脚本）**：放弃。12 GB RAM + 骁龙 8 Gen 2 下原生 `vendor.power-hal-aidl` + 高通 `sched_walt` 已具备良好表现。

---

## 五、排错与快速回退速查

- **按单项精确回退**：在 Git 仓库中直接执行 `git revert <Commit SHA>` 即可干净撤销对应特性。
- **常见运行时验证命令（手机 Root 终端）**：
  ```sh
  uname -a                                                        # 核对内核版本与提交号
  cat /proc/sys/net/ipv4/tcp_congestion_control                   # 应为 bbr3
  cat /proc/sys/net/core/default_qdisc                            # 应为 fq
  cat /sys/kernel/mm/lru_gen/enabled                              # 应为 0x0003
  cat /sys/kernel/mm/lru_gen/min_ttl_ms                           # 应为 1000
  cat /sys/block/sda/queue/read_ahead_kb                          # 应为 128
  cat /sys/block/sda/queue/scheduler                              # 应为 [ssg]（同时包含 adios/bfq/kyber/mq-deadline 可热切）
  cat /sys/module/elevator/parameters/default_iosched             # 应为 ssg
  cat /sys/module/elevator/parameters/enforce_default_iosched_on_init # 应为 Y
  cat /sys/module/blk_sysfs/parameters/default_read_ahead_kb      # 应为 128（仅规范 init 开机非零写入，手动调整不受限）
  cat /sys/module/workqueue/parameters/power_efficient            # 应为 Y
  cat /sys/block/zram0/comp_algorithm                             # 应包含 [lz4]
  ls /sys/block/zram0/backing_dev                                 # 应存在（ZRAM Writeback 接口）
  cat /dev/blkio/background/blkio.ssg.max_available_ratio         # 应为 25
  ```
