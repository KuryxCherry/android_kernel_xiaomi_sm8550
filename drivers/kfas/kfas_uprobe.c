// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Dual-Probe Surface Frame Telemetry
 *
 * Uses native kernel uprobe_register() filtered to the active game mm_struct
 * on Surface::queueBuffer and Surface::dequeueBuffer (offsets supplied via
 * sysfs so Android library upgrades require zero kernel changes).
 */
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/namei.h>
#include <linux/sched/mm.h>
#include <linux/string.h>
#include <linux/uprobes.h>
#include <asm/ptrace.h>

#include "kfas_internal.h"

#define KFAS_MIN_FRAME_NS	(2ULL * NSEC_PER_MSEC)
#define KFAS_MAX_FRAME_NS	(250ULL * NSEC_PER_MSEC)
#define KFAS_SURFACE_STALE_NS	(1000ULL * NSEC_PER_MSEC)

static inline u64 kfas_reg_arg0(struct pt_regs *regs)
{
#ifdef CONFIG_ARM64
	return regs->regs[0];
#else
	return 0;
#endif
}

static inline u64 kfas_task_run_delay(struct task_struct *p)
{
#ifdef CONFIG_SCHED_INFO
	return READ_ONCE(p->sched_info.run_delay);
#else
	return 0;
#endif
}

static bool kfas_uprobe_filter(struct uprobe_consumer *uc,
			       enum uprobe_filter_ctx ctx,
			       struct mm_struct *mm)
{
	struct mm_struct *target = READ_ONCE(g_kfas.target_mm);

	return target && mm == target;
}

static int kfas_dq_enter_handler(struct uprobe_consumer *uc,
				 struct pt_regs *regs)
{
	pid_t tgid = READ_ONCE(g_kfas.target_pid);
	pid_t tid;
	u64 now;
	unsigned long flags;
	int i, free_idx = -1, oldest_idx = 0;
	u64 oldest_ts = U64_MAX;

	if (!tgid || current->tgid != tgid)
		return 0;

	tid = current->pid;
	now = ktime_get_mono_fast_ns();

	raw_spin_lock_irqsave(&g_kfas.dq_lock, flags);
	for (i = 0; i < KFAS_DQ_SLOTS; i++) {
		if (g_kfas.dq_slots[i].tid == tid) {
			g_kfas.dq_slots[i].enter_ns = now;
			goto out;
		}
		if (g_kfas.dq_slots[i].tid == 0 && free_idx < 0)
			free_idx = i;
		if (g_kfas.dq_slots[i].enter_ns < oldest_ts) {
			oldest_ts = g_kfas.dq_slots[i].enter_ns;
			oldest_idx = i;
		}
	}
	i = free_idx >= 0 ? free_idx : oldest_idx;
	g_kfas.dq_slots[i].tid = tid;
	g_kfas.dq_slots[i].enter_ns = now;
	g_kfas.dq_slots[i].last_wait_ns = 0;
	g_kfas.dq_slots[i].wait_ts_ns = 0;
out:
	raw_spin_unlock_irqrestore(&g_kfas.dq_lock, flags);
	return 0;
}

