// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Core Lifecycle & WALT Bridge
 */
#include <linux/init.h>
#include <linux/kfas.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <trace/events/sched.h>

#include "kfas_internal.h"

#define KFAS_IDLE_TIMEOUT_MS	1000

struct kfas_state g_kfas;

const char *kfas_mode_name(enum kfas_mode mode)
{
	switch (mode) {
	case KFAS_MODE_POWERSAVE:
		return "powersave";
	case KFAS_MODE_BALANCE:
		return "balance";
	case KFAS_MODE_PERFORMANCE:
		return "performance";
	case KFAS_MODE_FAST:
		return "fast";
	default:
		return "balance";
	}
}

const char *kfas_bn_name(enum kfas_bottleneck bn)
{
	switch (bn) {
	case KFAS_BN_OK:
		return "OK";
	case KFAS_BN_CPU:
		return "CPU";
	case KFAS_BN_SCHED:
		return "SCHED";
	case KFAS_BN_GPU:
		return "GPU";
	default:
		return "OK";
	}
}

int kfas_register_walt_ops(const struct kfas_walt_ops *ops)
{
	if (!ops)
		return -EINVAL;
	smp_store_release(&g_kfas.walt_ops, ops);
	pr_info("kfas: sched_walt bridge registered\n");
	return 0;
}
EXPORT_SYMBOL_GPL(kfas_register_walt_ops);

void kfas_unregister_walt_ops(void)
{
	smp_store_release(&g_kfas.walt_ops, NULL);
}
EXPORT_SYMBOL_GPL(kfas_unregister_walt_ops);

static void kfas_init_default_profiles(void)
{
	g_kfas.enabled = true;
	g_kfas.mode = KFAS_MODE_BALANCE;
	g_kfas.placement_enable = true;
	g_kfas.contain_workers = true;
	g_kfas.unpause_big = true;

	g_kfas.mode_cfg[KFAS_MODE_POWERSAVE] = (struct kfas_mode_cfg){
		.margin_pct = 4,
		.up_gain = 85,
		.down_gain = 140,
		.down_hold_frames = 12,
		.little_floor_khz = 1017600,
		.temp_thresh_mc = 82000,
		.worker_uclamp_max = 260,
		.crit_uclamp_min = 320,
		.prime_enable = false,
	};

	g_kfas.mode_cfg[KFAS_MODE_BALANCE] = (struct kfas_mode_cfg){
		.margin_pct = 8,
		.up_gain = 115,
		.down_gain = 105,
		.down_hold_frames = 18,
		.little_floor_khz = 1228800,
		.temp_thresh_mc = 0,
		.worker_uclamp_max = 320,
		.crit_uclamp_min = 380,
		.prime_enable = true,
	};

	g_kfas.mode_cfg[KFAS_MODE_PERFORMANCE] = (struct kfas_mode_cfg){
		.margin_pct = 12,
		.up_gain = 150,
		.down_gain = 75,
		.down_hold_frames = 28,
		.little_floor_khz = 1344000,
		.temp_thresh_mc = 95000,
		.worker_uclamp_max = 380,
		.crit_uclamp_min = 480,
		.prime_enable = true,
	};

	g_kfas.mode_cfg[KFAS_MODE_FAST] = (struct kfas_mode_cfg){
		.margin_pct = 16,
		.up_gain = 190,
		.down_gain = 50,
		.down_hold_frames = 40,
		.little_floor_khz = 1459200,
		.temp_thresh_mc = 95000,
		.worker_uclamp_max = 440,
		.crit_uclamp_min = 580,
		.prime_enable = true,
	};
}

static bool kfas_pop_frame(struct kfas_frame_sample *out)
{
	unsigned long flags;
	bool ok = false;

	raw_spin_lock_irqsave(&g_kfas.frame_lock, flags);
	if (g_kfas.ring_tail != g_kfas.ring_head) {
		*out = g_kfas.ring[g_kfas.ring_tail];
		g_kfas.ring_tail = (g_kfas.ring_tail + 1) & KFAS_RING_MASK;
		ok = true;
	}
	raw_spin_unlock_irqrestore(&g_kfas.frame_lock, flags);
	return ok;
}

