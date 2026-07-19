// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/sched/topology.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/version.h>
#include <ax_sched_common.h>

#define GB_MAX_TIDS 16
#define GB_DEFAULT_CPU_UTIL 704
#define GB_DEFAULT_SUSTAINED_UTIL 704
#define GB_DEFAULT_RENDER_UTIL 768
#define GB_DEFAULT_LOADING_UTIL 832
#define GB_DEFAULT_UTIL_CAP 928
#define GB_DEFAULT_MAX_DURATION_MS 5000
#define GB_DEFAULT_RESOURCE_PULSE_MS 700
#define GB_DEFAULT_SPREAD_WINDOW_MS 16
#define GB_CPU_PRESSURE_MAX 8

static unsigned int gb_enabled = 1;
module_param_named(enabled, gb_enabled, uint, 0644);

static unsigned int gb_cpu_util = GB_DEFAULT_CPU_UTIL;
module_param_named(cpu_util, gb_cpu_util, uint, 0644);

static unsigned int gb_sustained_util = GB_DEFAULT_SUSTAINED_UTIL;
module_param_named(sustained_util, gb_sustained_util, uint, 0644);

static unsigned int gb_render_util = GB_DEFAULT_RENDER_UTIL;
module_param_named(render_util, gb_render_util, uint, 0644);

static unsigned int gb_loading_util = GB_DEFAULT_LOADING_UTIL;
module_param_named(loading_util, gb_loading_util, uint, 0644);

static unsigned int gb_util_cap = GB_DEFAULT_UTIL_CAP;
module_param_named(util_cap, gb_util_cap, uint, 0644);

static unsigned int gb_top_prio = MAX_PRIO - 1;
module_param_named(top_prio, gb_top_prio, uint, 0644);

static unsigned int gb_max_duration_ms = GB_DEFAULT_MAX_DURATION_MS;
module_param_named(max_duration_ms, gb_max_duration_ms, uint, 0644);

static unsigned int gb_resource_pulse_ms = GB_DEFAULT_RESOURCE_PULSE_MS;
module_param_named(resource_pulse_ms, gb_resource_pulse_ms, uint, 0644);

static unsigned int gb_big_only = 1;
module_param_named(big_only, gb_big_only, uint, 0644);

static unsigned int gb_fallback_big = 1;
module_param_named(fallback_big, gb_fallback_big, uint, 0644);

static unsigned int gb_spread_active = 1;
module_param_named(spread_active, gb_spread_active, uint, 0644);

static unsigned int gb_spread_window_ms = GB_DEFAULT_SPREAD_WINDOW_MS;
module_param_named(spread_window_ms, gb_spread_window_ms, uint, 0644);

static DEFINE_RAW_SPINLOCK(gb_lock);
static pid_t gb_pid = -1;
static unsigned int gb_sticky_profile;
static unsigned int gb_sticky_level;
static unsigned int gb_pulse_profile;
static unsigned int gb_pulse_level;
static unsigned long gb_pulse_expire_jiffies;
static int gb_tid_count;
static pid_t gb_tids[GB_MAX_TIDS];
static unsigned int gb_cpu_score[NR_CPUS];
static unsigned int gb_cpu_capacity[NR_CPUS];
static atomic_t gb_cpu_pressure[NR_CPUS];
static unsigned long gb_cpu_pressure_jiffies[NR_CPUS];

static unsigned int gb_profile_util(unsigned int profile, unsigned int level)
{
	unsigned int util;
	unsigned int cap;

	switch (profile) {
	case AX_GAME_BOOST_PROFILE_CPU:
		util = READ_ONCE(gb_cpu_util);
		break;
	case AX_GAME_BOOST_PROFILE_SUSTAINED:
		util = READ_ONCE(gb_sustained_util);
		break;
	case AX_GAME_BOOST_PROFILE_RENDER:
		util = READ_ONCE(gb_render_util);
		break;
	case AX_GAME_BOOST_PROFILE_LOADING:
		util = READ_ONCE(gb_loading_util);
		break;
	default:
		return 0;
	}

	if (level >= AX_BOOST_LEVEL_HEAVY)
		util = min_t(unsigned int, util + 64, SCHED_CAPACITY_SCALE);

	cap = ax_sched_clamp_util(READ_ONCE(gb_util_cap));
	return min_t(unsigned int, ax_sched_clamp_util(util), cap);
}

