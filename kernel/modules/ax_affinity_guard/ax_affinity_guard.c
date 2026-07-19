// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#include <linux/capability.h>
#include <linux/cpumask.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uidgid.h>
#include <ax_sched_common.h>
#include <trace/hooks/sched.h>

#define AG_UID_PER_USER 100000
#define AG_FIRST_APP_UID 10000

static unsigned int enabled = 1;
module_param_named(enabled, enabled, uint, 0644);

static unsigned int min_cpus = 4;
module_param_named(min_cpus, min_cpus, uint, 0644);

static unsigned int max_narrow_cpus = 2;
module_param_named(max_narrow_cpus, max_narrow_cpus, uint, 0644);

static unsigned int block_all_narrowing = 1;
module_param_named(block_all_narrowing, block_all_narrowing, uint, 0644);

static unsigned int allow_privileged_callers = 1;
module_param_named(allow_privileged_callers, allow_privileged_callers, uint, 0644);

static unsigned int same_process_only = 1;
module_param_named(same_process_only, same_process_only, uint, 0644);

static uid_t ag_uid(struct task_struct *task)
{
	return from_kuid(current_user_ns(), task_uid(task));
}

static bool ag_uid_is_app(uid_t uid)
{
	return uid % AG_UID_PER_USER >= AG_FIRST_APP_UID;
}

static bool ag_current_is_privileged(void)
{
	uid_t uid = from_kuid(current_user_ns(), current_uid());

	return !ag_uid_is_app(uid) || capable(CAP_SYS_NICE);
}

static bool ag_should_swallow(struct task_struct *task,
			      const struct cpumask *mask)
{
	cpumask_t requested;
	cpumask_t baseline;
	unsigned int requested_count;
	unsigned int baseline_count;
	unsigned int min_count;

	if (!READ_ONCE(enabled) || !task || !mask)
		return false;
	if (!ag_uid_is_app(ag_uid(task)))
		return false;
	if (READ_ONCE(allow_privileged_callers) && ag_current_is_privileged())
		return false;
	if (READ_ONCE(same_process_only) && task_tgid_nr(task) != task_tgid_nr(current))
		return false;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	cpumask_and(&baseline, task->cpus_ptr, cpu_active_mask);
#else
	cpumask_and(&baseline, &task->cpus_allowed, cpu_active_mask);
#endif
	cpumask_and(&requested, mask, &baseline);
	requested_count = cpumask_weight(&requested);
	baseline_count = cpumask_weight(&baseline);
	if (!baseline_count)
		return false;
	if (!requested_count)
		return true;
	if (cpumask_subset(&baseline, &requested))
		return false;

	min_count = min_t(unsigned int, READ_ONCE(min_cpus), baseline_count);
	if (READ_ONCE(block_all_narrowing))
		return true;
	if (requested_count < min_count ||
	    requested_count <= READ_ONCE(max_narrow_cpus))
		return true;

	return false;
}

static void ag_setaffinity_early(void *unused, struct task_struct *task,
				 const struct cpumask *mask, bool *skip)
{
	if (!skip || *skip)
		return;
	if (!ag_should_swallow(task, mask))
		return;

	*skip = true;
}

static int __init ax_affinity_guard_init(void)
{
	int ret;

	ret = register_trace_android_vh_sched_setaffinity_early(
		ag_setaffinity_early, NULL);
	if (ret)
		return ret;

	return 0;
}

module_init(ax_affinity_guard_init);

static void __exit ax_affinity_guard_exit(void)
{
	ax_sched_unregister_hook(android_vh_sched_setaffinity_early,
				 ag_setaffinity_early, NULL);
	tracepoint_synchronize_unregister();
}

module_exit(ax_affinity_guard_exit);

MODULE_LICENSE("GPL");
