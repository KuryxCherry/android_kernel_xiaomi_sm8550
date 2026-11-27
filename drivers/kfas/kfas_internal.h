/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Internal Definitions
 */
#ifndef _KFAS_INTERNAL_H
#define _KFAS_INTERNAL_H

#include <linux/atomic.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kfas.h>
#include <linux/kobject.h>
#include <linux/limits.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/uprobes.h>
#include <linux/workqueue.h>

#define KFAS_VERSION		"1.1.0"

#define KFAS_MAX_CLUSTERS	4
#define KFAS_MAX_OPPS		64
#define KFAS_MAX_PROBES		4
#define KFAS_MAX_TARGET_FPS	8
#define KFAS_RING_SIZE		64
#define KFAS_RING_MASK		(KFAS_RING_SIZE - 1)
#define KFAS_FPS_WINDOW		32
#define KFAS_SURFACE_SLOTS	8
#define KFAS_DQ_SLOTS		16
#define KFAS_WAKE_SLOTS		8
#define KFAS_MAX_MANAGED_TIDS	96
#define KFAS_MAX_SCAN_THREADS	128
#define KFAS_MAX_CRITICAL	3
#define KFAS_DAILY_MAX_MANAGED	128
#define KFAS_DAILY_MAX_SCAN	192
#define KFAS_DAILY_MAX_CRIT	3
#define KFAS_MAX_VIP_COMMS	8
#define KFAS_VIP_COMM_LEN	16
#define KFAS_MAX_PATH_LEN	256

enum kfas_mode {
	KFAS_MODE_POWERSAVE = 0,
	KFAS_MODE_BALANCE,
	KFAS_MODE_PERFORMANCE,
	KFAS_MODE_FAST,
	KFAS_MODE_COUNT,
};

enum kfas_bottleneck {
	KFAS_BN_OK = 0,
	KFAS_BN_CPU,
	KFAS_BN_SCHED,
	KFAS_BN_GPU,
};

enum kfas_probe_kind {
	KFAS_PROBE_NONE = 0,
	KFAS_PROBE_QUEUE = 1,
	KFAS_PROBE_DEQUEUE = 2,
};

struct kfas_mode_cfg {
	u32 margin_pct;		/* Headroom reserved from frame budget (%) */
	u32 up_gain;		/* Step-up aggressiveness (100 = 1.0x) */
	u32 down_gain;		/* Step-down smoothness (100 = 1.0x) */
	u32 down_hold_frames;	/* Consecutive clean frames before stepping down */
	u32 little_floor_khz;	/* Min ceiling floor on little cluster (kHz) */
	u32 temp_thresh_mc;	/* Soft thermal guardrail (milli-Celsius, 0 = off) */
	u32 worker_uclamp_max;	/* Tier-2 worker uclamp_max (0..1024) */
	u32 crit_uclamp_min;	/* Base uclamp_min for Tier-0/1 threads */
	bool prime_enable;	/* Allow prime core full boost & pinning */
};

struct kfas_probe_slot {
	enum kfas_probe_kind kind;
	loff_t offset;
	bool registered;
	struct uprobe_consumer uc;
};

struct kfas_frame_sample {
	u64 ts_ns;
	u64 frame_ns;
	u64 exec_ns;
	u64 delay_ns;
	u64 dq_wait_ns;
	pid_t tid;
	u8 cpu;
};

struct kfas_surface_slot {
	u64 surface;
	u64 last_ts_ns;
	u64 last_exec_ns;
	u64 last_delay_ns;
	pid_t last_tid;
	u32 count;
};

struct kfas_dq_slot {
	pid_t tid;
	u64 enter_ns;
	u64 last_wait_ns;
	u64 wait_ts_ns;
};

