// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Precision Thread Placement Engine
 *
 * Classifies active game threads into 3 tiers:
 *   Tier 0: Render thread (directly identified by Surface::queueBuffer TID)
 *   Tier 1: Top 1-2 primary logic/render threads (WALT demand + exec delta +
 *           sched_waking link to Tier 0 + optional vip_comms)
 *   Tier 2: Low-demand auxiliary worker threads (optional uclamp_max isolation)
 *
 * Routes Tier 0/1 natively through WALT's PIPELINE_FASTPATH + MVP priority on
 * microarchitecture-aware cores (Cortex-X3 prime + Cortex-A715 high-IPC gold
 * on SM8550) while leaving non-game system scheduling 100% stock.
 */
#include <linux/arch_topology.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/walt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/topology.h>
#include <trace/events/sched.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>
#ifdef CONFIG_ARM64
#include <asm/cpu.h>
#include <asm/cputype.h>
#endif

#include "kfas_internal.h"

#define KFAS_PLACE_PERIOD_MS	120
#define KFAS_WALT_BOOST_NS	(300ULL * NSEC_PER_MSEC)
#define ARM_PART_CORTEX_A715	0xD4D
#define ARM_PART_CORTEX_A710	0xD47
#define ARM_PART_CORTEX_X3	0xD4E

struct kfas_scan_entry {
	pid_t tid;
	char comm[TASK_COMM_LEN];
	u32 demand;
	u64 exec_ns;
	u32 delta_exec_us;
	u32 wake_hits;
	u32 score;
	int policy;
	int cur_cpu;
	bool is_vip;
};

struct kfas_prev_exec {
	pid_t tid;
	u64 exec_ns;
};

static struct kfas_scan_entry *g_scan_buf;
static struct kfas_managed_tid *g_next_managed;
static struct kfas_prev_exec *g_prev_exec;
static u32 g_nr_prev_exec;
static u64 g_last_place_ns;

static void kfas_set_core_ctl_boost_locked(bool want)
{
	const struct kfas_walt_ops *ops;

	if (want == g_kfas.core_ctl_boosted)
		return;

	ops = smp_load_acquire(&g_kfas.walt_ops);
	if (ops && ops->set_core_ctl_boost) {
		if (ops->set_core_ctl_boost(want) == 0)
			g_kfas.core_ctl_boosted = want;
	}
}

static void kfas_detect_topology(void)
{
	int cpu;
	unsigned long max_cap = 0, min_cap = ULONG_MAX;
	struct kfas_topo *t = &g_kfas.topo;

	memset(t, 0, sizeof(*t));
	t->prime_cpu = -1;

	for_each_possible_cpu(cpu) {
		unsigned long cap = arch_scale_cpu_capacity(cpu);

		if (cap > max_cap)
			max_cap = cap;
		if (cap < min_cap)
			min_cap = cap;
	}

	for_each_possible_cpu(cpu) {
		unsigned long cap = arch_scale_cpu_capacity(cpu);
		u32 part = 0;

#ifdef CONFIG_ARM64
		part = MIDR_PARTNUM((u32)per_cpu(cpu_data, cpu).reg_midr);
#endif
		if (cap == max_cap && max_cap > min_cap && t->prime_cpu < 0) {
			t->prime_cpu = cpu;
		} else if (cap == min_cap && max_cap > min_cap) {
			if (t->nr_little < NR_CPUS)
				t->little_cpus[t->nr_little++] = cpu;
		} else {
			/*
			 * Mid cluster: on SM8550, cpu3-4 are Cortex-A715 (0xD4D)
			 * and cpu5-6 are Cortex-A710 (0xD47). Prefer A715 first
			 * for Tier 0/1 critical threads.
			 */
			if (part == ARM_PART_CORTEX_A715) {
				if (t->nr_high_mid < NR_CPUS)
					t->high_mid_cpus[t->nr_high_mid++] = cpu;
			} else if (part == ARM_PART_CORTEX_A710) {
				if (t->nr_low_mid < NR_CPUS)
					t->low_mid_cpus[t->nr_low_mid++] = cpu;
			} else {
				if (t->nr_high_mid < NR_CPUS)
					t->high_mid_cpus[t->nr_high_mid++] = cpu;
			}
		}
	}

	/* Fallback if mid cluster had no A715 partnum distinction */
	if (t->nr_high_mid == 0 && t->nr_low_mid > 0) {
		int i;

		for (i = 0; i < t->nr_low_mid; i++)
			t->high_mid_cpus[t->nr_high_mid++] = t->low_mid_cpus[i];
		t->nr_low_mid = 0;
	}

	t->ready = true;
	pr_info("kfas: topology prime=%d high_mid=%d low_mid=%d little=%d\n",
		t->prime_cpu, t->nr_high_mid, t->nr_low_mid, t->nr_little);
}