static bool gb_pulse_active(void)
{
	return READ_ONCE(gb_pulse_profile) &&
		time_before(jiffies, READ_ONCE(gb_pulse_expire_jiffies));
}

static unsigned int gb_active_util(void)
{
	unsigned int util = 0;

	if (!READ_ONCE(gb_enabled) || READ_ONCE(gb_pid) <= 0)
		return 0;

	util = gb_profile_util(READ_ONCE(gb_sticky_profile),
				       READ_ONCE(gb_sticky_level));
	if (gb_pulse_active())
		util = max_t(unsigned int, util,
			       gb_profile_util(READ_ONCE(gb_pulse_profile),
					       READ_ONCE(gb_pulse_level)));

	return util;
}

unsigned int ax_game_boost_active_util(void)
{
	return gb_active_util();
}
EXPORT_SYMBOL_GPL(ax_game_boost_active_util);

static bool gb_task_is_important(struct task_struct *task)
{
	unsigned int prio;

	if (!task)
		return false;

	prio = READ_ONCE(gb_top_prio);
	return prio < MAX_PRIO && task->prio <= prio;
}

static bool gb_task_matches(struct task_struct *task)
{
	pid_t pid;
	pid_t tid;
	int count;
	int i;

	if (!task)
		return false;

	pid = READ_ONCE(gb_pid);
	if (pid <= 0 || task_tgid_nr(task) != pid)
		return false;

	tid = task_pid_nr(task);
	if (tid == pid)
		return true;

	count = min_t(int, READ_ONCE(gb_tid_count), GB_MAX_TIDS);
	for (i = 0; i < count; i++) {
		if (tid == READ_ONCE(gb_tids[i]))
			return true;
	}

	return gb_task_is_important(task) || ax_sched_task_is_render_helper(task);
}

unsigned int ax_game_boost_task_util(struct task_struct *task)
{
	unsigned int util = gb_active_util();

	if (!util || !gb_task_matches(task))
		return 0;

	return util;
}
EXPORT_SYMBOL_GPL(ax_game_boost_task_util);

static unsigned int gb_get_cpu_score(int cpu)
{
	unsigned int score = READ_ONCE(gb_cpu_score[cpu]);

	return score ? score : cpu + 1;
}

static unsigned int gb_get_cpu_capacity(int cpu)
{
	unsigned int capacity = READ_ONCE(gb_cpu_capacity[cpu]);

	return capacity ? capacity : SCHED_CAPACITY_SCALE;
}

static unsigned int gb_cpu_pressure_score(int cpu)
{
	unsigned int pressure = atomic_read(&gb_cpu_pressure[cpu]);
	unsigned long last = READ_ONCE(gb_cpu_pressure_jiffies[cpu]);
	unsigned long window;

	if (!READ_ONCE(gb_spread_active) || !pressure)
		return 0;

	window = max_t(unsigned long,
		       msecs_to_jiffies(max_t(unsigned int,
					      READ_ONCE(gb_spread_window_ms), 1)), 1);
	if (time_after_eq(jiffies, last + window)) {
		atomic_set(&gb_cpu_pressure[cpu], 0);
		return 0;
	}

	return min_t(unsigned int, pressure, GB_CPU_PRESSURE_MAX);
}

static void gb_note_cpu_pick(int cpu)
{
	unsigned int pressure;

	if (!READ_ONCE(gb_spread_active))
		return;

	WRITE_ONCE(gb_cpu_pressure_jiffies[cpu], jiffies);
	pressure = atomic_read(&gb_cpu_pressure[cpu]);
	if (pressure < GB_CPU_PRESSURE_MAX)
		atomic_inc(&gb_cpu_pressure[cpu]);
}

static void gb_init_cpu_scores(void)
{
	unsigned int max_score = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		unsigned int score = cpu + 1;

		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			score = max_t(unsigned int, policy->cpuinfo.max_freq,
				      policy->max);
			cpufreq_cpu_put(policy);
		}

		score = score ? score : cpu + 1;
		WRITE_ONCE(gb_cpu_score[cpu], score);
		max_score = max_t(unsigned int, max_score, score);
	}

	for_each_possible_cpu(cpu) {
		unsigned int capacity = SCHED_CAPACITY_SCALE;
		unsigned int score = gb_get_cpu_score(cpu);

		if (max_score)
			capacity = (unsigned int)((u64)score * SCHED_CAPACITY_SCALE /
						  max_score);
		WRITE_ONCE(gb_cpu_capacity[cpu], max_t(unsigned int, capacity, 1));
		atomic_set(&gb_cpu_pressure[cpu], 0);
		WRITE_ONCE(gb_cpu_pressure_jiffies[cpu], 0);
	}
}