struct kfas_cluster {
	unsigned int policy_cpu;
	cpumask_t related_cpus;
	unsigned long cap_orig;
	unsigned int nr_opps;
	unsigned int opps[KFAS_MAX_OPPS];	/* ascending kHz */
	unsigned int cur_max_khz;
	unsigned int cur_min_khz;
	struct freq_qos_request max_req;
	struct freq_qos_request min_req;
	bool qos_ready;
};

struct kfas_gpu_snap {
	u32 busy_pct;
	u32 freq_mhz;
	u32 max_freq_mhz;
	u64 ts_ns;
	bool valid;
};

struct kfas_wake_slot {
	pid_t tid;
	u32 count;
	u64 last_ts_ns;
};

struct kfas_managed_tid {
	pid_t tid;
	char comm[TASK_COMM_LEN];
	u8 tier;		/* 0 = render, 1 = primary logic, 2 = worker */
	int assigned_cpu;
	int actual_cpu;
	u32 demand;
	u64 last_exec_ns;
	u32 delta_exec_us;
	u32 uclamp_min;
	u32 uclamp_max;
	bool walt_pinned;
	bool uclamp_applied;
};

struct kfas_topo {
	bool ready;
	int prime_cpu;
	int high_mid_cpus[NR_CPUS];	/* e.g. Cortex-A715 (cpu3-4) */
	int nr_high_mid;
	int low_mid_cpus[NR_CPUS];	/* e.g. Cortex-A710 (cpu5-6) */
	int nr_low_mid;
	int little_cpus[NR_CPUS];	/* e.g. Cortex-A510 (cpu0-2) */
	int nr_little;
};

/*
 * Lock ordering (strictly enforced to prevent deadlock):
 *   session_mutex -> ctrl_mutex -> (frame_lock | dq_lock | wake_lock | gpu_lock)
 * Spinlocks are NEVER held across sleeping calls (uprobe/freq_qos/sched_setattr).
 */
struct kfas_state {
	struct mutex session_mutex;
	struct mutex ctrl_mutex;
	struct workqueue_struct *wq;
	struct work_struct frame_work;
	struct work_struct exit_work;
	struct delayed_work idle_work;
	struct delayed_work place_work;
	struct delayed_work daily_work;

	/* Master configuration */
	bool enabled;
	enum kfas_mode mode;
	bool placement_enable;
	bool contain_workers;
	bool unpause_big;
	bool daily_enable;
	struct kfas_mode_cfg mode_cfg[KFAS_MODE_COUNT];

	/* Probe specification (configured by userspace once) */
	char lib_path[KFAS_MAX_PATH_LEN];
	u32 nr_queue_offsets;
	loff_t queue_offsets[2];
	u32 nr_dequeue_offsets;
	loff_t dequeue_offsets[2];

	/* Active game session */
	bool session_active;
	pid_t target_pid;
	atomic_t exiting_pid;
	struct mm_struct *target_mm;
	struct inode *probe_inode;
	struct kfas_probe_slot probes[KFAS_MAX_PROBES];
	u32 nr_probes;

	/* Target FPS candidates & lock state */
	u32 cand_fps[KFAS_MAX_TARGET_FPS];
	u32 nr_cand_fps;
	u32 locked_fps;
	u64 fps_ring[KFAS_FPS_WINDOW];
	u32 fps_ring_idx;
	u32 fps_ring_cnt;
	u32 fps_eval_cnt;
	u32 fps_down_streak;

	/* Frame ring buffer & surface dominance (protected by frame_lock) */
	raw_spinlock_t frame_lock;
	struct kfas_frame_sample ring[KFAS_RING_SIZE];
	u32 ring_head;
	u32 ring_tail;
	u64 frames_total;
	u64 frames_dropped_ring;
	struct kfas_surface_slot surfaces[KFAS_SURFACE_SLOTS];
	u64 dominant_surface;

	/* Dequeue blocking stash (protected by dq_lock) */
	raw_spinlock_t dq_lock;
	struct kfas_dq_slot dq_slots[KFAS_DQ_SLOTS];