static void kfas_tp_sched_waking(void *data, struct task_struct *p)
{
	pid_t tgid = READ_ONCE(g_kfas.target_pid);
	pid_t rtid = READ_ONCE(g_kfas.render_tid);
	pid_t waker;
	u64 now;
	unsigned long flags;
	int i, free_idx = -1, min_idx = 0;
	u32 min_cnt = U32_MAX;

	if (!tgid || !rtid)
		return;
	if (p->pid != rtid)
		return;
	if (current->tgid != tgid || current->pid == rtid)
		return;

	waker = current->pid;
	now = ktime_get_mono_fast_ns();

	raw_spin_lock_irqsave(&g_kfas.wake_lock, flags);
	for (i = 0; i < KFAS_WAKE_SLOTS; i++) {
		if (g_kfas.wake_slots[i].tid == waker) {
			if (g_kfas.wake_slots[i].count < U32_MAX - 1)
				g_kfas.wake_slots[i].count++;
			g_kfas.wake_slots[i].last_ts_ns = now;
			goto out;
		}
		if (g_kfas.wake_slots[i].tid == 0 && free_idx < 0)
			free_idx = i;
		if (g_kfas.wake_slots[i].count < min_cnt) {
			min_cnt = g_kfas.wake_slots[i].count;
			min_idx = i;
		}
	}
	i = free_idx >= 0 ? free_idx : min_idx;
	g_kfas.wake_slots[i].tid = waker;
	g_kfas.wake_slots[i].count = 1;
	g_kfas.wake_slots[i].last_ts_ns = now;
out:
	raw_spin_unlock_irqrestore(&g_kfas.wake_lock, flags);
}

static bool kfas_is_vip_comm_locked(const char *comm)
{
	u32 i;

	for (i = 0; i < g_kfas.nr_vip_comms; i++) {
		size_t len = strlen(g_kfas.vip_comms[i]);

		if (len > 0 && strncmp(comm, g_kfas.vip_comms[i], len) == 0)
			return true;
	}
	return false;
}

static bool kfas_is_skip_worker_comm(const char *comm)
{
	/* Never throttle binder/RT/audio/rendering helper threads */
	return strncmp(comm, "Binder:", 7) == 0 ||
	       strncmp(comm, "binder:", 7) == 0 ||
	       strncmp(comm, "HwBinder:", 9) == 0 ||
	       strncmp(comm, "RenderThread", 12) == 0 ||
	       strncmp(comm, "Audio", 5) == 0;
}

static struct task_struct *kfas_get_task_by_tid(pid_t tid)
{
	struct task_struct *t;

	rcu_read_lock();
	t = pid_task(find_vpid(tid), PIDTYPE_PID);
	if (t)
		get_task_struct(t);
	rcu_read_unlock();
	return t;
}

static bool kfas_apply_task_uclamp(struct task_struct *task, u32 umin, u32 umax)
{
	struct sched_attr attr;

	/* Strictly leave RT / DL threads untouched */
	if (task->policy != SCHED_NORMAL && task->policy != SCHED_BATCH)
		return false;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy = (u32)task->policy;
	attr.sched_nice = task_nice(task);
	attr.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP;
	attr.sched_util_min = min_t(u32, 1024, umin);
	attr.sched_util_max = clamp_t(u32, umax, attr.sched_util_min, 1024);

	return sched_setattr_nocheck(task, &attr) == 0;
}

