// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - In-Tree GPU Devfreq Telemetry
 *
 * Hooks built-in devfreq tracepoints (zero cross-module CFI risk) to capture
 * Adreno/GPU utilization and frequency for CPU vs. GPU bottleneck separation.
 */
#include <linux/devfreq.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <trace/events/devfreq.h>

#include "kfas_internal.h"

static inline bool kfas_is_gpu_devfreq(struct devfreq *df)
{
	const char *name;

	if (!df)
		return false;
	name = dev_name(&df->dev);
	if (!name)
		return false;

	return strstr(name, "kgsl-3d0") || strstr(name, "gpu");
}

static void kfas_record_gpu_locked_update(struct devfreq *df,
					  unsigned long freq_hz)
{
	unsigned long busy = df->last_status.busy_time;
	unsigned long total = df->last_status.total_time;
	unsigned long max_hz = df->scaling_max_freq;
	unsigned long flags;
	u32 busy_pct = 0;

	if (total > 0) {
		u64 pct = div64_u64((u64)busy * 100ULL, (u64)total);

		busy_pct = pct > 100ULL ? 100U : (u32)pct;
	}

	raw_spin_lock_irqsave(&g_kfas.gpu_lock, flags);
	g_kfas.gpu.busy_pct = busy_pct;
	g_kfas.gpu.freq_mhz = (u32)(freq_hz / 1000000UL);
	if (max_hz > 0)
		g_kfas.gpu.max_freq_mhz = (u32)(max_hz / 1000000UL);
	g_kfas.gpu.ts_ns = ktime_get_mono_fast_ns();
	g_kfas.gpu.valid = true;
	raw_spin_unlock_irqrestore(&g_kfas.gpu_lock, flags);
}

static void kfas_tp_devfreq_monitor(void *data, struct devfreq *df)
{
	if (!READ_ONCE(g_kfas.session_active))
		return;
	if (!kfas_is_gpu_devfreq(df))
		return;

	kfas_record_gpu_locked_update(df, df->previous_freq);
}

static void kfas_tp_devfreq_freq(void *data, struct devfreq *df,
				 unsigned long freq, unsigned long prev_freq)
{
	if (!READ_ONCE(g_kfas.session_active))
		return;
	if (!kfas_is_gpu_devfreq(df))
		return;

	kfas_record_gpu_locked_update(df, freq);
}

void kfas_gpu_init(void)
{
	int r1, r2;

	raw_spin_lock_init(&g_kfas.gpu_lock);
	r1 = register_trace_devfreq_monitor(kfas_tp_devfreq_monitor, NULL);
	r2 = register_trace_devfreq_frequency(kfas_tp_devfreq_freq, NULL);
	g_kfas.gpu_tp_active = (!r1 || !r2);
	if (r1 && r2)
		pr_warn("kfas: devfreq GPU tracepoints unavailable (%d/%d)\n",
			r1, r2);
}

void kfas_gpu_exit(void)
{
	if (!g_kfas.gpu_tp_active)
		return;
	unregister_trace_devfreq_monitor(kfas_tp_devfreq_monitor, NULL);
	unregister_trace_devfreq_frequency(kfas_tp_devfreq_freq, NULL);
	g_kfas.gpu_tp_active = false;
}

void kfas_gpu_get_snapshot(struct kfas_gpu_snap *out)
{
	unsigned long flags;
	u64 now = ktime_get_mono_fast_ns();

	raw_spin_lock_irqsave(&g_kfas.gpu_lock, flags);
	*out = g_kfas.gpu;
	raw_spin_unlock_irqrestore(&g_kfas.gpu_lock, flags);

	if (out->valid && now > out->ts_ns &&
	    (now - out->ts_ns) > 300ULL * NSEC_PER_MSEC)
		out->valid = false;
}