static int gb_pick_cpu(struct task_struct *task, unsigned int util,
			       struct cpumask *local_cpu_mask, bool prefer_peak)
{
	unsigned int fallback_score = 0;
	unsigned int fallback_pressure = ~0U;
	unsigned int best_score = prefer_peak ? 0 : ~0U;
	unsigned int best_pressure = ~0U;
	int fallback_cpu = AX_SCHED_CPU_NONE;
	int best_cpu = AX_SCHED_CPU_NONE;
	int cpu;

	if (!task)
		return AX_SCHED_CPU_NONE;

	for_each_online_cpu(cpu) {
		unsigned int pressure;
		unsigned int score;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
		if (!cpumask_test_cpu(cpu, task->cpus_ptr))
#else
		if (!cpumask_test_cpu(cpu, &task->cpus_allowed))
#endif
			continue;
		if (local_cpu_mask && !cpumask_test_cpu(cpu, local_cpu_mask))
			continue;

		score = gb_get_cpu_score(cpu);
		pressure = gb_cpu_pressure_score(cpu);
		if (score > fallback_score ||
		    (score == fallback_score && pressure <= fallback_pressure)) {
			fallback_score = score;
			fallback_pressure = pressure;
			fallback_cpu = cpu;
		}

		if (!prefer_peak && util && gb_get_cpu_capacity(cpu) < util)
			continue;

		if (prefer_peak) {
			if (score > best_score ||
			    (score == best_score && pressure <= best_pressure)) {
				best_score = score;
				best_pressure = pressure;
				best_cpu = cpu;
			}
		} else if (score < best_score ||
			   (score == best_score && pressure <= best_pressure)) {
			best_score = score;
			best_pressure = pressure;
			best_cpu = cpu;
		}
	}

	cpu = best_cpu >= 0 ? best_cpu : fallback_cpu;
	if (cpu >= 0)
		gb_note_cpu_pick(cpu);

	return cpu;
}

static bool gb_prefers_peak(void)
{
	return gb_pulse_active() &&
		READ_ONCE(gb_pulse_profile) == AX_GAME_BOOST_PROFILE_LOADING;
}

bool ax_game_boost_pick_task_rq(struct task_struct *task,
					struct ax_sched_cpu_pick *pick)
{
	unsigned int util;
	int cpu;

	if (!pick || !READ_ONCE(gb_big_only))
		return false;

	util = ax_game_boost_task_util(task);
	if (!util)
		return false;

	cpu = gb_pick_cpu(task, util, NULL, gb_prefers_peak());
	return ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_GAME);
}
EXPORT_SYMBOL_GPL(ax_game_boost_pick_task_rq);

bool ax_game_boost_pick_fallback_rq(int prev_cpu, struct task_struct *task,
					    struct ax_sched_cpu_pick *pick)
{
	unsigned int util;
	int cpu;

	if (!pick || !READ_ONCE(gb_fallback_big))
		return false;

	util = ax_game_boost_task_util(task);
	if (!util)
		return false;

	cpu = gb_pick_cpu(task, util, NULL, gb_prefers_peak());
	if (cpu == prev_cpu)
		return false;

	return ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_GAME);
}
EXPORT_SYMBOL_GPL(ax_game_boost_pick_fallback_rq);

bool ax_game_boost_pick_lowest_rq(struct task_struct *task,
					  struct cpumask *local_cpu_mask,
					  struct ax_sched_cpu_pick *pick)
{
	unsigned int util;
	int cpu;

	if (!pick || !local_cpu_mask || !READ_ONCE(gb_big_only))
		return false;

	util = ax_game_boost_task_util(task);
	if (!util)
		return false;

	cpu = gb_pick_cpu(task, util, local_cpu_mask, gb_prefers_peak());
	return ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_GAME);
}
EXPORT_SYMBOL_GPL(ax_game_boost_pick_lowest_rq);