static void kfas_restore_managed_entry_locked(struct kfas_managed_tid *m)
{
	const struct kfas_walt_ops *ops = smp_load_acquire(&g_kfas.walt_ops);
	struct task_struct *task;

	if (!m->tid || (!m->walt_pinned && !m->uclamp_applied))
		return;

	task = kfas_get_task_by_tid(m->tid);
	if (task) {
		if (m->walt_pinned && ops && ops->clear_task_placement)
			ops->clear_task_placement(task);
		if (m->uclamp_applied)
			kfas_apply_task_uclamp(task, 0, 1024);
		put_task_struct(task);
	}

	m->walt_pinned = false;
	m->uclamp_applied = false;
	m->assigned_cpu = -1;
	m->uclamp_min = 0;
	m->uclamp_max = 1024;
}

static void kfas_restore_all_managed_locked(void)
{
	u32 i;

	for (i = 0; i < g_kfas.nr_managed; i++)
		kfas_restore_managed_entry_locked(&g_kfas.managed[i]);
	g_kfas.nr_managed = 0;
}

static u64 kfas_find_prev_exec_locked(pid_t tid)
{
	u32 i;

	for (i = 0; i < g_nr_prev_exec; i++) {
		if (g_prev_exec[i].tid == tid)
			return g_prev_exec[i].exec_ns;
	}
	return 0;
}