static int kfas_dq_ret_handler(struct uprobe_consumer *uc,
			       unsigned long func, struct pt_regs *regs)
{
	pid_t tgid = READ_ONCE(g_kfas.target_pid);
	pid_t rtid = READ_ONCE(g_kfas.render_tid);
	pid_t tid;
	u64 now, wait_ns, exec_now;
	unsigned long flags;
	bool arm_pre = false;
	int i;

	if (!tgid || current->tgid != tgid)
		return 0;

	tid = current->pid;
	now = ktime_get_mono_fast_ns();
	exec_now = READ_ONCE(current->se.sum_exec_runtime);

	raw_spin_lock_irqsave(&g_kfas.dq_lock, flags);
	for (i = 0; i < KFAS_DQ_SLOTS; i++) {
		if (g_kfas.dq_slots[i].tid == tid &&
		    g_kfas.dq_slots[i].enter_ns > 0 &&
		    now >= g_kfas.dq_slots[i].enter_ns) {
			wait_ns = now - g_kfas.dq_slots[i].enter_ns;
			if (wait_ns <= KFAS_MAX_FRAME_NS) {
				g_kfas.dq_slots[i].last_wait_ns = wait_ns;
				g_kfas.dq_slots[i].wait_ts_ns = now;
			}
			g_kfas.dq_slots[i].enter_ns = 0;
			break;
		}
	}
	/*
	 * Returning from dequeueBuffer marks the true CPU render start for
	 * frame N. Anchor pre-deadline rescue on the dominant render thread.
	 */
	if (rtid > 0 && tid == rtid && READ_ONCE(g_kfas.rendering_active)) {
		g_kfas.dq_start_ts_ns = now;
		g_kfas.dq_start_exec_ns = exec_now;
		g_kfas.dq_start_tid = tid;
		g_kfas.dq_start_seq = (u32)atomic_read(&g_kfas.frame_seq);
		arm_pre = true;
	}
	raw_spin_unlock_irqrestore(&g_kfas.dq_lock, flags);

	if (arm_pre) {
		u32 fps = max_t(u32, 15U, READ_ONCE(g_kfas.locked_fps));
		u64 budget_ns = div_u64(NSEC_PER_SEC, fps);
		u64 pre_ns = clamp_t(u64, div_u64(budget_ns * 75U, 100U),
				     5500000ULL, 24000000ULL);

		hrtimer_start(&g_kfas.pre_rescue_timer, ns_to_ktime(pre_ns),
			      HRTIMER_MODE_REL);
	}
	return 0;
}