static unsigned int gb_resource_mask(unsigned int profile)
{
	switch (profile) {
	case AX_GAME_BOOST_PROFILE_CPU:
		return AX_BOOST_PMQOS;
	case AX_GAME_BOOST_PROFILE_SUSTAINED:
		return AX_BOOST_GPU | AX_BOOST_MEMLAT | AX_BOOST_DDR;
	case AX_GAME_BOOST_PROFILE_RENDER:
		return AX_BOOST_GPU | AX_BOOST_MEMLAT | AX_BOOST_DDR |
			AX_BOOST_L3;
	case AX_GAME_BOOST_PROFILE_LOADING:
		return AX_BOOST_PMQOS | AX_BOOST_GPU | AX_BOOST_MEMLAT |
			AX_BOOST_DDR | AX_BOOST_L3;
	default:
		return 0;
	}
}

static bool gb_clear_locked(pid_t pid)
{
	if (pid > 0 && gb_pid > 0 && pid != gb_pid)
		return false;

	gb_pid = -1;
	gb_sticky_profile = 0;
	gb_sticky_level = 0;
	gb_pulse_profile = 0;
	gb_pulse_level = 0;
	gb_pulse_expire_jiffies = 0;
	gb_tid_count = 0;
	memset(gb_tids, 0, sizeof(gb_tids));
	return true;
}

void ax_game_boost_clear(pid_t pid)
{
	unsigned long flags;
	bool cleared;

	raw_spin_lock_irqsave(&gb_lock, flags);
	cleared = gb_clear_locked(pid);
	raw_spin_unlock_irqrestore(&gb_lock, flags);

	if (cleared)
		ax_sched_boost_clear(AX_BOOST_SOURCE_GAME);
}
EXPORT_SYMBOL_GPL(ax_game_boost_clear);

static unsigned int gb_clamp_duration(unsigned int duration_ms)
{
	return clamp(duration_ms, 1U, max_t(unsigned int,
					       READ_ONCE(gb_max_duration_ms), 1));
}

static unsigned int gb_sticky_pulse_ms(void)
{
	return clamp(READ_ONCE(gb_resource_pulse_ms), 1U,
		     max_t(unsigned int, READ_ONCE(gb_max_duration_ms), 1));
}

void ax_game_boost_update(pid_t pid, unsigned int profile,
			  unsigned int level, unsigned int duration_ms, bool sticky,
			  int tid_count, const pid_t *tids)
{
	unsigned long flags;
	unsigned int resources;
	unsigned int boost_ms;
	int count;
	int i;

	if (!READ_ONCE(gb_enabled) || pid <= 0 || !profile) {
		ax_game_boost_clear(pid);
		return;
	}

	profile = clamp_t(unsigned int, profile, AX_GAME_BOOST_PROFILE_SUSTAINED,
			  AX_GAME_BOOST_PROFILE_CPU);
	level = clamp_t(unsigned int, level, AX_BOOST_LEVEL_LIGHT,
			AX_BOOST_LEVEL_HEAVY);
	count = clamp(tid_count, 0, GB_MAX_TIDS);
	boost_ms = sticky ? gb_sticky_pulse_ms() : gb_clamp_duration(duration_ms);

	raw_spin_lock_irqsave(&gb_lock, flags);
	if (gb_pid != pid)
		gb_clear_locked(0);
	gb_pid = pid;
	gb_tid_count = 0;
	memset(gb_tids, 0, sizeof(gb_tids));
	for (i = 0; i < count; i++) {
		if (tids && tids[i] > 0)
			gb_tids[gb_tid_count++] = tids[i];
	}
	if (sticky) {
		gb_sticky_profile = profile;
		gb_sticky_level = level;
	} else {
		gb_pulse_profile = profile;
		gb_pulse_level = level;
		gb_pulse_expire_jiffies = jiffies + msecs_to_jiffies(boost_ms);
	}
	raw_spin_unlock_irqrestore(&gb_lock, flags);

	resources = gb_resource_mask(profile);
	if (resources)
		ax_sched_boost(AX_BOOST_SOURCE_GAME, level, resources, boost_ms);
}
EXPORT_SYMBOL_GPL(ax_game_boost_update);

static int __init ax_game_boost_init(void)
{
	gb_init_cpu_scores();
	return 0;
}

module_init(ax_game_boost_init);

static void __exit ax_game_boost_exit(void)
{
	ax_game_boost_clear(0);
}

module_exit(ax_game_boost_exit);

MODULE_LICENSE("GPL");
