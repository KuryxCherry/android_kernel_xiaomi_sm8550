// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Multi-Cluster FreqQoS Controller
 *
 * Implements closed-loop frame-time regulation over Linux FreqQoS constraints
 * with adaptive target-FPS locking, CPU/SCHED/GPU bottleneck separation,
 * cluster-proportional OPP mapping, and optional soft thermal guardrail.
 */
#include <linux/arch_topology.h>
#include <linux/cpufreq.h>
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

void kfas_freq_cleanup(void)
{
	u32 i;

	if (!g_kfas.freq_inited)
		return;

	for (i = 0; i < g_kfas.nr_clusters; i++) {
		struct kfas_cluster *cl = &g_kfas.clusters[i];

		if (cl->qos_ready) {
			freq_qos_remove_request(&cl->max_req);
			freq_qos_remove_request(&cl->min_req);
			cl->qos_ready = false;
		}
	}
	g_kfas.freq_inited = false;
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
	g_kfas.rendering_active = false;
	g_kfas.last_frame_ts_ns = 0;
	g_kfas.perf_level = 500;
	g_kfas.surplus_streak = 0;
	g_kfas.jank_streak = 0;
	g_kfas.down_cooldown = 0;
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
	} else if (chosen < g_kfas.locked_fps) {
		/* Require 3 consecutive slow windows before stepping down */
		g_kfas.fps_down_streak++;
		if (g_kfas.fps_down_streak >= 3) {
			g_kfas.locked_fps = chosen;
			g_kfas.fps_down_streak = 0;
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

void kfas_freq_on_frame_locked(const struct kfas_frame_sample *s)
{
	const struct kfas_mode_cfg *cfg = &g_kfas.mode_cfg[g_kfas.mode];
	struct kfas_gpu_snap gpu;
	u64 budget_ns, tol_ns, jank_ns, exec_ceil_ns, exec_hold_ns, ema_exec_ns;
	u32 frame_us, exec_us, delay_us, dq_us, prev_ema_frame_us;
	u32 fps_x10, temp_mc, margin, tol_pct, ema_tol_pct, ema_jank_us;
	u32 exec_ceil_pct;
	enum kfas_bottleneck bn = KFAS_BN_OK;
	bool severe = false;
	bool frame_missed, exec_high, exec_hold;

	if (!g_kfas.freq_inited && kfas_freq_ensure_init_locked() != 0)
		return;

	g_kfas.rendering_active = true;
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
	 * VSync-aware frame pressure classification:
	 *
	 * On VSync-paced games, wall-clock frame_ns hovers right around
	 * budget_ns (e.g. 8333 us at 120 FPS) with +/-3-6% Choreographer phase
	 * jitter. Comparing frame_ns against budget_ns*(1-margin) misclassifies
	 * locked full-FPS frames as CPU jank and pins perf_level at 1000.
	 *
	 * Separate real VSync slips from proactive CPU headroom:
	 *  - jank_ns:      budget_ns + positive VSync jitter tolerance
	 *  - ema_jank_us:  sustained EMA drift above 102-103% of budget
	 *  - exec_ceil_ns: render-thread CPU headroom ceiling (scaled by margin)
	 *  - exec_hold_ns: 10% deadband below exec_ceil_ns to prevent hunting
	 */
	tol_pct = margin >= 14 ? 4 : (margin >= 10 ? 5 : (margin >= 6 ? 6 : 7));
	tol_ns = max_t(u64, div_u64(budget_ns * tol_pct, 100U), 500000ULL);
	jank_ns = budget_ns + tol_ns;

	ema_tol_pct = margin >= 12 ? 2 : 3;
	ema_jank_us = (u32)div_u64(budget_ns * (100U + ema_tol_pct), 100000ULL);

	exec_ceil_pct = clamp_t(u32, 84U - margin, 54U, 82U);
	exec_ceil_ns = div_u64(budget_ns * exec_ceil_pct, 100U);
	exec_hold_ns = div_u64(budget_ns * (exec_ceil_pct - 10U), 100U);
	ema_exec_ns = (u64)g_kfas.ema_exec_us * 1000ULL;

	/*
	 * Require the current frame to also exceed budget + half-tolerance
	 * before triggering sustained EMA jank, so a single 16ms VSync tick
	 * cannot chain-react across the next 4-5 normal 8.35ms frames.
	 */
	frame_missed = (s->frame_ns > jank_ns) ||
		       (g_kfas.ema_frame_us > ema_jank_us &&
			s->frame_ns > budget_ns + (tol_ns >> 1));
	exec_high = (ema_exec_ns > exec_ceil_ns);
	exec_hold = (ema_exec_ns >= exec_hold_ns);

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
			u64 over_ns = s->frame_ns > budget_ns ?
				      (s->frame_ns - budget_ns) : 0;
			u32 over_pm = (u32)min_t(u64, 1500ULL,
						 div64_u64(over_ns * 1000ULL,
							   budget_ns));
			bool isolated_tick = (g_kfas.jank_streak == 0 &&
					      prev_ema_frame_us <= ema_jank_us &&
					      s->frame_ns <=
					      div_u64(budget_ns * 215, 100U));

			g_kfas.jank_streak = min_t(u32, 10,
						   g_kfas.jank_streak + 1);

			if (!isolated_tick &&
			    s->frame_ns > div_u64(budget_ns * 125, 100U)) {
				severe = true;
				g_kfas.down_cooldown = cfg->down_hold_frames * 4;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 100,
						65, 240);
			} else if (isolated_tick) {
				/*
				 * Single isolated VSync edge tick (<= ~2x budget)
				 * while smoothed EMA FPS is still locked: apply
				 * a gentle protective bump without pinning perf
				 * at 1000; a second consecutive miss escalates
				 * immediately via !isolated_tick.
				 */
				g_kfas.down_cooldown = cfg->down_hold_frames;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 260,
						10, 36);
			} else {
				g_kfas.down_cooldown = cfg->down_hold_frames * 3;
				delta = clamp_t(u32,
						(over_pm * cfg->up_gain) / 130,
						18, 110);
			}
		} else {
			/* Proactive headroom boost before an actual drop */
			u64 over_ns = ema_exec_ns > exec_ceil_ns ?
				      (ema_exec_ns - exec_ceil_ns) : 0;
			u32 over_pm = (u32)min_t(u64, 1000ULL,
						 div64_u64(over_ns * 1000ULL,
							   budget_ns));

			g_kfas.down_cooldown = cfg->down_hold_frames * 2;
			delta = clamp_t(u32, (over_pm * cfg->up_gain) / 200,
					10, 40);
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
			u64 head_ns = exec_ceil_ns > ema_exec_ns ?
				      (exec_ceil_ns - ema_exec_ns) : 0;
			u32 head_pm = (u32)min_t(u64, 1000ULL,
						 div64_u64(head_ns * 1000ULL,
							   budget_ns));
			u32 step = clamp_t(u32,
					   (head_pm * cfg->down_gain) / 360,
					   5, 18);

			if (g_kfas.perf_level > step)
				g_kfas.perf_level -= step;
			else
				g_kfas.perf_level = 0;

			/* Keep partial streak for smooth gradual descent */
			g_kfas.surplus_streak = cfg->down_hold_frames / 2;
		}
		break;
	}
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