static u64 kfas_consume_dq_wait(pid_t tid, u64 now)
{
	unsigned long flags;
	u64 wait_ns = 0;
	int i;

	raw_spin_lock_irqsave(&g_kfas.dq_lock, flags);
	for (i = 0; i < KFAS_DQ_SLOTS; i++) {
		if (g_kfas.dq_slots[i].tid == tid) {
			if (g_kfas.dq_slots[i].wait_ts_ns > 0 &&
			    now >= g_kfas.dq_slots[i].wait_ts_ns &&
			    (now - g_kfas.dq_slots[i].wait_ts_ns) <
				KFAS_MAX_FRAME_NS)
				wait_ns = g_kfas.dq_slots[i].last_wait_ns;
			g_kfas.dq_slots[i].last_wait_ns = 0;
			g_kfas.dq_slots[i].wait_ts_ns = 0;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_kfas.dq_lock, flags);
	return wait_ns;
}

static int kfas_q_enter_handler(struct uprobe_consumer *uc,
				struct pt_regs *regs)
{
	pid_t tgid = READ_ONCE(g_kfas.target_pid);
	pid_t tid;
	u64 now, surface, exec_now, delay_now, dq_wait_ns;
	u64 frame_ns = 0, exec_delta = 0, delay_delta = 0;
	unsigned long flags;
	struct kfas_surface_slot *slot = NULL;
	int i, free_idx = -1, oldest_idx = 0;
	u64 oldest_ts = U64_MAX;
	u32 best_cnt = 0;
	u64 best_surface = 0;
	bool emit = false;

	if (!tgid || current->tgid != tgid)
		return 0;

	tid = current->pid;
	now = ktime_get_mono_fast_ns();
	surface = kfas_reg_arg0(regs);
	exec_now = READ_ONCE(current->se.sum_exec_runtime);
	delay_now = kfas_task_run_delay(current);
	dq_wait_ns = kfas_consume_dq_wait(tid, now);

	raw_spin_lock_irqsave(&g_kfas.frame_lock, flags);

	/* Prune stale surfaces & locate matching slot */
	for (i = 0; i < KFAS_SURFACE_SLOTS; i++) {
		struct kfas_surface_slot *s = &g_kfas.surfaces[i];

		if (s->surface && now > s->last_ts_ns &&
		    (now - s->last_ts_ns) > KFAS_SURFACE_STALE_NS) {
			memset(s, 0, sizeof(*s));
		}
		if (s->surface == surface) {
			slot = s;
		} else if (!s->surface && free_idx < 0) {
			free_idx = i;
		} else if (s->last_ts_ns < oldest_ts) {
			oldest_ts = s->last_ts_ns;
			oldest_idx = i;
		}
	}

	if (!slot) {
		slot = &g_kfas.surfaces[free_idx >= 0 ? free_idx : oldest_idx];
		memset(slot, 0, sizeof(*slot));
		slot->surface = surface;
	}

	if (slot->last_ts_ns > 0 && now > slot->last_ts_ns) {
		u64 dt = now - slot->last_ts_ns;

		if (dt >= KFAS_MIN_FRAME_NS && dt <= KFAS_MAX_FRAME_NS) {
			frame_ns = dt;
			if (slot->last_tid == tid) {
				if (exec_now >= slot->last_exec_ns)
					exec_delta = exec_now - slot->last_exec_ns;
				if (delay_now >= slot->last_delay_ns)
					delay_delta = delay_now - slot->last_delay_ns;
			}
			if (slot->count < U32_MAX - 1)
				slot->count++;
		} else if (dt > KFAS_MAX_FRAME_NS) {
			/* Resume after pause/loading screen: reset window cleanly */
			slot->count = 1;
		}
	} else {
		slot->count = 1;
	}

	slot->last_ts_ns = now;
	slot->last_exec_ns = exec_now;
	slot->last_delay_ns = delay_now;
	slot->last_tid = tid;

	/* Select dominant game surface (highest recent valid frame count) */
	for (i = 0; i < KFAS_SURFACE_SLOTS; i++) {
		if (g_kfas.surfaces[i].surface &&
		    g_kfas.surfaces[i].count > best_cnt) {
			best_cnt = g_kfas.surfaces[i].count;
			best_surface = g_kfas.surfaces[i].surface;
		}
	}
	if (best_surface)
		g_kfas.dominant_surface = best_surface;

	if (frame_ns > 0 && surface == g_kfas.dominant_surface) {
		u32 next_head = (g_kfas.ring_head + 1) & KFAS_RING_MASK;

		if (next_head == g_kfas.ring_tail) {
			/* Drop oldest unread sample to keep freshest telemetry */
			g_kfas.ring_tail = (g_kfas.ring_tail + 1) & KFAS_RING_MASK;
			g_kfas.frames_dropped_ring++;
		}
		g_kfas.ring[g_kfas.ring_head] = (struct kfas_frame_sample){
			.ts_ns = now,
			.frame_ns = frame_ns,
			.exec_ns = exec_delta,
			.delay_ns = delay_delta,
			.dq_wait_ns = dq_wait_ns,
			.tid = tid,
			.cpu = (u8)raw_smp_processor_id(),
		};
		g_kfas.ring_head = next_head;
		g_kfas.frames_total++;
		atomic_inc(&g_kfas.frame_seq);
		emit = true;
	}

	raw_spin_unlock_irqrestore(&g_kfas.frame_lock, flags);

	if (emit && g_kfas.wq)
		queue_work(g_kfas.wq, &g_kfas.frame_work);

	return 0;
}

void kfas_uprobe_reset_state(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&g_kfas.frame_lock, flags);
	g_kfas.ring_head = 0;
	g_kfas.ring_tail = 0;
	g_kfas.frames_total = 0;
	g_kfas.frames_dropped_ring = 0;
	atomic_set(&g_kfas.frame_seq, 0);
	g_kfas.dominant_surface = 0;
	memset(g_kfas.surfaces, 0, sizeof(g_kfas.surfaces));
	raw_spin_unlock_irqrestore(&g_kfas.frame_lock, flags);

	raw_spin_lock_irqsave(&g_kfas.dq_lock, flags);
	memset(g_kfas.dq_slots, 0, sizeof(g_kfas.dq_slots));
	g_kfas.dq_start_ts_ns = 0;
	g_kfas.dq_start_exec_ns = 0;
	g_kfas.dq_start_tid = 0;
	g_kfas.dq_start_seq = 0;
	raw_spin_unlock_irqrestore(&g_kfas.dq_lock, flags);
}

