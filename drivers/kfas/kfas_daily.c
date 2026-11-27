// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Daily App Thread Assist Engine
 *
 * Provides event-driven, zero-polling thread placement for non-game foreground
 * Android apps without touching FreqQoS, uprobes, core_ctl, or cpusets:
 *   - Tier 0 (UI Critical): Main UI thread, RenderThread, Flutter/WebView UI
 *     -> routed via WALT PIPELINE_FASTPATH + MVP onto Cortex-A715 gold cores
 *        with a gentle mode-scaled uclamp_min and synchronous Binder inheritance.
 *   - Tier 2 (Background/Trash): Known telemetry/log/ad-SDK threads and
 *     Android background-priority workers (nice >= 8) with low demand
 *     -> softly contained to little cores via uclamp_max (with RESET_ON_FORK)
 *        while protecting Binder, Audio, MediaCodec, Camera, and ART GC.
 *
 * Runs at most two lightweight passes per foreground app switch (immediate +
 * 1.5s cold-start catch-up), then sleeps at 0% CPU until app exit, screen off,
 * or game takeover, cleanly restoring every modified thread.
 */
#include <linux/cpumask.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/walt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "kfas_internal.h"

#define KFAS_DAILY_CATCHUP_MS	1500

struct kfas_daily_scan_entry {
	pid_t tid;
	char comm[TASK_COMM_LEN];
	u32 demand;
	int policy;
	int nice_val;
	int cur_cpu;
	u8 ui_prio;		/* 0 = none, 1 = other UI, 2 = main, 3 = RenderThread */
	bool is_protected;
	bool is_trash;
};

static struct kfas_daily_scan_entry *g_daily_scan;
static struct kfas_managed_tid *g_daily_next;

static bool kfas_has_prefix(const char *s, const char *pfx)
{
	return strncmp(s, pfx, strlen(pfx)) == 0;
}

static u8 kfas_daily_ui_prio(pid_t tid, pid_t leader_pid, const char *comm)
{
	if (kfas_has_prefix(comm, "RenderThread"))
		return 3;
	if (tid == leader_pid)
		return 2;
	if (kfas_has_prefix(comm, "1.ui") ||
	    kfas_has_prefix(comm, "1.raster") ||
	    kfas_has_prefix(comm, "CrRendererMain") ||
	    kfas_has_prefix(comm, "VizCompositor") ||
	    kfas_has_prefix(comm, "Compositor") ||
	    kfas_has_prefix(comm, "mqt_js") ||
	    kfas_has_prefix(comm, "mqt_native_modu") ||
	    kfas_has_prefix(comm, "UIThread"))
		return 1;
	return 0;
}

static bool kfas_daily_is_protected(const char *comm)
{
	static const char *const keep[] = {
		"hwuiTask", "Binder:", "binder:", "HwBinder:",
		"Audio", "AAudio", "OpenSL", "MediaCodec", "ExoPlayer",
		"Camera", "GLThread", "Choreographer", "Animator",
		"AsyncLayout", "CrGpuMain", "Jit thread pool",
		"ThreadPoolForeg", "FMOD",
		"HeapTaskDaemon", "Finalizer", "ReferenceQueue",
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(keep); i++) {
		if (kfas_has_prefix(comm, keep[i]))
			return true;
	}
	return false;
}

static bool kfas_daily_is_trash(const char *comm)
{
	static const char *const trash[] = {
		"XLog", "Logger", "logger", "LogThread", "log_thread",
		"ALog", "TLog", "ELog", "slog", "mars::xlog",
		"Beacon", "beacon", "Bugly", "Crash", "Stat", "Analytics",
		"Metric", "Report", "Track", "Trace", "perfetto", "Perfetto",
		"Profile Saver", "Cleanup", "Garbage",
		"mbridge", "pangle", "gdt", "ksad", "tt_",
		"APM", "apm", "Matrix", "matrix",
		"Watchdog", "watchdog", "qm-thread",
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(trash); i++) {
		if (kfas_has_prefix(comm, trash[i]))
			return true;
	}
	return false;
}

static void kfas_daily_restore_all_locked(void)
{
	u32 i;

	for (i = 0; i < g_kfas.nr_daily_managed; i++)
		kfas_managed_restore_entry_locked(&g_kfas.daily_managed[i]);
	g_kfas.nr_daily_managed = 0;
	g_kfas.daily_nr_crit = 0;
	g_kfas.daily_nr_trash = 0;
}