static void kfas_frame_work_fn(struct work_struct *work)
{
	struct kfas_frame_sample sample;
	bool was_rendering;
	bool processed = false;

	mutex_lock(&g_kfas.ctrl_mutex);
	if (!g_kfas.session_active) {
		mutex_unlock(&g_kfas.ctrl_mutex);
		return;
	}

	was_rendering = g_kfas.rendering_active;
	while (kfas_pop_frame(&sample)) {
		kfas_freq_on_frame_locked(&sample);
		processed = true;
	}

	if (processed) {
		mod_delayed_work(g_kfas.wq, &g_kfas.idle_work,
				 msecs_to_jiffies(KFAS_IDLE_TIMEOUT_MS));
		if (!was_rendering && g_kfas.placement_enable)
			mod_delayed_work(g_kfas.wq, &g_kfas.place_work,
					 msecs_to_jiffies(20));
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
}

static void kfas_idle_work_fn(struct work_struct *work)
{
	mutex_lock(&g_kfas.ctrl_mutex);
	if (g_kfas.session_active && g_kfas.rendering_active) {
		g_kfas.rendering_active = false;
		g_kfas.jank_streak = 0;
		g_kfas.surplus_streak = 0;
		g_kfas.down_cooldown = 0;
		/* Relax ceilings & core_ctl during loading/paused screens */
		kfas_freq_release_limits_locked();
		kfas_place_idle_relax_locked();
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
}

static void kfas_exit_work_fn(struct work_struct *work)
{
	pid_t exited = atomic_xchg(&g_kfas.exiting_pid, 0);

	if (exited <= 0)
		return;

	mutex_lock(&g_kfas.session_mutex);
	if (g_kfas.session_active && g_kfas.target_pid == exited) {
		pr_info("kfas: target pid %d exited, auto-detaching\n", exited);
		kfas_session_stop_locked();
	}
	mutex_unlock(&g_kfas.session_mutex);
}

void kfas_schedule_exit_cleanup(pid_t tgid)
{
	if (g_kfas.wq && tgid > 0 && tgid == READ_ONCE(g_kfas.target_pid)) {
		atomic_set(&g_kfas.exiting_pid, tgid);
		queue_work(g_kfas.wq, &g_kfas.exit_work);
	}
}

static void kfas_tp_process_exit(void *data, struct task_struct *p)
{
	pid_t tgid = READ_ONCE(g_kfas.target_pid);

	if (tgid > 0 && p->pid == tgid && thread_group_leader(p))
		kfas_schedule_exit_cleanup(tgid);
}

int kfas_session_start_locked(pid_t pid, const u32 *fps_list, u32 nr_fps)
{
	struct task_struct *leader;
	int ret;

	if (!g_kfas.enabled)
		return -EPERM;

	if (g_kfas.session_active)
		kfas_session_stop_locked();

	rcu_read_lock();
	leader = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (leader && pid_alive(leader))
		get_task_struct(leader);
	else
		leader = NULL;
	rcu_read_unlock();

	if (!leader)
		return -ESRCH;

	mutex_lock(&g_kfas.ctrl_mutex);
	ret = kfas_freq_ensure_init_locked();
	if (ret) {
		mutex_unlock(&g_kfas.ctrl_mutex);
		put_task_struct(leader);
		return ret;
	}

	g_kfas.nr_cand_fps = min_t(u32, nr_fps, KFAS_MAX_TARGET_FPS);
	memcpy(g_kfas.cand_fps, fps_list, g_kfas.nr_cand_fps * sizeof(u32));
	atomic_set(&g_kfas.exiting_pid, 0);
	kfas_freq_reset_session_locked();
	kfas_place_session_start_locked();
	g_kfas.session_active = true;
	mutex_unlock(&g_kfas.ctrl_mutex);

	ret = kfas_uprobe_attach_locked(leader);
	put_task_struct(leader);

	if (ret) {
		mutex_lock(&g_kfas.ctrl_mutex);
		g_kfas.session_active = false;
		kfas_place_session_stop_locked();
		kfas_freq_release_limits_locked();
		mutex_unlock(&g_kfas.ctrl_mutex);
		return ret;
	}

	return 0;
}

void kfas_session_stop_locked(void)
{
	if (!g_kfas.session_active && !g_kfas.target_mm && !g_kfas.probe_inode)
		return;

	/*
	 * 1. Detach uprobes first so no new frame callbacks fire.
	 * 2. Mark session inactive & drain pending works outside ctrl_mutex.
	 * 3. Restore all FreqQoS, core_ctl, WALT pipeline, and uclamp state.
	 */
	kfas_uprobe_detach_locked();

	mutex_lock(&g_kfas.ctrl_mutex);
	g_kfas.session_active = false;
	g_kfas.rendering_active = false;
	mutex_unlock(&g_kfas.ctrl_mutex);

	cancel_work_sync(&g_kfas.frame_work);
	cancel_delayed_work_sync(&g_kfas.idle_work);
	cancel_delayed_work_sync(&g_kfas.place_work);

	mutex_lock(&g_kfas.ctrl_mutex);
	kfas_place_session_stop_locked();
	kfas_freq_release_limits_locked();
	g_kfas.render_tid = 0;
	g_kfas.cur_fps_x10 = 0;
	g_kfas.last_bn = KFAS_BN_OK;
	mutex_unlock(&g_kfas.ctrl_mutex);
}

static int __init kfas_init(void)
{
	int ret;

	mutex_init(&g_kfas.session_mutex);
	mutex_init(&g_kfas.ctrl_mutex);
	raw_spin_lock_init(&g_kfas.frame_lock);
	raw_spin_lock_init(&g_kfas.dq_lock);

	kfas_init_default_profiles();

	g_kfas.wq = alloc_workqueue("kfas_wq",
				    WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM,
				    1);
	if (!g_kfas.wq)
		return -ENOMEM;

	INIT_WORK(&g_kfas.frame_work, kfas_frame_work_fn);
	INIT_WORK(&g_kfas.exit_work, kfas_exit_work_fn);
	INIT_DELAYED_WORK(&g_kfas.idle_work, kfas_idle_work_fn);
	INIT_DELAYED_WORK(&g_kfas.place_work, kfas_place_work_fn);

	kfas_gpu_init();
	kfas_place_init();

	ret = register_trace_sched_process_exit(kfas_tp_process_exit, NULL);
	if (ret)
		pr_warn("kfas: failed to register sched_process_exit (%d)\n",
			ret);

	ret = kfas_sysfs_init();
	if (ret) {
		pr_err("kfas: sysfs init failed: %d\n", ret);
		unregister_trace_sched_process_exit(kfas_tp_process_exit, NULL);
		kfas_gpu_exit();
		destroy_workqueue(g_kfas.wq);
		return ret;
	}

	pr_info("kfas: initialized v%s (idle bypass active)\n", KFAS_VERSION);
	return 0;
}
late_initcall(kfas_init);