	/* Closed-loop controller telemetry (protected by ctrl_mutex) */
	bool rendering_active;
	u64 last_frame_ts_ns;
	u32 perf_level;		/* 0..1000 normalized compute headroom */
	u32 surplus_streak;
	u32 jank_streak;
	u32 down_cooldown;
	u32 cur_fps_x10;	/* EMA fps * 10 */
	u32 ema_frame_us;
	u32 ema_exec_us;
	u32 ema_delay_us;
	u32 ema_dq_wait_us;
	enum kfas_bottleneck last_bn;
	u32 last_cpu_temp_mc;
	pid_t render_tid;

	/* CPU clusters & FreqQoS */
	struct kfas_cluster clusters[KFAS_MAX_CLUSTERS];
	u32 nr_clusters;
	bool freq_inited;
	struct thermal_zone_device *tz_cpu;

	/* WALT bridge & thread placement */
	const struct kfas_walt_ops *walt_ops;
	bool core_ctl_boosted;
	bool prime_in_use;
	struct kfas_topo topo;
	bool waking_tp_active;
	raw_spinlock_t wake_lock;
	struct kfas_wake_slot wake_slots[KFAS_WAKE_SLOTS];
	struct kfas_managed_tid managed[KFAS_MAX_MANAGED_TIDS];
	u32 nr_managed;
	char vip_comms[KFAS_MAX_VIP_COMMS][KFAS_VIP_COMM_LEN];
	u32 nr_vip_comms;

	/* Daily non-game app thread assist */
	bool daily_active;
	pid_t daily_pid;
	atomic_t exiting_daily_pid;
	u32 daily_pass;
	u32 daily_nr_crit;
	u32 daily_nr_trash;
	struct kfas_managed_tid daily_managed[KFAS_DAILY_MAX_MANAGED];
	u32 nr_daily_managed;

	/* GPU devfreq telemetry (protected by gpu_lock) */
	raw_spinlock_t gpu_lock;
	struct kfas_gpu_snap gpu;
	bool gpu_tp_active;

	/* Sysfs */
	struct kobject *kobj;
};

extern struct kfas_state g_kfas;

/* kfas_main.c */
int kfas_session_start_locked(pid_t pid, const u32 *fps_list, u32 nr_fps);
void kfas_session_stop_locked(void);
void kfas_schedule_exit_cleanup(pid_t tgid);
const char *kfas_mode_name(enum kfas_mode mode);
const char *kfas_bn_name(enum kfas_bottleneck bn);

/* kfas_uprobe.c */
int kfas_uprobe_attach_locked(struct task_struct *leader);
void kfas_uprobe_detach_locked(void);
void kfas_uprobe_reset_state(void);

/* kfas_freq.c */
int kfas_freq_ensure_init_locked(void);
void kfas_freq_cleanup(void);
void kfas_freq_reset_session_locked(void);
void kfas_freq_release_limits_locked(void);
void kfas_freq_on_frame_locked(const struct kfas_frame_sample *s);

/* kfas_place.c */
void kfas_place_init(void);
void kfas_place_session_start_locked(void);
void kfas_place_session_stop_locked(void);
void kfas_place_idle_relax_locked(void);
void kfas_place_boost_urgent_locked(void);
void kfas_place_work_fn(struct work_struct *work);
ssize_t kfas_place_format_threads(char *buf, size_t size);

/* kfas_daily.c */
void kfas_daily_init(void);
int kfas_daily_start_locked(pid_t pid);
void kfas_daily_stop_locked(void);
void kfas_daily_work_fn(struct work_struct *work);
ssize_t kfas_daily_format_threads(char *buf, size_t size);

/* kfas_gpu.c */
void kfas_gpu_init(void);
void kfas_gpu_exit(void);
void kfas_gpu_get_snapshot(struct kfas_gpu_snap *out);

/* kfas_sysfs.c */
int kfas_sysfs_init(void);
void kfas_sysfs_exit(void);

#endif /* _KFAS_INTERNAL_H */