static u32 kfas_take_wake_hits(pid_t tid)
{
	unsigned long flags;
	u32 hits = 0;
	int i;

	raw_spin_lock_irqsave(&g_kfas.wake_lock, flags);
	for (i = 0; i < KFAS_WAKE_SLOTS; i++) {
		if (g_kfas.wake_slots[i].tid == tid) {
			hits = g_kfas.wake_slots[i].count;
			g_kfas.wake_slots[i].count = hits / 2;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_kfas.wake_lock, flags);
	return hits;
}

static int kfas_pick_crit_cpu_locked(int rank, bool allow_prime, u32 demand)
{
	const struct kfas_topo *t = &g_kfas.topo;

	if (!t->ready)
		return -1;

	/*
	 * Rank 0 (heaviest frame thread): prefer high-IPC efficiency Gold
	 * (Cortex-A715 #0) for light/moderate workloads; escalate to Prime X3
	 * only when single-thread load, perf pressure, or real jank warrants X3.
	 */
	if (rank == 0) {
		u32 dem_bar = 640;
		u32 perf_bar = 780;
		u32 jank_bar = 2;

		if (g_kfas.mode == KFAS_MODE_FAST) {
			dem_bar = 450;
			perf_bar = 620;
			jank_bar = 1;
		} else if (g_kfas.mode == KFAS_MODE_PERFORMANCE) {
			dem_bar = 540;
			perf_bar = 700;
			jank_bar = 2;
		}

		if (allow_prime && t->prime_cpu >= 0 &&
		    (demand >= dem_bar || g_kfas.perf_level >= perf_bar ||
		     g_kfas.jank_streak >= jank_bar))
			return t->prime_cpu;
		if (t->nr_high_mid > 0)
			return t->high_mid_cpus[0];
		return t->prime_cpu;
	}

	/* Rank 1 (second critical thread): A715 #0 (or A715 #1 if #0 taken) */
	if (rank == 1) {
		if (t->nr_high_mid > 0)
			return t->high_mid_cpus[0];
		if (t->nr_low_mid > 0)
			return t->low_mid_cpus[0];
		return -1;
	}

	/* Rank 2 (third critical thread): A715 #1, then A710 #0 */
	if (t->nr_high_mid > 1)
		return t->high_mid_cpus[1];
	if (t->nr_low_mid > 0)
		return t->low_mid_cpus[0];
	return -1;
}

static void kfas_place_pass_locked(bool urgent)
{
	const struct kfas_walt_ops *ops = smp_load_acquire(&g_kfas.walt_ops);
	const struct kfas_mode_cfg *cfg = &g_kfas.mode_cfg[g_kfas.mode];
	pid_t tgid = READ_ONCE(g_kfas.target_pid);
	pid_t rtid = g_kfas.render_tid;
	struct task_struct *leader, *t;
	u32 nr_scan = 0, i, j;
	int crit_idx[KFAS_MAX_CRITICAL];
	u32 nr_crit = 0;
	cpumask_t used_cpus;
	u32 nr_next = 0;
	u64 now_ns, pass_dt_us = 0;
	u32 max_busy = 0;
	bool have_prev;
	bool used_prime = false;
	bool want_boost;

	if (!g_kfas.session_active || !tgid || !g_kfas.rendering_active ||
	    !g_kfas.placement_enable || !g_scan_buf || !g_next_managed ||
	    !g_prev_exec)
		return;

	if (!g_kfas.topo.ready)
		kfas_detect_topology();

	/* Phase 1: Snapshot thread group under RCU (no sleeping!) */
	rcu_read_lock();
	leader = pid_task(find_vpid(tgid), PIDTYPE_PID);
	if (leader && pid_alive(leader)) {
		for_each_thread(leader, t) {
			struct kfas_scan_entry *e;

			if (nr_scan >= KFAS_MAX_SCAN_THREADS)
				break;
			if (READ_ONCE(t->__state) == TASK_DEAD)
				continue;

			e = &g_scan_buf[nr_scan++];
			e->tid = t->pid;
			get_task_comm(e->comm, t);
			e->demand = (ops && ops->get_task_demand_scaled) ?
				    ops->get_task_demand_scaled(t) : 0;
			e->exec_ns = READ_ONCE(t->se.sum_exec_runtime);
			e->policy = t->policy;
			e->cur_cpu = task_cpu(t);
			e->delta_exec_us = 0;
			e->wake_hits = 0;
			e->score = 0;
			e->is_vip = kfas_is_vip_comm_locked(e->comm);
		}
	}
	rcu_read_unlock();

	if (nr_scan == 0)
		return;

	/* Phase 2: Score threads by exec delta + WALT demand + wake-link */
	now_ns = ktime_get_mono_fast_ns();
	have_prev = (g_nr_prev_exec > 0 && g_last_place_ns > 0 &&
		     now_ns > g_last_place_ns);
	if (have_prev)
		pass_dt_us = div_u64(now_ns - g_last_place_ns, 1000U);
	g_last_place_ns = now_ns;

	for (i = 0; i < nr_scan; i++) {
		struct kfas_scan_entry *e = &g_scan_buf[i];
		u64 prev_ns = kfas_find_prev_exec_locked(e->tid);

		if (prev_ns > 0 && e->exec_ns >= prev_ns)
			e->delta_exec_us = (u32)div_u64(
				e->exec_ns - prev_ns, 1000U);

		e->wake_hits = kfas_take_wake_hits(e->tid);
		/* Score blends WALT demand (0..1024), active exec_ms*8, wake-link */
		e->score = e->demand + min_t(u32, 1000, e->delta_exec_us / 125) +
			   min_t(u32, 350, e->wake_hits * 25) +
			   (e->is_vip ? 400 : 0);
	}

	/* Save exec snapshots for next pass across all threads */
	g_nr_prev_exec = nr_scan;
	for (i = 0; i < nr_scan; i++) {
		g_prev_exec[i].tid = g_scan_buf[i].tid;
		g_prev_exec[i].exec_ns = g_scan_buf[i].exec_ns;
	}

	/* Pick Tier 0 (render_tid) first */
	for (i = 0; i < KFAS_MAX_CRITICAL; i++)
		crit_idx[i] = -1;

	if (rtid > 0) {
		for (i = 0; i < nr_scan; i++) {
			if (g_scan_buf[i].tid == rtid) {
				crit_idx[nr_crit++] = (int)i;
				break;
			}
		}
	}

	/* Pick up to 2 heaviest / wake-linked Tier 1 threads */
	while (nr_crit < KFAS_MAX_CRITICAL) {
		int best = -1;
		u32 best_score = 160; /* Minimum activity bar for Tier 1 */

		for (i = 0; i < nr_scan; i++) {
			bool already = false;

			for (j = 0; j < nr_crit; j++) {
				if (crit_idx[j] == (int)i) {
					already = true;
					break;
				}
			}
			if (already)
				continue;
			if (g_scan_buf[i].policy != SCHED_NORMAL &&
			    g_scan_buf[i].policy != SCHED_BATCH)
				continue;
			/* Skip sleeping threads that did not run in the last pass */
			if (have_prev && g_scan_buf[i].delta_exec_us < 500 &&
			    g_scan_buf[i].wake_hits == 0 &&
			    !g_scan_buf[i].is_vip)
				continue;

			if (g_scan_buf[i].score > best_score) {
				best_score = g_scan_buf[i].score;
				best = (int)i;
			}
		}
		if (best < 0)
			break;
		crit_idx[nr_crit++] = best;
	}

	/* Sort chosen critical threads by score descending so heaviest gets X3 */
	for (i = 0; i < nr_crit; i++) {
		for (j = i + 1; j < nr_crit; j++) {
			if (g_scan_buf[crit_idx[j]].score >
			    g_scan_buf[crit_idx[i]].score) {
				int tmp = crit_idx[i];

				crit_idx[i] = crit_idx[j];
				crit_idx[j] = tmp;
			}
		}
	}

	/* Phase 3: Apply placements safely (sleeping allowed outside RCU) */
	cpumask_clear(&used_cpus);
	memset(g_next_managed, 0,
	       KFAS_MAX_MANAGED_TIDS * sizeof(*g_next_managed));

	for (i = 0; i < nr_crit && nr_next < KFAS_MAX_MANAGED_TIDS; i++) {
		struct kfas_scan_entry *e = &g_scan_buf[crit_idx[i]];
		struct kfas_managed_tid *m = &g_next_managed[nr_next++];
		struct task_struct *task;
		int tgt_cpu;
		u32 umin;
		int boost;

		tgt_cpu = kfas_pick_crit_cpu_locked((int)i, cfg->prime_enable,
						    e->demand);
		/* Ensure no two critical threads target the exact same CPU */
		if (tgt_cpu >= 0 && cpumask_test_cpu(tgt_cpu, &used_cpus)) {
			u32 k;

			tgt_cpu = -1;
			for (k = 0; k < (u32)g_kfas.topo.nr_high_mid; k++) {
				int c = g_kfas.topo.high_mid_cpus[k];

				if (!cpumask_test_cpu(c, &used_cpus)) {
					tgt_cpu = c;
					break;
				}
			}
			if (tgt_cpu < 0) {
				for (k = 0; k < (u32)g_kfas.topo.nr_low_mid; k++) {
					int c = g_kfas.topo.low_mid_cpus[k];

					if (!cpumask_test_cpu(c, &used_cpus)) {
						tgt_cpu = c;
						break;
					}
				}
			}
		}
		if (tgt_cpu >= 0)
			cpumask_set_cpu(tgt_cpu, &used_cpus);
		if (tgt_cpu == g_kfas.topo.prime_cpu && tgt_cpu >= 0) {
			used_prime = true;
			if (g_kfas.unpause_big && cfg->prime_enable)
				kfas_set_core_ctl_boost_locked(true);
		}

		/* Dynamic per-task uclamp_min scaled gently with frame pressure */
		umin = cfg->crit_uclamp_min +
		       (g_kfas.perf_level * 160) / 1000;
		if (urgent || g_kfas.jank_streak >= 2)
			umin = min_t(u32, 920, umin + 140);
		if (tgt_cpu == g_kfas.topo.prime_cpu && cfg->prime_enable &&
		    (g_kfas.perf_level >= 750 || g_kfas.jank_streak >= 2))
			umin = max_t(u32, umin, 560);
		umin = clamp_t(u32, umin, 280, 960);

#if IS_ENABLED(CONFIG_SCHED_WALT)
		boost = (urgent || g_kfas.jank_streak >= 2) ?
			(tgt_cpu == g_kfas.topo.prime_cpu ?
			 TASK_BOOST_ON_MAX : TASK_BOOST_ON_MID) :
			TASK_BOOST_NONE;
#else
		boost = 0;
#endif

		m->tid = e->tid;
		strscpy(m->comm, e->comm, sizeof(m->comm));
		m->tier = (e->tid == rtid) ? 0 : 1;
		m->assigned_cpu = tgt_cpu;
		m->actual_cpu = e->cur_cpu;
		m->demand = e->demand;
		m->last_exec_ns = e->exec_ns;
		m->delta_exec_us = e->delta_exec_us;
		m->uclamp_min = umin;
		m->uclamp_max = 1024;
		if (have_prev && pass_dt_us >= 20000) {
			u32 busy = (u32)min_t(u64, 100ULL,
					      div64_u64((u64)e->delta_exec_us * 100ULL,
							pass_dt_us));

			if (busy > max_busy)
				max_busy = busy;
		}

		task = kfas_get_task_by_tid(e->tid);
		if (task) {
			if (ops && ops->set_task_placement &&
			    ops->set_task_placement(task, tgt_cpu, true,
						    boost,
						    KFAS_WALT_BOOST_NS) == 0)
				m->walt_pinned = true;
			if (kfas_apply_task_uclamp(task, umin, 1024))
				m->uclamp_applied = true;
			put_task_struct(task);
		}
	}

	if (have_prev && pass_dt_us >= 20000)
		g_kfas.max_crit_busy_pct = max_busy;

	g_kfas.prime_in_use = used_prime;
	want_boost = g_kfas.unpause_big && cfg->prime_enable &&
		     (used_prime || g_kfas.mode >= KFAS_MODE_PERFORMANCE ||
		      g_kfas.perf_level >= 720 || g_kfas.jank_streak >= 2);
	kfas_set_core_ctl_boost_locked(want_boost);

	/* Tier 2: Gently contain low-demand worker threads off X3/A715 */
	if (g_kfas.contain_workers && cfg->worker_uclamp_max > 0 &&
	    cfg->worker_uclamp_max < 1024) {
		for (i = 0; i < nr_scan && nr_next < KFAS_MAX_MANAGED_TIDS; i++) {
			struct kfas_scan_entry *e = &g_scan_buf[i];
			struct kfas_managed_tid *m;
			struct task_struct *task;
			bool is_crit = false;

			for (j = 0; j < nr_crit; j++) {
				if (crit_idx[j] == (int)i) {
					is_crit = true;
					break;
				}
			}
			if (is_crit)
				continue;
			if (e->policy != SCHED_NORMAL)
				continue;
			if (e->is_vip || kfas_is_skip_worker_comm(e->comm))
				continue;
			/* Release workers that become legitimately busy */
			if (e->demand >= 180 || e->delta_exec_us >= 20000)
				continue;

			m = &g_next_managed[nr_next++];
			m->tid = e->tid;
			strscpy(m->comm, e->comm, sizeof(m->comm));
			m->tier = 2;
			m->assigned_cpu = -1;
			m->actual_cpu = e->cur_cpu;
			m->demand = e->demand;
			m->last_exec_ns = e->exec_ns;
			m->delta_exec_us = e->delta_exec_us;
			m->uclamp_min = 0;
			m->uclamp_max = cfg->worker_uclamp_max;

			task = kfas_get_task_by_tid(e->tid);
			if (task) {
				if (kfas_apply_task_uclamp(task, 0,
							   cfg->worker_uclamp_max))
					m->uclamp_applied = true;
				put_task_struct(task);
			}
		}
	}

	/* Phase 4: Restore any previously managed TID no longer in g_next_managed */
	for (i = 0; i < g_kfas.nr_managed; i++) {
		bool kept = false;

		for (j = 0; j < nr_next; j++) {
			if (g_next_managed[j].tid == g_kfas.managed[i].tid) {
				/*
				 * If a thread moved from Tier 0/1 to Tier 2,
				 * clear its WALT pipeline/MVP flag explicitly.
				 */
				if (g_kfas.managed[i].walt_pinned &&
				    !g_next_managed[j].walt_pinned) {
					struct task_struct *task =
						kfas_get_task_by_tid(
							g_kfas.managed[i].tid);

					if (task) {
						if (ops &&
						    ops->clear_task_placement)
							ops->clear_task_placement(task);
						put_task_struct(task);
					}
				}
				kept = true;
				break;
			}
		}
		if (!kept)
			kfas_restore_managed_entry_locked(&g_kfas.managed[i]);
	}

	memcpy(g_kfas.managed, g_next_managed,
	       nr_next * sizeof(struct kfas_managed_tid));
	g_kfas.nr_managed = nr_next;
}

void kfas_place_work_fn(struct work_struct *work)
{
	mutex_lock(&g_kfas.ctrl_mutex);
	if (g_kfas.session_active && g_kfas.rendering_active &&
	    g_kfas.placement_enable) {
		kfas_place_pass_locked(false);
		queue_delayed_work(g_kfas.wq, &g_kfas.place_work,
				   msecs_to_jiffies(KFAS_PLACE_PERIOD_MS));
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
}

void kfas_place_boost_urgent_locked(void)
{
	kfas_place_pass_locked(true);
}

void kfas_place_session_start_locked(void)
{
	unsigned long flags;

	g_nr_prev_exec = 0;
	g_last_place_ns = 0;
	g_kfas.max_crit_busy_pct = 0;
	g_kfas.prime_in_use = false;
	raw_spin_lock_irqsave(&g_kfas.wake_lock, flags);
	memset(g_kfas.wake_slots, 0, sizeof(g_kfas.wake_slots));
	raw_spin_unlock_irqrestore(&g_kfas.wake_lock, flags);

	if (!g_kfas.waking_tp_active) {
		if (register_trace_sched_waking(kfas_tp_sched_waking, NULL) == 0)
			g_kfas.waking_tp_active = true;
	}
}

void kfas_place_idle_relax_locked(void)
{
	g_last_place_ns = 0;
	g_kfas.max_crit_busy_pct = 0;
	g_kfas.prime_in_use = false;
	kfas_restore_all_managed_locked();
	kfas_set_core_ctl_boost_locked(false);
}

void kfas_place_session_stop_locked(void)
{
	if (g_kfas.waking_tp_active) {
		unregister_trace_sched_waking(kfas_tp_sched_waking, NULL);
		g_kfas.waking_tp_active = false;
	}
	g_nr_prev_exec = 0;
	g_last_place_ns = 0;
	g_kfas.max_crit_busy_pct = 0;
	g_kfas.prime_in_use = false;
	kfas_restore_all_managed_locked();
	kfas_set_core_ctl_boost_locked(false);
}

void kfas_place_init(void)
{
	raw_spin_lock_init(&g_kfas.wake_lock);
	g_scan_buf = kcalloc(KFAS_MAX_SCAN_THREADS, sizeof(*g_scan_buf),
			     GFP_KERNEL);
	g_next_managed = kcalloc(KFAS_MAX_MANAGED_TIDS, sizeof(*g_next_managed),
				 GFP_KERNEL);
	g_prev_exec = kcalloc(KFAS_MAX_SCAN_THREADS, sizeof(*g_prev_exec),
			      GFP_KERNEL);
	kfas_detect_topology();
}

ssize_t kfas_place_format_threads(char *buf, size_t size)
{
	ssize_t len = 0;
	u32 i;

	mutex_lock(&g_kfas.ctrl_mutex);
	len += scnprintf(buf + len, size - len,
			 "topo: prime=%d high_mid=%d low_mid=%d little=%d core_ctl_boost=%d walt_bridge=%d\n",
			 g_kfas.topo.prime_cpu, g_kfas.topo.nr_high_mid,
			 g_kfas.topo.nr_low_mid, g_kfas.topo.nr_little,
			 g_kfas.core_ctl_boosted ? 1 : 0,
			 smp_load_acquire(&g_kfas.walt_ops) ? 1 : 0);
	len += scnprintf(buf + len, size - len,
			 "tid\ttier\tassign\tactual\tdemand\texec_us\tuclamp\tcomm\n");

	for (i = 0; i < g_kfas.nr_managed && len < (ssize_t)size - 96; i++) {
		const struct kfas_managed_tid *m = &g_kfas.managed[i];

		len += scnprintf(buf + len, size - len,
				 "%d\tT%u\t%d\t%d\t%u\t%u\t%u..%u\t%s\n",
				 m->tid, m->tier, m->assigned_cpu,
				 m->actual_cpu, m->demand, m->delta_exec_us,
				 m->uclamp_min, m->uclamp_max, m->comm);
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
	return len;
}
