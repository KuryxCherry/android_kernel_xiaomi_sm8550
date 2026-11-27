// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Multi-Cluster FreqQoS Controller
 *
 * Implements closed-loop frame-time regulation over Linux FreqQoS constraints
 * with adaptive target-FPS locking, 5-frame sliding-window drift detection,
 * intra-frame hrtimer ceiling rescue, CPU/SCHED/GPU bottleneck separation,
 * cluster-proportional OPP mapping, and optional soft thermal guardrail.
 */
#include <linux/arch_topology.h>
#include <linux/cpufreq.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pm_qos.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/topology.h>

#include "kfas_internal.h"

static int cmp_u32_asc(const void *a, const void *b)
{
	u32 va = *(const u32 *)a;
	u32 vb = *(const u32 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static int cmp_u64_asc(const void *a, const void *b)
{
	u64 va = *(const u64 *)a;
	u64 vb = *(const u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static int cmp_cluster_cap(const void *a, const void *b)
{
	const struct kfas_cluster *ca = a;
	const struct kfas_cluster *cb = b;

	if (ca->cap_orig < cb->cap_orig)
		return -1;
	if (ca->cap_orig > cb->cap_orig)
		return 1;
	if (ca->policy_cpu < cb->policy_cpu)
		return -1;
	if (ca->policy_cpu > cb->policy_cpu)
		return 1;
	return 0;
}

static void kfas_init_thermal_zone_locked(void)
{
	static const char *const tz_names[] = {
		"cpuss-0", "cpu-1-0", "cpu-0-0", "cpu-0-0-usr", "socd",
	};
	int i;

	if (g_kfas.tz_cpu)
		return;

	for (i = 0; i < ARRAY_SIZE(tz_names); i++) {
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(tz_names[i]);
		if (!IS_ERR(tz)) {
			g_kfas.tz_cpu = tz;
			pr_info("kfas: bound thermal zone '%s'\n", tz_names[i]);
			return;
		}
	}
}

int kfas_freq_ensure_init_locked(void)
{
	int cpu;
	u32 i;

	if (g_kfas.freq_inited)
		return 0;

	g_kfas.nr_clusters = 0;
	memset(g_kfas.clusters, 0, sizeof(g_kfas.clusters));

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpufreq_frequency_table *pos;
		struct kfas_cluster *cl;
		bool dup = false;
		u32 nr = 0;

		if (g_kfas.nr_clusters >= KFAS_MAX_CLUSTERS)
			break;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		for (i = 0; i < g_kfas.nr_clusters; i++) {
			if (g_kfas.clusters[i].policy_cpu == policy->cpu) {
				dup = true;
				break;
			}
		}
		if (dup) {
			cpufreq_cpu_put(policy);
			continue;
		}

		cl = &g_kfas.clusters[g_kfas.nr_clusters];
		cl->policy_cpu = policy->cpu;
		cpumask_copy(&cl->related_cpus, policy->related_cpus);
		cl->cap_orig = arch_scale_cpu_capacity(policy->cpu);

		if (policy->freq_table) {
			cpufreq_for_each_valid_entry(pos, policy->freq_table) {
				if (nr < KFAS_MAX_OPPS)
					cl->opps[nr++] = pos->frequency;
			}
		}
		if (nr == 0) {
			cl->opps[0] = policy->cpuinfo.min_freq;
			cl->opps[1] = policy->cpuinfo.max_freq;
			nr = 2;
		} else {
			u32 w = 0, r;

			sort(cl->opps, nr, sizeof(u32), cmp_u32_asc, NULL);
			for (r = 0; r < nr; r++) {
				if (w == 0 || cl->opps[r] != cl->opps[w - 1])
					cl->opps[w++] = cl->opps[r];
			}
			nr = w;
		}
		cl->nr_opps = nr;
		cl->cur_max_khz = FREQ_QOS_MAX_DEFAULT_VALUE;
		cl->cur_min_khz = FREQ_QOS_MIN_DEFAULT_VALUE;

		cpufreq_cpu_put(policy);
		g_kfas.nr_clusters++;
	}

	if (g_kfas.nr_clusters == 0)
		return -EPROBE_DEFER;

	/* Sort clusters ascending by capacity: [0]=little, [1]=mid/gold, [2]=prime */
	sort(g_kfas.clusters, g_kfas.nr_clusters, sizeof(struct kfas_cluster),
	     cmp_cluster_cap, NULL);

	/* Register FreqQoS requests after sorting so plist pointers stay stable */
	for (i = 0; i < g_kfas.nr_clusters; i++) {
		struct kfas_cluster *cl = &g_kfas.clusters[i];
		struct cpufreq_policy *policy = cpufreq_cpu_get(cl->policy_cpu);
		int r1, r2;

		if (!policy)
			continue;

		r1 = freq_qos_add_request(&policy->constraints, &cl->max_req,
					  FREQ_QOS_MAX,
					  FREQ_QOS_MAX_DEFAULT_VALUE);
		r2 = freq_qos_add_request(&policy->constraints, &cl->min_req,
					  FREQ_QOS_MIN,
					  FREQ_QOS_MIN_DEFAULT_VALUE);
		cpufreq_cpu_put(policy);

		if (r1 >= 0 && r2 >= 0) {
			cl->qos_ready = true;
			pr_info("kfas: cluster[%u] policy%u cap=%lu opps=%u (%u..%u kHz)\n",
				i, cl->policy_cpu, cl->cap_orig, cl->nr_opps,
				cl->opps[0], cl->opps[cl->nr_opps - 1]);
		} else {
			if (r1 >= 0)
				freq_qos_remove_request(&cl->max_req);
			if (r2 >= 0)
				freq_qos_remove_request(&cl->min_req);
			pr_warn("kfas: cluster[%u] FreqQoS add failed (%d/%d)\n",
				i, r1, r2);
		}
	}

	kfas_init_thermal_zone_locked();
	g_kfas.freq_inited = true;
	return 0;
}

static void kfas_cluster_set_limits(struct kfas_cluster *cl,
				    unsigned int min_khz,
				    unsigned int max_khz)
{
	if (!cl->qos_ready)
		return;

	if (max_khz != cl->cur_max_khz) {
		if (freq_qos_update_request(&cl->max_req, (s32)max_khz) >= 0)
			cl->cur_max_khz = max_khz;
	}
	if (min_khz != cl->cur_min_khz) {
		if (freq_qos_update_request(&cl->min_req, (s32)min_khz) >= 0)
			cl->cur_min_khz = min_khz;
	}
}

void kfas_freq_release_limits_locked(void)
{
	u32 i;

	for (i = 0; i < g_kfas.nr_clusters; i++) {
		kfas_cluster_set_limits(&g_kfas.clusters[i],
					FREQ_QOS_MIN_DEFAULT_VALUE,
					FREQ_QOS_MAX_DEFAULT_VALUE);
	}
}

void kfas_freq_reset_session_locked(void)
{
	WRITE_ONCE(g_kfas.rendering_active, false);
	g_kfas.last_frame_ts_ns = 0;
	g_kfas.perf_level = 500;
	g_kfas.surplus_streak = 0;
	g_kfas.jank_streak = 0;
	memset(g_kfas.win5_ns, 0, sizeof(g_kfas.win5_ns));
	g_kfas.win5_idx = 0;
	g_kfas.win5_cnt = 0;
	g_kfas.rescue_armed_seq = 0;
	g_kfas.rescue_active = false;
	g_kfas.mid_frame_rescues = 0;
	atomic_set(&g_kfas.frame_seq, 0);
	g_kfas.down_cooldown = 0;
	g_kfas.max_crit_busy_pct = 0;
	g_kfas.cur_fps_x10 = 0;
	g_kfas.ema_frame_us = 0;
	g_kfas.ema_exec_us = 0;
	g_kfas.ema_delay_us = 0;
	g_kfas.ema_dq_wait_us = 0;
	g_kfas.last_bn = KFAS_BN_OK;
	g_kfas.render_tid = 0;
	g_kfas.fps_ring_idx = 0;
	g_kfas.fps_ring_cnt = 0;
	g_kfas.fps_eval_cnt = 0;
	g_kfas.fps_down_streak = 0;

	if (g_kfas.nr_cand_fps > 0)
		g_kfas.locked_fps = g_kfas.cand_fps[g_kfas.nr_cand_fps - 1];
	else
		g_kfas.locked_fps = 60;

	kfas_freq_release_limits_locked();
}

static unsigned int kfas_pick_opp(const struct kfas_cluster *cl, u32 pct_1000,
				  unsigned int floor_khz)
{
	u32 idx;
	unsigned int freq;

	if (cl->nr_opps == 0)
		return FREQ_QOS_MAX_DEFAULT_VALUE;

	if (pct_1000 >= 1000)
		return cl->opps[cl->nr_opps - 1];

	idx = (pct_1000 * (cl->nr_opps - 1) + 500) / 1000;
	if (idx >= cl->nr_opps)
		idx = cl->nr_opps - 1;
	freq = cl->opps[idx];

	if (floor_khz > 0 && freq < floor_khz) {
		u32 i;

		for (i = idx; i < cl->nr_opps; i++) {
			if (cl->opps[i] >= floor_khz) {
				freq = cl->opps[i];
				break;
			}
		}
		if (freq < floor_khz)
			freq = cl->opps[cl->nr_opps - 1];
	}

	return freq;
}

static void kfas_update_target_fps_locked(u64 frame_ns)
{
	u64 sorted[KFAS_FPS_WINDOW];
	u32 n, p35_idx, measured_fps, chosen;
	u32 i;

	g_kfas.fps_ring[g_kfas.fps_ring_idx] = frame_ns;
	g_kfas.fps_ring_idx = (g_kfas.fps_ring_idx + 1) % KFAS_FPS_WINDOW;
	if (g_kfas.fps_ring_cnt < KFAS_FPS_WINDOW)
		g_kfas.fps_ring_cnt++;

	if (g_kfas.nr_cand_fps <= 1) {
		if (g_kfas.nr_cand_fps == 1)
			g_kfas.locked_fps = g_kfas.cand_fps[0];
		return;
	}

	g_kfas.fps_eval_cnt++;
	if (g_kfas.fps_ring_cnt < 12 || (g_kfas.fps_eval_cnt & 15) != 0)
		return;

	n = g_kfas.fps_ring_cnt;
	memcpy(sorted, g_kfas.fps_ring, n * sizeof(u64));
	sort(sorted, n, sizeof(u64), cmp_u64_asc, NULL);

	/*
	 * Use the 35th-percentile frame time (faster than median) so transient
	 * lag spikes do not pull the target FPS lock down, while genuine 30/60
	 * FPS cutscenes or in-game caps still settle cleanly.
	 */
	p35_idx = (n * 35) / 100;
	if (sorted[p35_idx] == 0)
		return;

	measured_fps = (u32)div64_u64(NSEC_PER_SEC + sorted[p35_idx] / 2,
				      sorted[p35_idx]);
	chosen = g_kfas.cand_fps[g_kfas.nr_cand_fps - 1];
	for (i = 0; i < g_kfas.nr_cand_fps; i++) {
		if (g_kfas.cand_fps[i] + 3 >= measured_fps) {
			chosen = g_kfas.cand_fps[i];
			break;
		}
	}

	if (chosen > g_kfas.locked_fps) {
		/* Step up immediately (e.g. exiting 30fps cutscene) */
		g_kfas.locked_fps = chosen;
		g_kfas.fps_down_streak = 0;
		g_kfas.win5_idx = 0;
		g_kfas.win5_cnt = 0;
	} else if (chosen < g_kfas.locked_fps) {
		/* Require 3 consecutive slow windows before stepping down */
		g_kfas.fps_down_streak++;
		if (g_kfas.fps_down_streak >= 3) {
			g_kfas.locked_fps = chosen;
			g_kfas.fps_down_streak = 0;
			g_kfas.win5_idx = 0;
			g_kfas.win5_cnt = 0;
		}
	} else {
		g_kfas.fps_down_streak = 0;
	}
}

static u32 kfas_sample_temp_mc_locked(u64 now_ns)
{
	static u64 last_ts;
	int temp = 0;

	if (!g_kfas.tz_cpu)
		return 0;

	if (now_ns > last_ts && (now_ns - last_ts) < 250ULL * NSEC_PER_MSEC)
		return g_kfas.last_cpu_temp_mc;

	last_ts = now_ns;
	if (thermal_zone_get_temp(g_kfas.tz_cpu, &temp) == 0 && temp > 0)
		g_kfas.last_cpu_temp_mc = (u32)temp;

	return g_kfas.last_cpu_temp_mc;
}

static void kfas_apply_perf_level_locked(const struct kfas_mode_cfg *cfg,
					 bool severe_spike)
{
	u32 perf = g_kfas.perf_level;
	u32 i;

	for (i = 0; i < g_kfas.nr_clusters; i++) {
		struct kfas_cluster *cl = &g_kfas.clusters[i];
		unsigned int max_khz;
		unsigned int min_khz = FREQ_QOS_MIN_DEFAULT_VALUE;
		u32 scaled;

		if (g_kfas.nr_clusters == 1) {
			scaled = 250 + (perf * 750) / 1000;
			max_khz = kfas_pick_opp(cl, scaled, 0);
		} else if (i == 0) {
			/*
			 * Little cluster (A510): cap routine operation near the
			 * A510 efficiency knee across all 4 modes so high-volt
			 * tail OPPs (1.67-2.01 GHz) are avoided and busy tasks
			 * can spill cleanly onto powered mid cores (CPU4-6).
			 */
			u32 little_cap = (g_kfas.mode == KFAS_MODE_POWERSAVE) ? 540 :
					 (g_kfas.mode == KFAS_MODE_BALANCE) ? 660 :
					 (g_kfas.mode == KFAS_MODE_PERFORMANCE) ? 730 : 800;

			scaled = 240 + (perf * (little_cap - 240)) / 1000;
			if (severe_spike)
				scaled = min_t(u32, little_cap + 120,
					       scaled + 100);
			max_khz = kfas_pick_opp(cl, scaled,
						cfg->little_floor_khz);
		} else if (i == g_kfas.nr_clusters - 1 &&
			   g_kfas.nr_clusters >= 3) {
			/*
			 * Prime cluster (X3): keep capped near efficiency band
			 * whenever A715 handles the game without prime pinning
			 * so stray background bursts cannot spike X3 to 3.19GHz.
			 * Ramp smoothly up to peak OPP once prime_in_use or
			 * severe jank escalates load onto X3.
			 */
			bool prime_open = cfg->prime_enable &&
					  (g_kfas.prime_in_use ||
					   g_kfas.mode >= KFAS_MODE_PERFORMANCE ||
					   severe_spike);

			if (!prime_open) {
				scaled = 120 + (perf * 330) / 1000;
			} else if (severe_spike) {
				scaled = min_t(u32, 1000, perf + 140);
			} else if (perf < 500) {
				scaled = 180 + (perf * 360) / 500;
			} else {
				scaled = 540 + ((perf - 500) * 460) / 500;
			}
			max_khz = kfas_pick_opp(cl, scaled, 0);
			if (g_kfas.jank_streak >= 3 && cfg->prime_enable &&
			    g_kfas.prime_in_use)
				min_khz = kfas_pick_opp(cl,
							min_t(u32, 650, scaled),
							0);
		} else {
			/* Mid/Gold cluster (A715+A710 primary workhorse) */
			scaled = 220 + (perf * 780) / 1000;
			if (severe_spike)
				scaled = min_t(u32, 1000, scaled + 100);
			max_khz = kfas_pick_opp(cl, scaled, 0);
			if (g_kfas.jank_streak >= 3)
				min_khz = kfas_pick_opp(cl,
							min_t(u32, 650,
							      scaled / 2 + 180),
							0);
		}

		if (min_khz > max_khz && max_khz != FREQ_QOS_MAX_DEFAULT_VALUE)
			min_khz = max_khz;

		kfas_cluster_set_limits(cl, min_khz, max_khz);
	}
}

void kfas_freq_on_mid_rescue_locked(void)
{
	const struct kfas_mode_cfg *cfg = &g_kfas.mode_cfg[g_kfas.mode];
	u32 i;

	if (!g_kfas.freq_inited || !g_kfas.session_active ||
	    !g_kfas.rendering_active)
		return;

	/* Respect soft thermal guardrail with a narrow hysteresis band */
	if (cfg->temp_thresh_mc > 0 &&
	    g_kfas.last_cpu_temp_mc > cfg->temp_thresh_mc + 3000)
		return;

	g_kfas.rescue_active = true;
	g_kfas.mid_frame_rescues++;

	/*
	 * Mid-frame stall ceiling lift (concept adapted from open-source
	 * MiCode perfmgr intra-frame hrtimer timeout rescue):
	 *  - Immediately lift CPU max_khz ceilings WITHOUT forcing min_khz up
	 *    or permanently mutating perf_level.
	 *  - If the render/game threads are actively CPU-stalled, WALT ramps
	 *    to peak OPP right away instead of waiting for queueBuffer.
	 *  - If the frame is merely an idle VSync beat or static screen, WALT
	 *    keeps clocks idle with zero wasted power and zero perf ratchet.
	 */
	for (i = 0; i < g_kfas.nr_clusters; i++) {
		struct kfas_cluster *cl = &g_kfas.clusters[i];
		unsigned int max_khz = FREQ_QOS_MAX_DEFAULT_VALUE;

		if (g_kfas.nr_clusters > 1 && i == 0) {
			u32 little_cap = (g_kfas.mode == KFAS_MODE_POWERSAVE) ? 660 :
					 (g_kfas.mode == KFAS_MODE_BALANCE) ? 780 :
					 (g_kfas.mode == KFAS_MODE_PERFORMANCE) ? 850 : 920;

			max_khz = kfas_pick_opp(cl, little_cap,
						cfg->little_floor_khz);
		} else if (i == g_kfas.nr_clusters - 1 &&
			   g_kfas.nr_clusters >= 3 && !cfg->prime_enable) {
			max_khz = kfas_pick_opp(cl, 550, 0);
		}

		kfas_cluster_set_limits(cl, cl->cur_min_khz, max_khz);
	}
}

void kfas_freq_arm_rescue_locked(void)
{
	u64 budget_ns, margin_ns, deadline_ns, now_ns, elapsed_ns, rem_ns;

	if (!g_kfas.session_active || !g_kfas.rendering_active ||
	    g_kfas.last_frame_ts_ns == 0)
		return;

	budget_ns = div_u64(NSEC_PER_SEC, max_t(u32, 15, g_kfas.locked_fps));
	/*
	 * Target-FPS-adaptive intra-frame stall deadline (~1.5x budget, margin
	 * clamped to 2.2..12ms): ~10.4ms@144, ~12.5ms@120, ~16.7ms@90,
	 * ~25ms@60, ~45.3ms@30. Sits above normal VSync jitter while firing
	 * before a stalled frame misses a second scanout.
	 */
	margin_ns = clamp_t(u64, budget_ns / 2, 2200000ULL, 12000000ULL);
	deadline_ns = budget_ns + margin_ns;

	now_ns = ktime_get_mono_fast_ns();
	elapsed_ns = now_ns > g_kfas.last_frame_ts_ns ?
		     (now_ns - g_kfas.last_frame_ts_ns) : 0;

	g_kfas.rescue_armed_seq = (u32)atomic_read(&g_kfas.frame_seq);
	g_kfas.rescue_active = false;

	if (elapsed_ns >= deadline_ns) {
		hrtimer_cancel(&g_kfas.rescue_timer);
		kfas_freq_on_mid_rescue_locked();
		return;
	}

	rem_ns = deadline_ns - elapsed_ns;
	hrtimer_start(&g_kfas.rescue_timer, ns_to_ktime(rem_ns),
		      HRTIMER_MODE_REL);
}

void kfas_freq_on_frame_locked(const struct kfas_frame_sample *s)
{
	const struct kfas_mode_cfg *cfg = &g_kfas.mode_cfg[g_kfas.mode];
	struct kfas_gpu_snap gpu;
	u64 budget_ns, tol_ns, tol_min_ns, jank_ns, ema_jank_ns;
	u64 win5_budget_ns, win5_limit_ns, win5_sum_ns = 0;
	u64 exec_ceil_ns, exec_hold_ns, ema_exec_ns;
	u32 frame_us, exec_us, delay_us, dq_us, prev_ema_frame_us;
	u32 fps_x10, temp_mc, margin, tol_pct, ema_tol_pct, ema_jank_us;
	u32 win5_tol_pct, newest_slot;
	u32 exec_ceil_pct, exec_hold_pct, crit_busy;
	enum kfas_bottleneck bn = KFAS_BN_OK;
	bool severe = false;
	bool single_missed, win5_missed = false, frame_missed;
	bool exec_high, exec_hold;

	if (!g_kfas.freq_inited && kfas_freq_ensure_init_locked() != 0)
		return;

	WRITE_ONCE(g_kfas.rendering_active, true);
	g_kfas.rescue_active = false;
	g_kfas.last_frame_ts_ns = s->ts_ns;
	g_kfas.render_tid = s->tid;

	kfas_update_target_fps_locked(s->frame_ns);

	frame_us = (u32)div_u64(s->frame_ns, 1000U);
	exec_us = (u32)div_u64(s->exec_ns, 1000U);
	delay_us = (u32)div_u64(s->delay_ns, 1000U);
	dq_us = (u32)div_u64(s->dq_wait_ns, 1000U);
	prev_ema_frame_us = g_kfas.ema_frame_us;

	if (g_kfas.ema_frame_us == 0) {
		g_kfas.ema_frame_us = frame_us;
		g_kfas.ema_exec_us = exec_us;
		g_kfas.ema_delay_us = delay_us;
		g_kfas.ema_dq_wait_us = dq_us;
		prev_ema_frame_us = frame_us;
	} else {
		g_kfas.ema_frame_us = (g_kfas.ema_frame_us * 7 + frame_us) / 8;
		g_kfas.ema_exec_us = (g_kfas.ema_exec_us * 7 + exec_us) / 8;
		g_kfas.ema_delay_us = (g_kfas.ema_delay_us * 3 + delay_us) / 4;
		g_kfas.ema_dq_wait_us = (g_kfas.ema_dq_wait_us * 7 + dq_us) / 8;
	}

	if (g_kfas.ema_frame_us > 0) {
		fps_x10 = (u32)div64_u64(10000000ULL, g_kfas.ema_frame_us);
		g_kfas.cur_fps_x10 = fps_x10;
	}

	budget_ns = div_u64(NSEC_PER_SEC, max_t(u32, 15, g_kfas.locked_fps));
	margin = min_t(u32, 30U, cfg->margin_pct);

	/*
	 * VSync-aware single-frame + 5-frame sliding window classification:
	 *
	 * On triple-buffered VSync pipelines, Surface::queueBuffer intervals
	 * naturally oscillate +/-10-14% around budget_ns (e.g. 7.5ms / 9.1ms
	 * pairs at locked 120 FPS) without missing scanout:
	 *  - single_missed: frame_ns > budget_ns + mode-scaled phase tolerance
	 *  - win5_missed:   5-frame cumulative deficit catches sustained subtle
	 *                   FPS sag (even when alternating VSync jitter stays
	 *                   under single_missed) without false-triggering on
	 *                   symmetric VSync oscillation (concept adapted from
	 *                   open-source MiCode perfmgr FRAME_UNIT + load_reset)
	 *  - exec_ceil/hold: combines render-thread exec_ns AND heaviest
	 *                   critical thread (e.g. UnityMain) live busy%
	 */
	tol_pct = margin >= 14 ? 10 : (margin >= 10 ? 12 : (margin >= 6 ? 14 : 16));
	tol_min_ns = margin >= 14 ? 850000ULL :
		     (margin >= 10 ? 1000000ULL :
		      (margin >= 6 ? 1150000ULL : 1300000ULL));
	tol_ns = max_t(u64, div_u64(budget_ns * tol_pct, 100U), tol_min_ns);
	jank_ns = budget_ns + tol_ns;

	ema_tol_pct = margin >= 12 ? 2 : 3;
	ema_jank_ns = div_u64(budget_ns * (100U + ema_tol_pct), 100U);
	ema_jank_us = (u32)div_u64(ema_jank_ns, 1000U);

	newest_slot = g_kfas.win5_idx;
	g_kfas.win5_ns[newest_slot] = s->frame_ns;
	g_kfas.win5_idx = (newest_slot + 1) % KFAS_WIN5_SIZE;
	if (g_kfas.win5_cnt < KFAS_WIN5_SIZE)
		g_kfas.win5_cnt++;

	win5_tol_pct = margin >= 14 ? 4 :
		       (margin >= 10 ? 5 :
			(margin >= 6 ? 6 : 8));
	win5_budget_ns = budget_ns * KFAS_WIN5_SIZE;
	win5_limit_ns = div_u64(win5_budget_ns * (100U + win5_tol_pct), 100U);

	single_missed = (s->frame_ns > jank_ns);
	if (!single_missed && g_kfas.win5_cnt == KFAS_WIN5_SIZE) {
		u32 wi;

		for (wi = 0; wi < KFAS_WIN5_SIZE; wi++)
			win5_sum_ns += g_kfas.win5_ns[wi];
		win5_missed = (win5_sum_ns > win5_limit_ns) &&
			      (s->frame_ns > budget_ns);
	}

	exec_ceil_pct = clamp_t(u32, 84U - margin, 54U, 82U);
	exec_hold_pct = exec_ceil_pct - 10U;
	exec_ceil_ns = div_u64(budget_ns * exec_ceil_pct, 100U);
	exec_hold_ns = div_u64(budget_ns * exec_hold_pct, 100U);
	ema_exec_ns = (u64)g_kfas.ema_exec_us * 1000ULL;
	crit_busy = g_kfas.max_crit_busy_pct;

	frame_missed = single_missed || win5_missed;
	exec_high = (ema_exec_ns > exec_ceil_ns) ||
		    (crit_busy > exec_ceil_pct);
	exec_hold = (ema_exec_ns >= exec_hold_ns) ||
		    (crit_busy >= exec_hold_pct);

	kfas_gpu_get_snapshot(&gpu);

	/*
	 * Classify the dominant cause of frame-time pressure:
	 *  1. SCHED: render thread spent >10% of frame budget waiting in runqueue.
	 *  2. GPU:   frame slipped while GPU is saturated & CPU exec is light.
	 *  3. CPU:   frame slipped on CPU or render exec exceeded headroom cap.
	 *  4. OK:    frame finished cleanly inside target FPS & CPU headroom.
	 */
	if (s->delay_ns > div_u64(budget_ns, 10U) &&
	    (frame_missed || exec_high)) {
		bn = KFAS_BN_SCHED;
	} else if (frame_missed) {
		bool gpu_saturated = gpu.valid && gpu.busy_pct >= 88;
		bool cpu_light = s->exec_ns < div_u64(budget_ns * 45, 100U);
		bool wait_heavy = s->dq_wait_ns > div_u64(budget_ns * 30, 100U);

		if (gpu_saturated && cpu_light &&
		    (wait_heavy || (gpu.max_freq_mhz > 0 &&
				    gpu.freq_mhz * 100 >= gpu.max_freq_mhz * 85)))
			bn = KFAS_BN_GPU;
		else
			bn = KFAS_BN_CPU;
	} else if (exec_high) {
		bn = KFAS_BN_CPU;
	} else {
		bn = KFAS_BN_OK;
	}

	g_kfas.last_bn = bn;

	switch (bn) {
	case KFAS_BN_SCHED: {
		u32 bump = 28;

		g_kfas.surplus_streak = 0;
		g_kfas.down_cooldown = cfg->down_hold_frames * 3;
		g_kfas.jank_streak = min_t(u32, 10, g_kfas.jank_streak + 1);
		g_kfas.perf_level = min_t(u32, 1000, g_kfas.perf_level + bump);
		if (g_kfas.placement_enable)
			kfas_place_boost_urgent_locked();
		break;
	}
	case KFAS_BN_GPU: {
		/* Avoid futile CPU frequency spikes when GPU is the real wall */
		g_kfas.surplus_streak = 0;
		g_kfas.down_cooldown = cfg->down_hold_frames * 2;
		if (g_kfas.jank_streak > 0)
			g_kfas.jank_streak--;
		if (s->exec_ns > div_u64(budget_ns * 55, 100U))
			g_kfas.perf_level = min_t(u32, 1000,
						  g_kfas.perf_level + 12);
		break;
	}
	case KFAS_BN_CPU: {
		u32 delta;

		g_kfas.surplus_streak = 0;
		if (frame_missed) {
			u64 cur_over_ns = s->frame_ns > budget_ns ?
					  (s->frame_ns - budget_ns) : 0;
			u64 avg_over_ns = (win5_missed &&
					   win5_sum_ns > win5_budget_ns) ?
					  div_u64(win5_sum_ns - win5_budget_ns,
						  KFAS_WIN5_SIZE) : 0;
			u64 over_ns = max(cur_over_ns, avg_over_ns);
			u32 over_pm = (u32)min_t(u64, 1500ULL,
						 div64_u64(over_ns * 1000ULL,
							   budget_ns));
			bool isolated_tick = (!win5_missed &&
					      g_kfas.jank_streak == 0 &&
					      prev_ema_frame_us <= ema_jank_us &&
					      s->frame_ns <=
					      div_u64(budget_ns * 215, 100U));
			bool cpu_light_tick =
				(ema_exec_ns < div_u64(budget_ns * 40, 100U)) &&
				(crit_busy < 55);

			g_kfas.jank_streak = min_t(u32, 10,
						   g_kfas.jank_streak + 1);

			if (!isolated_tick &&
			    s->frame_ns > div_u64(budget_ns * 125, 100U)) {
				severe = true;
				g_kfas.down_cooldown = cfg->down_hold_frames * 4;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 100,
						65, 240);
			} else if (isolated_tick && cpu_light_tick) {
				/*
				 * Single isolated VSync beat while CPU load is
				 * clearly light: nudge lightly with a short
				 * cooldown so 1x/sec VSync skips cannot ratchet
				 * perf_level to 1000. A second consecutive miss
				 * still triggers immediate severe escalation.
				 */
				g_kfas.down_cooldown = max_t(u32, 4,
							     cfg->down_hold_frames / 2);
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 480,
						6, 18);
			} else if (isolated_tick) {
				g_kfas.down_cooldown = cfg->down_hold_frames;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 260,
						12, 36);
			} else {
				g_kfas.down_cooldown = cfg->down_hold_frames * 3;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 130,
						18, 110);
			}
		} else {
			/* Proactive headroom boost before an actual drop */
			u32 over_pm = 0;

			if (ema_exec_ns > exec_ceil_ns)
				over_pm = (u32)min_t(u64, 1000ULL,
						     div64_u64((ema_exec_ns - exec_ceil_ns) * 1000ULL,
							       budget_ns));
			if (crit_busy > exec_ceil_pct)
				over_pm = max_t(u32, over_pm,
						min_t(u32, 1000U,
						      (crit_busy - exec_ceil_pct) * 10U));

			g_kfas.down_cooldown = cfg->down_hold_frames;
			delta = clamp_t(u32, (over_pm * cfg->up_gain) / 200,
					8, 36);
		}
		g_kfas.perf_level = min_t(u32, 1000, g_kfas.perf_level + delta);
		break;
	}
	case KFAS_BN_OK:
	default: {
		g_kfas.jank_streak = 0;
		if (exec_hold) {
			/* Inside CPU headroom deadband: hold frequency steady */
			g_kfas.surplus_streak = 0;
			break;
		}
		if (g_kfas.down_cooldown > 0) {
			g_kfas.down_cooldown--;
			break;
		}
		g_kfas.surplus_streak++;
		if (g_kfas.surplus_streak >= cfg->down_hold_frames) {
			u32 exec_pct = (u32)min_t(u64, 100ULL,
						  div64_u64(ema_exec_ns * 100ULL,
							    budget_ns));
			u32 load_pct = max(exec_pct, crit_busy);
			u32 head_pm = exec_ceil_pct > load_pct ?
				      (exec_ceil_pct - load_pct) * 10U : 0;
			u32 step = clamp_t(u32,
					   (head_pm * cfg->down_gain) / 300,
					   6, 22);
			u32 keep_streak;

			/*
			 * Level-adaptive descent pacing: shed expensive high-
			 * voltage headroom faster when perf_level is high, and
			 * probe gently near the low-frequency stability knee.
			 */
			if (g_kfas.perf_level >= 700) {
				step = min_t(u32, 36U, (step * 3) / 2);
				keep_streak = (cfg->down_hold_frames * 2) / 3;
			} else if (g_kfas.perf_level < 400) {
				step = max_t(u32, 4U, (step * 3) / 4);
				keep_streak = cfg->down_hold_frames / 4;
			} else {
				keep_streak = cfg->down_hold_frames / 2;
			}

			if (g_kfas.perf_level > step)
				g_kfas.perf_level -= step;
			else
				g_kfas.perf_level = 0;

			g_kfas.surplus_streak = keep_streak;
		}
		break;
	}
	}

	/*
	 * Anti-windup load_reset after acting on a frame miss:
	 *  - win5_missed: reset the 5-frame integrator to budget_ns so stale
	 *    pre-boost frames cannot immediately re-trigger on the next frame.
	 *  - single_missed: replace only this handled spike slot with budget_ns
	 *    so a single isolated spike does not linger across the next 4 frames.
	 */
	if (win5_missed) {
		u32 wi;

		for (wi = 0; wi < KFAS_WIN5_SIZE; wi++)
			g_kfas.win5_ns[wi] = budget_ns;
	} else if (single_missed) {
		g_kfas.win5_ns[newest_slot] = budget_ns;
	}

	/* Optional soft thermal guardrail */
	temp_mc = kfas_sample_temp_mc_locked(s->ts_ns);
	if (cfg->temp_thresh_mc > 0 && temp_mc > cfg->temp_thresh_mc) {
		u32 excess_c = (temp_mc - cfg->temp_thresh_mc) / 1000;
		u32 cap_perf = excess_c >= 10 ? 300 : (920 - excess_c * 60);

		if (g_kfas.perf_level > cap_perf)
			g_kfas.perf_level = cap_perf;
	}

	kfas_apply_perf_level_locked(cfg, severe);
}