int kfas_uprobe_attach_locked(struct task_struct *leader)
{
	struct path lib_path;
	struct inode *inode;
	struct mm_struct *mm;
	u32 i, idx = 0;
	int ret;

	if (!g_kfas.lib_path[0] || g_kfas.nr_queue_offsets == 0) {
		pr_warn("kfas: probes not configured before attach\n");
		return -EINVAL;
	}

	mm = get_task_mm(leader);
	if (!mm)
		return -ESRCH;

	ret = kern_path(g_kfas.lib_path, LOOKUP_FOLLOW, &lib_path);
	if (ret) {
		pr_err("kfas: kern_path(%s) failed: %d\n",
		       g_kfas.lib_path, ret);
		mmput(mm);
		return ret;
	}

	inode = igrab(d_inode(lib_path.dentry));
	path_put(&lib_path);
	if (!inode || !S_ISREG(inode->i_mode)) {
		if (inode)
			iput(inode);
		mmput(mm);
		return -EINVAL;
	}

	kfas_uprobe_reset_state();

	/* Publish target_mm and target_pid BEFORE registering uprobes */
	WRITE_ONCE(g_kfas.target_mm, mm);
	WRITE_ONCE(g_kfas.target_pid, leader->tgid);
	g_kfas.probe_inode = inode;
	g_kfas.nr_probes = 0;
	memset(g_kfas.probes, 0, sizeof(g_kfas.probes));

	for (i = 0; i < g_kfas.nr_queue_offsets && idx < KFAS_MAX_PROBES; i++) {
		struct kfas_probe_slot *p = &g_kfas.probes[idx];

		p->kind = KFAS_PROBE_QUEUE;
		p->offset = g_kfas.queue_offsets[i];
		p->uc.handler = kfas_q_enter_handler;
		p->uc.filter = kfas_uprobe_filter;
		ret = uprobe_register(inode, p->offset, &p->uc);
		if (ret) {
			pr_err("kfas: uprobe_register queue@0x%llx failed: %d\n",
			       (unsigned long long)p->offset, ret);
			goto err_rollback;
		}
		p->registered = true;
		idx++;
	}

	for (i = 0; i < g_kfas.nr_dequeue_offsets && idx < KFAS_MAX_PROBES; i++) {
		struct kfas_probe_slot *p = &g_kfas.probes[idx];

		p->kind = KFAS_PROBE_DEQUEUE;
		p->offset = g_kfas.dequeue_offsets[i];
		p->uc.handler = kfas_dq_enter_handler;
		p->uc.ret_handler = kfas_dq_ret_handler;
		p->uc.filter = kfas_uprobe_filter;
		ret = uprobe_register(inode, p->offset, &p->uc);
		if (ret) {
			/* Dequeue probe is optional; log and continue if queue works */
			pr_warn("kfas: uprobe_register dequeue@0x%llx failed: %d (continuing)\n",
				(unsigned long long)p->offset, ret);
			memset(p, 0, sizeof(*p));
			continue;
		}
		p->registered = true;
		idx++;
	}

	g_kfas.nr_probes = idx;
	pr_debug("kfas: attached %u uprobes to pid %d (%s)\n",
		 idx, leader->tgid, g_kfas.lib_path);
	return 0;

err_rollback:
	g_kfas.nr_probes = idx;
	kfas_uprobe_detach_locked();
	return ret;
}

void kfas_uprobe_detach_locked(void)
{
	u32 i;
	struct inode *inode = g_kfas.probe_inode;
	struct mm_struct *mm = g_kfas.target_mm;

	/* Stop callbacks from recording new work immediately */
	WRITE_ONCE(g_kfas.target_pid, 0);

	if (inode) {
		for (i = 0; i < g_kfas.nr_probes; i++) {
			if (g_kfas.probes[i].registered) {
				uprobe_unregister(inode, g_kfas.probes[i].offset,
						  &g_kfas.probes[i].uc);
				g_kfas.probes[i].registered = false;
			}
		}
	}

	/* Safe to drop target_mm and inode ONLY after uprobe_unregister completes */
	WRITE_ONCE(g_kfas.target_mm, NULL);
	g_kfas.probe_inode = NULL;
	g_kfas.nr_probes = 0;

	if (mm)
		mmput(mm);
	if (inode)
		iput(inode);

	kfas_uprobe_reset_state();
}
