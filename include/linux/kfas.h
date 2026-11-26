/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel Frame-Aware Scheduler (KFAS) - WALT Scheduler Bridge Interface
 */
#ifndef _LINUX_KFAS_H
#define _LINUX_KFAS_H

#include <linux/types.h>

struct task_struct;

struct kfas_walt_ops {
	int (*set_core_ctl_boost)(bool boost);
	int (*set_task_placement)(struct task_struct *p, int target_cpu,
				  bool mvp, int boost, u64 boost_period_ns);
	void (*clear_task_placement)(struct task_struct *p);
	u32 (*get_task_demand_scaled)(struct task_struct *p);
};

#if IS_ENABLED(CONFIG_KFAS)
int kfas_register_walt_ops(const struct kfas_walt_ops *ops);
void kfas_unregister_walt_ops(void);
#else
static inline int kfas_register_walt_ops(const struct kfas_walt_ops *ops)
{
	return 0;
}
static inline void kfas_unregister_walt_ops(void)
{
}
#endif

#endif /* _LINUX_KFAS_H */