static int kfas_daily_pick_ui_cpu(u32 slot, const cpumask_t *used)
{
	const struct kfas_topo *t = &g_kfas.topo;
	int i;

	if (!t->ready)
		return -1;

	/* Prefer high-IPC Cortex-A715 cores (cpu3-4), then A710 (cpu5-6) */
	if (slot < (u32)t->nr_high_mid &&
	    !cpumask_test_cpu(t->high_mid_cpus[slot], used))
		return t->high_mid_cpus[slot];

	for (i = 0; i < t->nr_high_mid; i++) {
		if (!cpumask_test_cpu(t->high_mid_cpus[i], used))
			return t->high_mid_cpus[i];
	}
	for (i = 0; i < t->nr_low_mid; i++) {
		if (!cpumask_test_cpu(t->low_mid_cpus[i], used))
			return t->low_mid_cpus[i];
	}
	return -1;
}

static void kfas_daily_pass_locked(void)
{
	const struct kfas_walt_ops *ops = smp_load_acquire(&g_kfas.walt_ops);
	pid_t tgid = READ_ONCE(g_kfas.daily_pid);
	struct task_struct *leader, *t;
	u32 nr_scan = 0, nr_crit = 0, nr_trash = 0, nr_next = 0;
	u32 i, j, ui_umin, trash_umax;
	int crit_idx[KFAS_DAILY_MAX_CRIT];
	cpumask_t used_cpus;
	int boost_none;

	if (!g_kfas.daily_active || !tgid || g_kfas.session_active ||
	    !READ_ONCE(g_kfas.daily_enable) || !g_daily_scan || !g_daily_next)
		return;

#if IS_ENABLED(CONFIG_SCHED_WALT)
	boost_none = TASK_BOOST_NONE;
#else
	boost_none = 0;
#endif

	ui_umin = (g_kfas.mode == KFAS_MODE_POWERSAVE) ? 180 :
		  (g_kfas.mode == KFAS_MODE_BALANCE) ? 220 :
		  (g_kfas.mode == KFAS_MODE_PERFORMANCE) ? 295 : 350;
	trash_umax = (g_kfas.mode == KFAS_MODE_POWERSAVE) ? 200 :
		     (g_kfas.mode == KFAS_MODE_BALANCE) ? 220 : 240;

	/* Phase 1: Snapshot thread group under RCU */
	rcu_read_lock();
	leader = pid_task(find_pid_ns(tgid, &init_pid_ns), PIDTYPE_PID);
	if (leader && pid_alive(leader)) {
		for_each_thread(leader, t) {
			struct kfas_daily_scan_entry *e;

			if (nr_scan >= KFAS_DAILY_MAX_SCAN)
				break;
			if (READ_ONCE(t->__state) == TASK_DEAD)
				continue;

			e = &g_daily_scan[nr_scan++];
			e->tid = t->pid;
			get_task_comm(e->comm, t);
			e->demand = (ops && ops->get_task_demand_scaled) ?
				    ops->get_task_demand_scaled(t) : 0;
			e->policy = t->policy;
			e->nice_val = task_nice(t);
			e->cur_cpu = task_cpu(t);
			e->ui_prio = kfas_daily_ui_prio(e->tid, tgid, e->comm);
			e->is_protected = kfas_daily_is_protected(e->comm);
			e->is_trash = (e->ui_prio == 0 && !e->is_protected) &&
				      (e->nice_val >= 8 ||
				       kfas_daily_is_trash(e->comm));
		}
	}
	rcu_read_unlock();

	if (nr_scan == 0)
		return;

	/* Select up to 3 UI-critical threads (RenderThread -> Main -> WebView/Flutter) */
	for (i = 0; i < KFAS_DAILY_MAX_CRIT; i++)
		crit_idx[i] = -1;

	while (nr_crit < KFAS_DAILY_MAX_CRIT) {
		int best = -1;
		u8 best_prio = 0;

		for (i = 0; i < nr_scan; i++) {
			bool dup = false;

			if (g_daily_scan[i].ui_prio == 0)
				continue;
			if (g_daily_scan[i].policy != SCHED_NORMAL &&
			    g_daily_scan[i].policy != SCHED_BATCH)
				continue;
			for (j = 0; j < nr_crit; j++) {
				if (crit_idx[j] == (int)i) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			if (g_daily_scan[i].ui_prio > best_prio) {
				best_prio = g_daily_scan[i].ui_prio;
				best = (int)i;
			}
		}
		if (best < 0)
			break;
		crit_idx[nr_crit++] = best;
	}

	cpumask_clear(&used_cpus);
	memset(g_daily_next, 0,
	       KFAS_DAILY_MAX_MANAGED * sizeof(*g_daily_next));

	/* Phase 2: Route UI-critical threads onto A715 gold cores via WALT MVP */
	for (i = 0; i < nr_crit && nr_next < KFAS_DAILY_MAX_MANAGED; i++) {
		struct kfas_daily_scan_entry *e = &g_daily_scan[crit_idx[i]];
		struct kfas_managed_tid *m = &g_daily_next[nr_next++];
		const struct kfas_managed_tid *prev;
		struct task_struct *task;
		struct pid *pid_ref = NULL;
		int tgt_cpu = kfas_daily_pick_ui_cpu(i, &used_cpus);

		if (tgt_cpu >= 0)
			cpumask_set_cpu(tgt_cpu, &used_cpus);

		m->tid = e->tid;
		strscpy(m->comm, e->comm, sizeof(m->comm));
		m->tier = 0;
		m->assigned_cpu = tgt_cpu;
		m->actual_cpu = e->cur_cpu;
		m->demand = e->demand;
		m->delta_exec_us = 0;
		m->uclamp_min = ui_umin;
		m->uclamp_max = 1024;

		task = kfas_task_get_and_pid(e->tid, tgid, &pid_ref);
		if (task) {
			m->pid_ref = pid_ref;
			prev = kfas_managed_find_prev(g_kfas.daily_managed,
						      g_kfas.nr_daily_managed,
						      e->tid);
			m->orig_reset_on_fork =
				(prev && prev->pid_ref == pid_ref &&
				 prev->uclamp_applied) ?
				prev->orig_reset_on_fork :
				!!task->sched_reset_on_fork;
			if (ops && ops->set_task_placement &&
			    ops->set_task_placement(task, tgt_cpu, true,
						    boost_none, 0) == 0)
				m->walt_pinned = true;
			if (kfas_task_apply_uclamp(task, ui_umin, 1024, true))
				m->uclamp_applied = true;
			put_task_struct(task);
		}
	}

	/* Phase 3: Contain low-demand background/trash threads to little cluster */
	for (i = 0; i < nr_scan && nr_next < KFAS_DAILY_MAX_MANAGED; i++) {
		struct kfas_daily_scan_entry *e = &g_daily_scan[i];
		struct kfas_managed_tid *m;
		const struct kfas_managed_tid *prev;
		struct task_struct *task;
		struct pid *pid_ref = NULL;

		if (!e->is_trash)
			continue;
		if (e->policy != SCHED_NORMAL)
			continue;
		/* Do not throttle a worker currently performing heavy work */
		if (e->demand >= 240)
			continue;

		m = &g_daily_next[nr_next++];
		m->tid = e->tid;
		strscpy(m->comm, e->comm, sizeof(m->comm));
		m->tier = 2;
		m->assigned_cpu = -1;
		m->actual_cpu = e->cur_cpu;
		m->demand = e->demand;
		m->delta_exec_us = 0;
		m->uclamp_min = 0;
		m->uclamp_max = trash_umax;

		task = kfas_task_get_and_pid(e->tid, tgid, &pid_ref);
		if (task) {
			m->pid_ref = pid_ref;
			prev = kfas_managed_find_prev(g_kfas.daily_managed,
						      g_kfas.nr_daily_managed,
						      e->tid);
			m->orig_reset_on_fork =
				(prev && prev->pid_ref == pid_ref &&
				 prev->uclamp_applied) ?
				prev->orig_reset_on_fork :
				!!task->sched_reset_on_fork;
			if (kfas_task_apply_uclamp(task, 0, trash_umax, true))
				m->uclamp_applied = true;
			put_task_struct(task);
		}
		nr_trash++;
	}

	/* Phase 4: Restore any previously managed TID dropped in this pass */
	for (i = 0; i < g_kfas.nr_daily_managed; i++) {
		struct kfas_managed_tid *old = &g_kfas.daily_managed[i];
		bool kept = false;

		for (j = 0; j < nr_next; j++) {
			if (g_daily_next[j].tid == old->tid) {
				if (old->walt_pinned &&
				    !g_daily_next[j].walt_pinned &&
				    old->pid_ref) {
					struct task_struct *task =
						get_pid_task(old->pid_ref,
							     PIDTYPE_PID);

					if (task) {
						if (ops &&
						    ops->clear_task_placement)
							ops->clear_task_placement(task);
						put_task_struct(task);
					}
				}
				if (old->pid_ref) {
					put_pid(old->pid_ref);
					old->pid_ref = NULL;
				}
				kept = true;
				break;
			}
		}
		if (!kept)
			kfas_managed_restore_entry_locked(old);
	}

	memcpy(g_kfas.daily_managed, g_daily_next,
	       nr_next * sizeof(struct kfas_managed_tid));
	g_kfas.nr_daily_managed = nr_next;
	g_kfas.daily_nr_crit = nr_crit;
	g_kfas.daily_nr_trash = nr_trash;
}

void kfas_daily_work_fn(struct work_struct *work)
{
	mutex_lock(&g_kfas.ctrl_mutex);
	if (g_kfas.daily_active && !g_kfas.session_active &&
	    READ_ONCE(g_kfas.daily_enable)) {
		kfas_daily_pass_locked();
		g_kfas.daily_pass++;
		/* Run one catch-up pass after 1.5s for cold-start threads, then stop */
		if (g_kfas.daily_pass == 1) {
			mod_delayed_work(g_kfas.wq, &g_kfas.daily_work,
					 msecs_to_jiffies(KFAS_DAILY_CATCHUP_MS));
		}
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
}

int kfas_daily_start_locked(pid_t pid)
{
	struct task_struct *leader;

	if (!g_kfas.enabled || !READ_ONCE(g_kfas.daily_enable) ||
	    g_kfas.session_active)
		return 0;

	if (g_kfas.daily_active && READ_ONCE(g_kfas.daily_pid) == pid)
		return 0;

	kfas_daily_stop_locked();

	rcu_read_lock();
	leader = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (!leader || !pid_alive(leader))
		leader = NULL;
	rcu_read_unlock();

	if (!leader)
		return -ESRCH;

	mutex_lock(&g_kfas.ctrl_mutex);
	atomic_set(&g_kfas.exiting_daily_pid, 0);
	WRITE_ONCE(g_kfas.daily_pid, pid);
	g_kfas.daily_pass = 0;
	WRITE_ONCE(g_kfas.daily_active, true);
	mutex_unlock(&g_kfas.ctrl_mutex);

	mod_delayed_work(g_kfas.wq, &g_kfas.daily_work, 0);
	return 0;
}

void kfas_daily_stop_locked(void)
{
	if (!READ_ONCE(g_kfas.daily_active) && !READ_ONCE(g_kfas.daily_pid) &&
	    g_kfas.nr_daily_managed == 0)
		return;

	mutex_lock(&g_kfas.ctrl_mutex);
	WRITE_ONCE(g_kfas.daily_active, false);
	WRITE_ONCE(g_kfas.daily_pid, 0);
	g_kfas.daily_pass = 0;
	mutex_unlock(&g_kfas.ctrl_mutex);

	cancel_delayed_work_sync(&g_kfas.daily_work);

	mutex_lock(&g_kfas.ctrl_mutex);
	kfas_daily_restore_all_locked();
	mutex_unlock(&g_kfas.ctrl_mutex);
}

void kfas_daily_init(void)
{
	g_daily_scan = kcalloc(KFAS_DAILY_MAX_SCAN, sizeof(*g_daily_scan),
			       GFP_KERNEL);
	g_daily_next = kcalloc(KFAS_DAILY_MAX_MANAGED, sizeof(*g_daily_next),
			       GFP_KERNEL);
}

ssize_t kfas_daily_format_threads(char *buf, size_t size)
{
	ssize_t len = 0;
	u32 i;

	mutex_lock(&g_kfas.ctrl_mutex);
	len += scnprintf(buf + len, size - len,
			 "topo: prime=%d high_mid=%d low_mid=%d little=%d daily_pid=%d walt_bridge=%d\n",
			 g_kfas.topo.prime_cpu, g_kfas.topo.nr_high_mid,
			 g_kfas.topo.nr_low_mid, g_kfas.topo.nr_little,
			 READ_ONCE(g_kfas.daily_pid),
			 smp_load_acquire(&g_kfas.walt_ops) ? 1 : 0);
	len += scnprintf(buf + len, size - len,
			 "tid\ttier\tassign\tactual\tdemand\texec_us\tuclamp\tcomm\n");

	for (i = 0; i < g_kfas.nr_daily_managed && len < (ssize_t)size - 96; i++) {
		const struct kfas_managed_tid *m = &g_kfas.daily_managed[i];
		int live_cpu = m->actual_cpu;

		if (m->pid_ref) {
			struct task_struct *t;

			rcu_read_lock();
			t = pid_task(m->pid_ref, PIDTYPE_PID);
			if (t)
				live_cpu = task_cpu(t);
			rcu_read_unlock();
		}

		len += scnprintf(buf + len, size - len,
				 "%d\tT%u\t%d\t%d\t%u\t%u\t%u..%u\t%s\n",
				 m->tid, m->tier, m->assigned_cpu,
				 live_cpu, m->demand, m->delta_exec_us,
				 m->uclamp_min, m->uclamp_max, m->comm);
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
	return len;
}
