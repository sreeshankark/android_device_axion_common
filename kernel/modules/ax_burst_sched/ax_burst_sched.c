// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cgroup.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/ioprio.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/topology.h>
#include <linux/sched/types.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <uapi/linux/sched/types.h>
#include <ax_sched_common.h>
#include <trace/hooks/sched.h>
#if defined(AX_BS_HAS_INPUT_HOOK)
#include <linux/input.h>
#include <trace/hooks/input.h>
#endif
#if defined(AX_BS_HAS_IOLIMIT_HOOK)
#include <trace/hooks/fs.h>
#endif
#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
#include <trace/hooks/gpu.h>
#endif
#if defined(AX_BS_HAS_RWSEM_HOOK) && !defined(CONFIG_PREEMPT_RT)
#include <trace/hooks/rwsem.h>
#define AX_BS_HAS_LOCK_BOOST
#endif
#if defined(AX_BS_HAS_MUTEX_HOOK) && defined(AX_BS_HAS_LOCK_BOOST)
#include <trace/hooks/dtask.h>
#endif

#define AX_BS_MAX_TARGETS 16
#define AX_BS_MAX_TIDS 16
#define AX_BS_CMD_SIZE 256
#define AX_BS_PROC_DIR "ax_burst_sched"
#define AX_BS_PROC_SCENE "scene"
#define AX_BS_PROC_STATS "stats"
#define AX_BS_MODE_LAUNCH 1
#define AX_BS_MODE_ANIMATION 2
#define AX_BS_MODE_REMOTE 3
#define AX_BS_MODE_TOP_APP 4
#define AX_BS_MODE_PERF 5
#define AX_BS_MODE_MAX AX_BS_MODE_PERF
#define AX_BS_SOURCE_LAUNCH 1
#define AX_BS_SOURCE_ANIMATION 2
#define AX_BS_SOURCE_REMOTE 3
#define AX_BS_SOURCE_FRAME_RESCUE 4
#define AX_BS_SOURCE_START_ACTIVITY_BINDER 5
#define AX_BS_SOURCE_TOP_APP 6
#define AX_BS_SOURCE_ADPF_CPU 7
#define AX_BS_SOURCE_ADPF_GPU 8
#define AX_BS_SOURCE_POWER_INTERACTION 9
#define AX_BS_SOURCE_POWER_DISPLAY 10
#define AX_BS_SOURCE_POWER_LAUNCH 11
#define AX_BS_SOURCE_POWER_RENDER 12
#define AX_BS_SOURCE_GAME_LOADING 13
#define AX_BS_SOURCE_GAME 14
#define AX_BS_SOURCE_TOP_APP_SWITCH 15
#define AX_BS_SOURCE_MAX AX_BS_SOURCE_TOP_APP_SWITCH
#define AX_BS_SEVERITY_LIGHT 1
#define AX_BS_SEVERITY_HEAVY 2
#define AX_BS_SEVERITY_MAX AX_BS_SEVERITY_HEAVY
#define AX_BS_ROLE_APP 0
#define AX_BS_ROLE_SYSTEM_SERVER 1
#define AX_BS_ROLE_SYSTEM_UI 2
#define AX_BS_ROLE_LAUNCHER 3
#define AX_BS_ROLE_TOP_APP 4
#define AX_BS_ROLE_MAX AX_BS_ROLE_TOP_APP
#define AX_BS_DEFAULT_LAUNCH_UTIL 896
#define AX_BS_DEFAULT_ANIMATION_UTIL 832
#define AX_BS_DEFAULT_REMOTE_UTIL 800
#define AX_BS_DEFAULT_TOP_APP_UTIL 704
#define AX_BS_DEFAULT_PERF_UTIL 800
#define AX_BS_DEFAULT_SYSTEM_SERVER_UTIL 800
#define AX_BS_DEFAULT_SYSTEM_UI_UTIL 896
#define AX_BS_DEFAULT_LAUNCHER_UTIL 864
#define AX_BS_DEFAULT_TOP_ROLE_UTIL 864
#define AX_BS_DEFAULT_SCENE_UTIL 896
#define AX_BS_DEFAULT_SUPPORT_WORKER_UTIL 864
#define AX_BS_DEFAULT_LAUNCH_WORKER_UTIL 864
#define AX_BS_DEFAULT_UTIL_CAP 960
#define AX_BS_DEFAULT_INITIAL_SCORE 160
#define AX_BS_DEFAULT_SCORE_GAIN 40
#define AX_BS_DEFAULT_SCORE_DECAY 12
#define AX_BS_DEFAULT_SCORE_WINDOW_MS 32
#define AX_BS_DEFAULT_MAX_DURATION_MS 3000
#define AX_BS_DEFAULT_SPREAD_WINDOW_MS 12
#define AX_BS_DEFAULT_SPILL_PRESSURE 2
#define AX_BS_DEFAULT_MIGRATION_HOLD_MS 10
#define AX_BS_DEFAULT_AUTO_TOP_APP_UTIL 800
#define AX_BS_DEFAULT_AUTO_TOP_APP_HOLD_MS 64
#define AX_BS_DEFAULT_TOP_APP_HOT_SCORE 160
#define AX_BS_DEFAULT_TOP_APP_PRIO 119
#define AX_BS_DEFAULT_SYSTEMUI_PREEMPTS_TOP_APP 1
#define AX_BS_DEFAULT_IO_GUARD_CLASS IOPRIO_CLASS_IDLE
#define AX_BS_DEFAULT_IO_GUARD_PRIO 7
#define AX_BS_DEFAULT_IO_GUARD_DURATION_MS 384
#define AX_BS_DEFAULT_IO_GUARD_SCAN_MS 128
#define AX_BS_DEFAULT_IO_LIMIT_DELAY_MS 0
#define AX_BS_DEFAULT_IO_LIMIT_MIN_BYTES 4096
#define AX_BS_DEFAULT_GPU_CONTEXT_BOOST_MS 384
#define AX_BS_DEFAULT_DYNAMIC_SVP_MIN_RANK 60
#define AX_BS_DEFAULT_DYNAMIC_SVP_MIN_DELTA 8
#define AX_BS_MAX_IO_GUARDS 64
#define AX_BS_MAX_LOCK_TARGETS 32
#define AX_BS_DEFAULT_LOCK_UTIL 704
#define AX_BS_DEFAULT_LOCK_MAX_UTIL 896
#define AX_BS_DEFAULT_LOCK_TIMEOUT_MS 64
#define AX_BS_CLEANUP_INTERVAL_MS 5000
#define AX_BS_SCORE_MAX 255
#define AX_BS_CPU_PRESSURE_MAX 8
#define AX_BS_FIRST_APPLICATION_UID 10000
#define AX_BS_RANK_BACKGROUND 10
#define AX_BS_RANK_RECLAIM 20
#define AX_BS_RANK_SUPPORT 60
#define AX_BS_RANK_LOCK 68
#define AX_BS_RANK_TOP_APP 70
#define AX_BS_RANK_DISPLAY 72
#define AX_BS_RANK_SYSTEM 78
#define AX_BS_RANK_VISUAL 80
#define AX_BS_RANK_LAUNCH 86
#define AX_BS_RANK_FRAME 100

struct ax_bs_target {
	seqcount_t cpu_seq;
	pid_t pid;
	int cpu;
	int uid;
	int mode;
	int source;
	int severity;
	int role;
	unsigned int resource_source;
	int tid_count;
	pid_t tids[AX_BS_MAX_TIDS];
	struct pid *pid_ref;
	unsigned long expire_jiffies;
	unsigned long last_wake_jiffies;
	bool sticky;
	atomic_t score;
	atomic64_t wakeups;
	atomic64_t hits;
};

struct ax_bs_auto_state {
	unsigned long input_until;
	unsigned long owner_until;
	unsigned long max_until;
	pid_t tgid;
	int cpu;
	bool systemui;
};

struct ax_bs_task_scene {
	bool guard;
	bool visual;
	bool display_support;
	bool offscreen;
};

#if defined(AX_BS_HAS_LOCK_BOOST)
struct ax_bs_lock_target {
	pid_t tid;
	struct pid *pid_ref;
	unsigned long expire_jiffies;
	unsigned int util;
};
#endif

struct ax_bs_io_guard {
	pid_t tid;
	struct pid *pid_ref;
	int old_ioprio;
	int guard_ioprio;
	unsigned long expire_jiffies;
};

static unsigned int ax_bs_enabled = 1;
module_param_named(enabled, ax_bs_enabled, uint, 0644);

static unsigned int ax_bs_launch_util = AX_BS_DEFAULT_LAUNCH_UTIL;
module_param_named(launch_util, ax_bs_launch_util, uint, 0644);

static unsigned int ax_bs_animation_util = AX_BS_DEFAULT_ANIMATION_UTIL;
module_param_named(animation_util, ax_bs_animation_util, uint, 0644);

static unsigned int ax_bs_remote_util = AX_BS_DEFAULT_REMOTE_UTIL;
module_param_named(remote_util, ax_bs_remote_util, uint, 0644);

static unsigned int ax_bs_top_app_util = AX_BS_DEFAULT_TOP_APP_UTIL;
module_param_named(top_app_util, ax_bs_top_app_util, uint, 0644);

static unsigned int ax_bs_perf_util = AX_BS_DEFAULT_PERF_UTIL;
module_param_named(perf_util, ax_bs_perf_util, uint, 0644);

static unsigned int ax_bs_system_server_util = AX_BS_DEFAULT_SYSTEM_SERVER_UTIL;
module_param_named(system_server_util, ax_bs_system_server_util, uint, 0644);

static unsigned int ax_bs_system_ui_util = AX_BS_DEFAULT_SYSTEM_UI_UTIL;
module_param_named(system_ui_util, ax_bs_system_ui_util, uint, 0644);

static unsigned int ax_bs_launcher_util = AX_BS_DEFAULT_LAUNCHER_UTIL;
module_param_named(launcher_util, ax_bs_launcher_util, uint, 0644);

static unsigned int ax_bs_top_role_util = AX_BS_DEFAULT_TOP_ROLE_UTIL;
module_param_named(top_role_util, ax_bs_top_role_util, uint, 0644);

static unsigned int ax_bs_scene_util = AX_BS_DEFAULT_SCENE_UTIL;
module_param_named(scene_util, ax_bs_scene_util, uint, 0644);

static unsigned int ax_bs_support_worker_util = AX_BS_DEFAULT_SUPPORT_WORKER_UTIL;
module_param_named(support_worker_util, ax_bs_support_worker_util, uint, 0644);

static unsigned int ax_bs_launch_worker_util = AX_BS_DEFAULT_LAUNCH_WORKER_UTIL;
module_param_named(launch_worker_util, ax_bs_launch_worker_util, uint, 0644);

static unsigned int ax_bs_util_cap = AX_BS_DEFAULT_UTIL_CAP;
module_param_named(util_cap, ax_bs_util_cap, uint, 0644);

static unsigned int ax_bs_initial_score = AX_BS_DEFAULT_INITIAL_SCORE;
module_param_named(initial_score, ax_bs_initial_score, uint, 0644);

static unsigned int ax_bs_score_gain = AX_BS_DEFAULT_SCORE_GAIN;
module_param_named(score_gain, ax_bs_score_gain, uint, 0644);

static unsigned int ax_bs_score_decay = AX_BS_DEFAULT_SCORE_DECAY;
module_param_named(score_decay, ax_bs_score_decay, uint, 0644);

static unsigned int ax_bs_score_window_ms = AX_BS_DEFAULT_SCORE_WINDOW_MS;
module_param_named(score_window_ms, ax_bs_score_window_ms, uint, 0644);

static unsigned int ax_bs_max_duration_ms = AX_BS_DEFAULT_MAX_DURATION_MS;
module_param_named(max_duration_ms, ax_bs_max_duration_ms, uint, 0644);

static unsigned int ax_bs_big_only = 1;
module_param_named(big_only, ax_bs_big_only, uint, 0644);

static unsigned int ax_bs_fallback_big = 1;
module_param_named(fallback_big, ax_bs_fallback_big, uint, 0644);

static unsigned int ax_bs_migration_assist = 1;
module_param_named(migration_assist, ax_bs_migration_assist, uint, 0644);

static unsigned int ax_bs_migration_hold_ms = AX_BS_DEFAULT_MIGRATION_HOLD_MS;
module_param_named(migration_hold_ms, ax_bs_migration_hold_ms, uint, 0644);

static unsigned int ax_bs_top_app_hot_score = AX_BS_DEFAULT_TOP_APP_HOT_SCORE;
module_param_named(top_app_hot_score, ax_bs_top_app_hot_score, uint, 0644);

static unsigned int ax_bs_top_app_prio = AX_BS_DEFAULT_TOP_APP_PRIO;
module_param_named(top_app_prio, ax_bs_top_app_prio, uint, 0644);

static unsigned int ax_bs_systemui_preempts_top_app =
	AX_BS_DEFAULT_SYSTEMUI_PREEMPTS_TOP_APP;
module_param_named(systemui_preempts_top_app, ax_bs_systemui_preempts_top_app,
		   uint, 0644);

static unsigned int ax_bs_spread_active = 1;
module_param_named(spread_active, ax_bs_spread_active, uint, 0644);

static unsigned int ax_bs_spread_window_ms = AX_BS_DEFAULT_SPREAD_WINDOW_MS;
module_param_named(spread_window_ms, ax_bs_spread_window_ms, uint, 0644);

static unsigned int ax_bs_spill_pressure = AX_BS_DEFAULT_SPILL_PRESSURE;
module_param_named(spill_pressure, ax_bs_spill_pressure, uint, 0644);

static unsigned int ax_bs_auto_top_app = 1;
module_param_named(auto_top_app, ax_bs_auto_top_app, uint, 0644);

static unsigned int ax_bs_auto_top_app_util = AX_BS_DEFAULT_AUTO_TOP_APP_UTIL;
module_param_named(auto_top_app_util, ax_bs_auto_top_app_util, uint, 0644);

static unsigned int ax_bs_auto_top_app_hold_ms =
	AX_BS_DEFAULT_AUTO_TOP_APP_HOLD_MS;
module_param_named(auto_top_app_hold_ms, ax_bs_auto_top_app_hold_ms, uint,
		   0644);

#if defined(AX_BS_HAS_INPUT_HOOK)
static unsigned int ax_bs_auto_top_app_max_ms = 800;
module_param_named(auto_top_app_max_ms, ax_bs_auto_top_app_max_ms, uint, 0644);
#endif

static unsigned int ax_bs_reclaim_guard = 1;
module_param_named(reclaim_guard, ax_bs_reclaim_guard, uint, 0644);

static unsigned int ax_bs_background_guard = 1;
module_param_named(background_guard, ax_bs_background_guard, uint, 0644);

static unsigned int ax_bs_scene_focus = 1;
module_param_named(scene_focus, ax_bs_scene_focus, uint, 0644);

#if defined(AX_BS_HAS_IOPRIO)
static unsigned int ax_bs_io_guard = 1;
#else
static unsigned int ax_bs_io_guard;
#endif
module_param_named(io_guard, ax_bs_io_guard, uint, 0644);

static unsigned int ax_bs_io_guard_class = AX_BS_DEFAULT_IO_GUARD_CLASS;
module_param_named(io_guard_class, ax_bs_io_guard_class, uint, 0644);

static unsigned int ax_bs_io_guard_prio = AX_BS_DEFAULT_IO_GUARD_PRIO;
module_param_named(io_guard_prio, ax_bs_io_guard_prio, uint, 0644);

static unsigned int ax_bs_io_guard_duration_ms = AX_BS_DEFAULT_IO_GUARD_DURATION_MS;
module_param_named(io_guard_duration_ms, ax_bs_io_guard_duration_ms, uint, 0644);

static unsigned int ax_bs_io_guard_scan_ms = AX_BS_DEFAULT_IO_GUARD_SCAN_MS;
module_param_named(io_guard_scan_ms, ax_bs_io_guard_scan_ms, uint, 0644);

#if defined(AX_BS_HAS_IOLIMIT_HOOK)
static unsigned int ax_bs_io_limit_delay_ms = AX_BS_DEFAULT_IO_LIMIT_DELAY_MS;
module_param_named(io_limit_delay_ms, ax_bs_io_limit_delay_ms, uint, 0644);

static unsigned int ax_bs_io_limit_min_bytes = AX_BS_DEFAULT_IO_LIMIT_MIN_BYTES;
module_param_named(io_limit_min_bytes, ax_bs_io_limit_min_bytes, uint, 0644);
#endif

#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
static unsigned int ax_bs_gpu_context_boost = 1;
module_param_named(gpu_context_boost, ax_bs_gpu_context_boost, uint, 0644);

static unsigned int ax_bs_gpu_context_boost_ms =
	AX_BS_DEFAULT_GPU_CONTEXT_BOOST_MS;
module_param_named(gpu_context_boost_ms, ax_bs_gpu_context_boost_ms, uint,
		   0644);
#endif

#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
static unsigned int ax_bs_dynamic_svp = 1;
module_param_named(dynamic_svp, ax_bs_dynamic_svp, uint, 0644);

static unsigned int ax_bs_dynamic_svp_min_rank =
	AX_BS_DEFAULT_DYNAMIC_SVP_MIN_RANK;
module_param_named(dynamic_svp_min_rank, ax_bs_dynamic_svp_min_rank, uint,
		   0644);

static unsigned int ax_bs_dynamic_svp_min_delta =
	AX_BS_DEFAULT_DYNAMIC_SVP_MIN_DELTA;
module_param_named(dynamic_svp_min_delta, ax_bs_dynamic_svp_min_delta, uint,
		   0644);
#endif

#if defined(AX_BS_HAS_LOCK_BOOST)
static unsigned int ax_bs_lock_boost = 1;
module_param_named(lock_boost, ax_bs_lock_boost, uint, 0644);

static unsigned int ax_bs_lock_util = AX_BS_DEFAULT_LOCK_UTIL;
module_param_named(lock_util, ax_bs_lock_util, uint, 0644);

static unsigned int ax_bs_lock_max_util = AX_BS_DEFAULT_LOCK_MAX_UTIL;
module_param_named(lock_max_util, ax_bs_lock_max_util, uint, 0644);

static unsigned int ax_bs_lock_timeout_ms = AX_BS_DEFAULT_LOCK_TIMEOUT_MS;
module_param_named(lock_timeout_ms, ax_bs_lock_timeout_ms, uint, 0644);
#endif

#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
static unsigned int ax_bs_freq_assist = 1;
module_param_named(freq_assist, ax_bs_freq_assist, uint, 0644);
#endif

static DEFINE_RAW_SPINLOCK(ax_bs_lock);
static DEFINE_MUTEX(ax_bs_io_guard_lock);
static struct delayed_work ax_bs_cleanup_work;
static struct delayed_work ax_bs_io_guard_work;
static struct proc_dir_entry *ax_bs_proc_dir;
static struct ax_bs_target ax_bs_targets[AX_BS_MAX_TARGETS];
static struct ax_bs_io_guard ax_bs_io_guards[AX_BS_MAX_IO_GUARDS];
static atomic_t ax_bs_composer_tgid = ATOMIC_INIT(-1);
#if defined(AX_BS_HAS_LOCK_BOOST)
static struct ax_bs_lock_target ax_bs_lock_targets[AX_BS_MAX_LOCK_TARGETS];
#endif
static unsigned int ax_bs_cpu_score[NR_CPUS];
static unsigned int ax_bs_cpu_capacity[NR_CPUS];
static bool ax_bs_cpu_capacity_valid[NR_CPUS];
static atomic_t ax_bs_cpu_pressure[NR_CPUS];
static unsigned long ax_bs_cpu_pressure_jiffies[NR_CPUS];
#if defined(AX_BS_HAS_INPUT_HOOK)
static DEFINE_RAW_SPINLOCK(ax_bs_auto_lock);
static seqcount_t ax_bs_auto_seq = SEQCNT_ZERO(ax_bs_auto_seq);
static struct ax_bs_auto_state ax_bs_auto_state = {
	.tgid = -1,
	.cpu = AX_SCHED_CPU_NONE,
};
static bool ax_bs_input_hook_registered;
#endif
static atomic64_t ax_bs_session_updates;
static atomic64_t ax_bs_session_clears;
static atomic64_t ax_bs_session_prunes;
static atomic64_t ax_bs_wake_assists;
static atomic64_t ax_bs_uclamp_assists;
static atomic64_t ax_bs_fallback_assists;
static atomic64_t ax_bs_migration_blocks;
static atomic64_t ax_bs_migration_releases;
static atomic64_t ax_bs_enqueue_events;
static atomic64_t ax_bs_dequeue_events;
static atomic64_t ax_bs_binder_assists;
static atomic64_t ax_bs_reclaim_assists;
static atomic64_t ax_bs_support_worker_assists;
static atomic64_t ax_bs_background_guard_assists;
#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
static atomic64_t ax_bs_dynamic_svp_preempts;
static bool ax_bs_dynamic_svp_hook_registered;
#endif
#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
static bool ax_bs_cpu_capacity_hook_registered;
static bool ax_bs_cpu_capacity_ready;
#endif
#if defined(AX_BS_HAS_IOLIMIT_HOOK)
static atomic64_t ax_bs_io_limit_delays;
static bool ax_bs_iolimit_hook_registered;
#endif
#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
static atomic64_t ax_bs_gpu_context_boosts;
static bool ax_bs_gpu_context_hook_registered;
#endif
#if defined(AX_BS_HAS_LOCK_BOOST)
static atomic_t ax_bs_lock_active_count;
static atomic64_t ax_bs_lock_assists;
static atomic64_t ax_bs_lock_hits;
static atomic64_t ax_bs_lock_wake_assists;
static atomic64_t ax_bs_lock_fallback_assists;
static atomic64_t ax_bs_lock_migration_blocks;
static atomic64_t ax_bs_lock_uclamp_assists;
static atomic64_t ax_bs_lock_prunes;
#endif
#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
static atomic64_t ax_bs_freq_assists;
#endif

static void ax_bs_io_guard_kick(void);

static unsigned int ax_bs_clamp_score(unsigned int score)
{
	return min_t(unsigned int, score, AX_BS_SCORE_MAX);
}

static unsigned int ax_bs_base_util(int mode)
{
	switch (mode) {
	case AX_BS_MODE_LAUNCH:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_launch_util));
	case AX_BS_MODE_REMOTE:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_remote_util));
	case AX_BS_MODE_ANIMATION:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_animation_util));
	case AX_BS_MODE_TOP_APP:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_top_app_util));
	case AX_BS_MODE_PERF:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_perf_util));
	default:
		return 0;
	}
}

static unsigned int ax_bs_role_util(int role, int mode)
{
	switch (role) {
	case AX_BS_ROLE_SYSTEM_SERVER:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_system_server_util));
	case AX_BS_ROLE_SYSTEM_UI:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_system_ui_util));
	case AX_BS_ROLE_LAUNCHER:
		return ax_sched_clamp_util(READ_ONCE(ax_bs_launcher_util));
	case AX_BS_ROLE_TOP_APP:
		if (mode == AX_BS_MODE_TOP_APP)
			return 0;
		return ax_sched_clamp_util(READ_ONCE(ax_bs_top_role_util));
	default:
		return 0;
	}
}

static unsigned int ax_bs_resource_mask(int mode, int source, int severity)
{
	unsigned int resources = AX_BOOST_PMQOS;

	if (mode == AX_BS_MODE_TOP_APP)
		return 0;

	if (mode == AX_BS_MODE_PERF) {
		switch (source) {
		case AX_BS_SOURCE_POWER_DISPLAY:
			return AX_BOOST_PMQOS;
		case AX_BS_SOURCE_ADPF_GPU:
		case AX_BS_SOURCE_POWER_RENDER:
			return AX_BOOST_GPU | AX_BOOST_MEMLAT;
		case AX_BS_SOURCE_GAME:
			return AX_BOOST_GPU | AX_BOOST_MEMLAT |
				AX_BOOST_DDR | AX_BOOST_L3;
		case AX_BS_SOURCE_TOP_APP_SWITCH:
			return AX_BOOST_PMQOS | AX_BOOST_GPU |
				AX_BOOST_MEMLAT | AX_BOOST_DDR |
				AX_BOOST_L3;
		case AX_BS_SOURCE_POWER_LAUNCH:
		case AX_BS_SOURCE_GAME_LOADING:
			return AX_BOOST_PMQOS | AX_BOOST_GPU |
				AX_BOOST_MEMLAT | AX_BOOST_DDR;
		default:
			return 0;
		}
	}

	if (source == AX_BS_SOURCE_START_ACTIVITY_BINDER)
		return resources;

	if (mode == AX_BS_MODE_LAUNCH || severity >= AX_BS_SEVERITY_HEAVY)
		return resources | AX_BOOST_GPU |
			AX_BOOST_MEMLAT | AX_BOOST_DDR |
			AX_BOOST_L3;

	if (mode == AX_BS_MODE_ANIMATION || mode == AX_BS_MODE_REMOTE)
		resources |= AX_BOOST_GPU | AX_BOOST_MEMLAT |
			AX_BOOST_DDR;

	return resources;
}

#if defined(AX_BS_HAS_GAME_BOOST)
static unsigned int ax_bs_game_profile(int mode, int source)
{
	if (mode != AX_BS_MODE_PERF)
		return 0;

	switch (source) {
	case AX_BS_SOURCE_ADPF_CPU:
		return AX_GAME_BOOST_PROFILE_CPU;
	case AX_BS_SOURCE_ADPF_GPU:
	case AX_BS_SOURCE_POWER_RENDER:
		return AX_GAME_BOOST_PROFILE_RENDER;
	case AX_BS_SOURCE_POWER_LAUNCH:
	case AX_BS_SOURCE_GAME_LOADING:
		return AX_GAME_BOOST_PROFILE_LOADING;
	case AX_BS_SOURCE_GAME:
		return AX_GAME_BOOST_PROFILE_SUSTAINED;
	default:
		return 0;
	}
}

static void ax_bs_update_game_boost(pid_t pid, int mode, int source,
				    int severity, unsigned int duration_ms,
				    bool sticky, int tid_count, pid_t *tids)
{
	unsigned int profile = ax_bs_game_profile(mode, source);

	if (!profile)
		return;

	ax_game_boost_update(pid, profile, severity, duration_ms, sticky, tid_count, tids);
}
#endif

static bool ax_bs_pid_alive(struct pid *pid)
{
	bool alive;

	rcu_read_lock();
	alive = pid_task(pid, PIDTYPE_PID) != NULL;
	rcu_read_unlock();

	return alive;
}

static bool ax_bs_target_active(struct ax_bs_target *target)
{
	return READ_ONCE(target->pid) > 0 &&
		READ_ONCE(target->mode) > 0 &&
		(READ_ONCE(target->sticky) ||
		 time_before(jiffies, READ_ONCE(target->expire_jiffies)));
}

static bool ax_bs_has_transient_role(int role)
{
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];

		if (!ax_bs_target_active(target))
			continue;
		if (READ_ONCE(target->mode) == AX_BS_MODE_TOP_APP)
			continue;
		if (READ_ONCE(target->role) == role)
			return true;
	}

	return false;
}

static bool ax_bs_target_alive(struct ax_bs_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	return ax_bs_target_active(target) && pid && ax_bs_pid_alive(pid);
}

static struct ax_bs_target *ax_bs_find_target(pid_t pid)
{
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		if (READ_ONCE(ax_bs_targets[i].pid) == pid)
			return &ax_bs_targets[i];
	}

	return NULL;
}

static struct ax_bs_target *ax_bs_alloc_target(pid_t pid)
{
	struct ax_bs_target *target;
	int i;

	target = ax_bs_find_target(pid);
	if (target)
		return target;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		target = &ax_bs_targets[i];
		if (READ_ONCE(target->pid) <= 0 || !ax_bs_target_alive(target))
			return target;
	}

	return NULL;
}

static void ax_bs_release_pid(struct pid *pid)
{
	if (pid)
		put_pid(pid);
}

static struct pid *ax_bs_detach_target(struct ax_bs_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	write_seqcount_begin(&target->cpu_seq);
	WRITE_ONCE(target->pid, -1);
	WRITE_ONCE(target->cpu, AX_SCHED_CPU_NONE);
	write_seqcount_end(&target->cpu_seq);
	WRITE_ONCE(target->uid, -1);
	WRITE_ONCE(target->mode, 0);
	WRITE_ONCE(target->source, 0);
	WRITE_ONCE(target->severity, 0);
	WRITE_ONCE(target->role, AX_BS_ROLE_APP);
	WRITE_ONCE(target->resource_source, 0);
	WRITE_ONCE(target->tid_count, 0);
	WRITE_ONCE(target->expire_jiffies, 0);
	WRITE_ONCE(target->last_wake_jiffies, 0);
	WRITE_ONCE(target->sticky, false);
	atomic_set(&target->score, 0);
	atomic64_set(&target->wakeups, 0);
	atomic64_set(&target->hits, 0);
	memset(target->tids, 0, sizeof(target->tids));
	smp_wmb();
	WRITE_ONCE(target->pid_ref, NULL);

	return pid;
}

static void ax_bs_add_clear_source(unsigned int *sources, int *count,
				   unsigned int source)
{
	int i;

	if (!sources || !count || !source)
		return;

	for (i = 0; i < *count; i++) {
		if (sources[i] == source)
			return;
	}

	if (*count < AX_BS_MAX_TARGETS)
		sources[(*count)++] = source;
}

static void ax_bs_add_clear_pid(pid_t *pids, int *count, pid_t pid)
{
	int i;

	if (!pids || !count || pid <= 0)
		return;

	for (i = 0; i < *count; i++) {
		if (pids[i] == pid)
			return;
	}

	if (*count < AX_BS_MAX_TARGETS)
		pids[(*count)++] = pid;
}

static void ax_bs_clear_target(struct ax_bs_target *target,
			       unsigned int *sources, int *source_count,
			       pid_t *pids, int *pid_count)
{
	unsigned int resource_source = READ_ONCE(target->resource_source);
	pid_t pid = READ_ONCE(target->pid);

	if (READ_ONCE(target->pid) > 0)
		atomic64_inc(&ax_bs_session_clears);
	ax_bs_add_clear_source(sources, source_count, resource_source);
	ax_bs_add_clear_pid(pids, pid_count, pid);
	ax_bs_release_pid(ax_bs_detach_target(target));
}

static void ax_bs_clear_locked(pid_t pid, unsigned int *sources,
			       int *source_count, pid_t *pids, int *pid_count)
{
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];

		if (pid > 0 && READ_ONCE(target->pid) != pid)
			continue;

		ax_bs_clear_target(target, sources, source_count, pids, pid_count);
	}
}

#if defined(AX_BS_HAS_LOCK_BOOST)
static void ax_bs_prune_lock_targets_locked(void);
#endif

static void ax_bs_prune_locked(unsigned int *sources, int *source_count,
			       pid_t *pids, int *pid_count)
{
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		struct pid *pid = READ_ONCE(target->pid_ref);

		if (READ_ONCE(target->pid) > 0 &&
		    (!pid ||
		     (!READ_ONCE(target->sticky) &&
		      time_after_eq(jiffies, READ_ONCE(target->expire_jiffies))) ||
		     !ax_bs_pid_alive(pid))) {
			atomic64_inc(&ax_bs_session_prunes);
			ax_bs_clear_target(target, sources, source_count,
					   pids, pid_count);
		}
	}

#if defined(AX_BS_HAS_LOCK_BOOST)
	ax_bs_prune_lock_targets_locked();
#endif
}

static struct pid *ax_bs_set_target_locked(struct ax_bs_target *target, pid_t pid,
					   int uid, int mode, int source,
					   int severity, int role,
					   int requested_tids, char **argv,
					   int argc, struct pid *pid_ref,
					   unsigned long expire_jiffies,
					   bool sticky)
{
	bool same_pid = ax_bs_target_active(target) &&
		READ_ONCE(target->pid) == pid;
	bool preserve = same_pid &&
		READ_ONCE(target->mode) == mode &&
		READ_ONCE(target->source) == source &&
		READ_ONCE(target->severity) == severity &&
		READ_ONCE(target->role) == role &&
		READ_ONCE(target->sticky) == sticky;
	unsigned long last_wake_jiffies = preserve ?
		READ_ONCE(target->last_wake_jiffies) : jiffies;
	unsigned int score = preserve ? atomic_read(&target->score) :
		ax_bs_clamp_score(READ_ONCE(ax_bs_initial_score));
	s64 wakeups = preserve ? atomic64_read(&target->wakeups) : 0;
	s64 hits = preserve ? atomic64_read(&target->hits) : 0;
	int cpu = same_pid ? READ_ONCE(target->cpu) : AX_SCHED_CPU_NONE;
	struct pid *old_pid = ax_bs_detach_target(target);
	int parsed;
	int i;

	WRITE_ONCE(target->uid, uid);
	WRITE_ONCE(target->mode, mode);
	WRITE_ONCE(target->source, source);
	WRITE_ONCE(target->severity, severity);
	WRITE_ONCE(target->role, role);
	WRITE_ONCE(target->resource_source, AX_BOOST_SOURCE_BURST_BASE +
		   (unsigned int)(target - ax_bs_targets));
	WRITE_ONCE(target->tid_count, 0);
	WRITE_ONCE(target->expire_jiffies, expire_jiffies);
	WRITE_ONCE(target->last_wake_jiffies, last_wake_jiffies);
	WRITE_ONCE(target->sticky, sticky);
	atomic_set(&target->score, ax_bs_clamp_score(score));
	atomic64_set(&target->wakeups, wakeups);
	atomic64_set(&target->hits, hits);
	memset(target->tids, 0, sizeof(target->tids));
	for (i = 0; i < requested_tids && i + 7 < argc &&
	     target->tid_count < AX_BS_MAX_TIDS; i++) {
		if (kstrtoint(strstrip(argv[i + 7]), 10, &parsed) || parsed <= 0)
			continue;
		target->tids[target->tid_count++] = parsed;
	}
	WRITE_ONCE(target->pid_ref, pid_ref);
	smp_wmb();
	write_seqcount_begin(&target->cpu_seq);
	WRITE_ONCE(target->cpu, cpu);
	WRITE_ONCE(target->pid, pid);
	write_seqcount_end(&target->cpu_seq);
	atomic64_inc(&ax_bs_session_updates);

	return old_pid;
}

#if defined(AX_BS_HAS_LOCK_BOOST)
static bool ax_bs_lock_target_active(struct ax_bs_lock_target *target)
{
	return READ_ONCE(target->tid) > 0 &&
		READ_ONCE(target->util) > 0 &&
		time_before(jiffies, READ_ONCE(target->expire_jiffies));
}

static bool ax_bs_lock_target_alive(struct ax_bs_lock_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	return ax_bs_lock_target_active(target) && pid && ax_bs_pid_alive(pid);
}

static struct pid *ax_bs_detach_lock_target(struct ax_bs_lock_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	if (READ_ONCE(target->tid) > 0 && READ_ONCE(target->util) > 0 &&
	    atomic_read(&ax_bs_lock_active_count) > 0)
		atomic_dec(&ax_bs_lock_active_count);
	WRITE_ONCE(target->tid, -1);
	WRITE_ONCE(target->expire_jiffies, 0);
	WRITE_ONCE(target->util, 0);
	smp_wmb();
	WRITE_ONCE(target->pid_ref, NULL);

	return pid;
}

static void ax_bs_clear_lock_target(struct ax_bs_lock_target *target)
{
	ax_bs_release_pid(ax_bs_detach_lock_target(target));
}

static void ax_bs_prune_lock_targets_locked(void)
{
	int i;

	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		struct ax_bs_lock_target *target = &ax_bs_lock_targets[i];
		struct pid *pid = READ_ONCE(target->pid_ref);

		if (READ_ONCE(target->tid) <= 0)
			continue;
		if (pid && ax_bs_lock_target_active(target) && ax_bs_pid_alive(pid))
			continue;

		atomic64_inc(&ax_bs_lock_prunes);
		ax_bs_clear_lock_target(target);
	}
}

static struct ax_bs_lock_target *ax_bs_find_lock_target_locked(pid_t tid)
{
	int i;

	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		if (READ_ONCE(ax_bs_lock_targets[i].tid) == tid)
			return &ax_bs_lock_targets[i];
	}

	return NULL;
}

static struct ax_bs_lock_target *ax_bs_alloc_lock_target_locked(pid_t tid)
{
	struct ax_bs_lock_target *target;
	int i;

	target = ax_bs_find_lock_target_locked(tid);
	if (target)
		return target;

	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		target = &ax_bs_lock_targets[i];
		if (READ_ONCE(target->tid) <= 0 || !ax_bs_lock_target_alive(target)) {
			ax_bs_clear_lock_target(target);
			return target;
		}
	}

	return NULL;
}

static struct pid *ax_bs_set_lock_target_locked(struct ax_bs_lock_target *target,
						pid_t tid, struct pid *pid,
						unsigned int util,
						unsigned long expire_jiffies)
{
	struct pid *old_pid = ax_bs_detach_lock_target(target);

	WRITE_ONCE(target->pid_ref, pid);
	WRITE_ONCE(target->util, util);
	WRITE_ONCE(target->expire_jiffies, expire_jiffies);
	atomic_inc(&ax_bs_lock_active_count);
	smp_wmb();
	WRITE_ONCE(target->tid, tid);

	return old_pid;
}

static unsigned long ax_bs_lock_target_delay(struct ax_bs_lock_target *target)
{
	unsigned long expire = READ_ONCE(target->expire_jiffies);

	if (time_after(expire, jiffies))
		return max_t(unsigned long, expire - jiffies, 1);
	return 1;
}
#endif

static unsigned long ax_bs_min_delay(unsigned long delay, unsigned long next)
{
	if (!next)
		return delay;
	if (!delay || next < delay)
		return next;
	return delay;
}

static unsigned long ax_bs_target_delay(struct ax_bs_target *target)
{
	unsigned long expire = READ_ONCE(target->expire_jiffies);

	if (READ_ONCE(target->sticky))
		return msecs_to_jiffies(AX_BS_CLEANUP_INTERVAL_MS);
	if (time_after(expire, jiffies))
		return max_t(unsigned long, expire - jiffies, 1);
	return 1;
}

static unsigned long ax_bs_next_delay_locked(void)
{
	unsigned long delay = 0;
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		if (ax_bs_target_alive(&ax_bs_targets[i]))
			delay = ax_bs_min_delay(delay,
						ax_bs_target_delay(&ax_bs_targets[i]));
	}
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		if (ax_bs_lock_target_alive(&ax_bs_lock_targets[i]))
			delay = ax_bs_min_delay(delay,
						ax_bs_lock_target_delay(&ax_bs_lock_targets[i]));
	}
#endif

	return delay;
}

static void ax_bs_schedule_cleanup(unsigned long delay)
{
	if (delay)
		mod_delayed_work(system_wq, &ax_bs_cleanup_work, delay);
	else
		cancel_delayed_work(&ax_bs_cleanup_work);
}

static void ax_bs_cleanup_work_fn(struct work_struct *work)
{
	unsigned int sources[AX_BS_MAX_TARGETS];
	pid_t pids[AX_BS_MAX_TARGETS];
	unsigned long flags;
	unsigned long delay;
	int source_count = 0;
	int pid_count = 0;
	int i;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	ax_bs_prune_locked(sources, &source_count, pids, &pid_count);
	delay = ax_bs_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);

	for (i = 0; i < pid_count; i++) {
		ax_sched_thread_snooper_track(pids[i], false);
#if defined(AX_BS_HAS_GAME_BOOST)
		ax_game_boost_clear(pids[i]);
#endif
	}
	for (i = 0; i < source_count; i++)
		ax_sched_boost_clear(sources[i]);
	ax_bs_io_guard_kick();
	ax_bs_schedule_cleanup(delay);
}

static bool ax_bs_target_score_active(struct ax_bs_target *target)
{
	unsigned long last;
	unsigned long window;

	if (!target)
		return false;

	last = READ_ONCE(target->last_wake_jiffies);
	if (!last)
		return false;

	window = max_t(unsigned long,
		       msecs_to_jiffies(max_t(unsigned int,
					      READ_ONCE(ax_bs_score_window_ms),
					      1)),
		       1);
	return time_before(jiffies, last + window);
}

static unsigned int ax_bs_target_util(struct ax_bs_target *target)
{
	unsigned int base;
	unsigned int score;
	unsigned int cap;

	if (!target || !READ_ONCE(ax_bs_enabled) || !ax_bs_target_active(target))
		return 0;

	base = ax_bs_base_util(READ_ONCE(target->mode));
	if (!base)
		return 0;

	base = max_t(unsigned int, base, ax_bs_role_util(READ_ONCE(target->role),
							READ_ONCE(target->mode)));
	if (READ_ONCE(target->mode) != AX_BS_MODE_TOP_APP &&
	    READ_ONCE(target->severity) >= AX_BS_SEVERITY_HEAVY)
		base = max_t(unsigned int, base,
			     ax_sched_clamp_util(READ_ONCE(ax_bs_scene_util)));
	cap = ax_sched_clamp_util(READ_ONCE(ax_bs_util_cap));
	score = ax_bs_target_score_active(target) ?
		ax_bs_clamp_score(atomic_read(&target->score)) : 0;
	if (cap <= base)
		return cap;

	return min_t(unsigned int, base + score * (cap - base) / AX_BS_SCORE_MAX, cap);
}

static unsigned int ax_bs_mode_rank(int mode)
{
	switch (mode) {
	case AX_BS_MODE_LAUNCH:
		return AX_BS_RANK_LAUNCH;
	case AX_BS_MODE_ANIMATION:
	case AX_BS_MODE_REMOTE:
		return AX_BS_RANK_VISUAL;
	case AX_BS_MODE_TOP_APP:
		return AX_BS_RANK_TOP_APP;
	case AX_BS_MODE_PERF:
		return AX_BS_RANK_DISPLAY;
	default:
		return 0;
	}
}

static unsigned int ax_bs_role_rank(int role)
{
	switch (role) {
	case AX_BS_ROLE_SYSTEM_SERVER:
		return AX_BS_RANK_SYSTEM;
	case AX_BS_ROLE_SYSTEM_UI:
	case AX_BS_ROLE_LAUNCHER:
		return AX_BS_RANK_LAUNCH;
	case AX_BS_ROLE_TOP_APP:
		return AX_BS_RANK_VISUAL;
	default:
		return 0;
	}
}

static unsigned int ax_bs_source_rank(int source)
{
	switch (source) {
	case AX_BS_SOURCE_FRAME_RESCUE:
		return AX_BS_RANK_FRAME;
	case AX_BS_SOURCE_LAUNCH:
	case AX_BS_SOURCE_POWER_LAUNCH:
	case AX_BS_SOURCE_GAME_LOADING:
	case AX_BS_SOURCE_TOP_APP_SWITCH:
		return AX_BS_RANK_LAUNCH;
	case AX_BS_SOURCE_ANIMATION:
	case AX_BS_SOURCE_REMOTE:
	case AX_BS_SOURCE_START_ACTIVITY_BINDER:
		return AX_BS_RANK_VISUAL;
	case AX_BS_SOURCE_POWER_RENDER:
	case AX_BS_SOURCE_ADPF_GPU:
	case AX_BS_SOURCE_GAME:
		return AX_BS_RANK_DISPLAY;
	case AX_BS_SOURCE_ADPF_CPU:
	case AX_BS_SOURCE_POWER_INTERACTION:
	case AX_BS_SOURCE_POWER_DISPLAY:
		return AX_BS_RANK_TOP_APP;
	default:
		return 0;
	}
}

static unsigned int ax_bs_target_rank(struct ax_bs_target *target)
{
	unsigned int rank;

	if (!target || !READ_ONCE(ax_bs_enabled) || !ax_bs_target_active(target))
		return 0;

	rank = max_t(unsigned int, ax_bs_mode_rank(READ_ONCE(target->mode)),
		     ax_bs_role_rank(READ_ONCE(target->role)));
	rank = max_t(unsigned int, rank,
		     ax_bs_source_rank(READ_ONCE(target->source)));
	if (READ_ONCE(target->severity) >= AX_BS_SEVERITY_HEAVY)
		rank = max_t(unsigned int, rank, AX_BS_RANK_LAUNCH);
	if (ax_bs_target_score_active(target) &&
	    atomic_read(&target->score) >=
		    ax_bs_clamp_score(READ_ONCE(ax_bs_top_app_hot_score)))
		rank = max_t(unsigned int, rank, AX_BS_RANK_VISUAL);

	return min_t(unsigned int, rank, AX_BS_RANK_FRAME);
}

#if defined(AX_BS_HAS_INPUT_HOOK)
static bool ax_bs_task_in_cpu_group(struct task_struct *task, const char *name)
{
#if defined(CONFIG_CGROUP_SCHED)
	struct cgroup_subsys_state *css;
	bool match = false;

	if (!task || !name)
		return false;

	rcu_read_lock();
	css = task_css(task, cpu_cgrp_id);
	if (css && css->cgroup && css->cgroup->kn)
		match = !strcmp(css->cgroup->kn->name, name);
	rcu_read_unlock();

	return match;
#else
	return false;
#endif
}
#endif

static bool ax_bs_task_is_primary_visual(struct task_struct *task)
{
	if (!task)
		return false;

	return task_pid_nr(task) == task_tgid_nr(task) ||
		!strcmp(task->comm, AX_SCHED_RENDER_THREAD) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_UI) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_DISPLAY) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_ANIM) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_ANIM_LF);
}

#if defined(AX_BS_HAS_INPUT_HOOK)
static void ax_bs_read_auto_state(struct ax_bs_auto_state *state)
{
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&ax_bs_auto_seq);
		state->input_until = READ_ONCE(ax_bs_auto_state.input_until);
		state->owner_until = READ_ONCE(ax_bs_auto_state.owner_until);
		state->max_until = READ_ONCE(ax_bs_auto_state.max_until);
		state->tgid = READ_ONCE(ax_bs_auto_state.tgid);
		state->cpu = READ_ONCE(ax_bs_auto_state.cpu);
		state->systemui = READ_ONCE(ax_bs_auto_state.systemui);
	} while (read_seqcount_retry(&ax_bs_auto_seq, seq));
}

static bool ax_bs_input_window_active(const struct ax_bs_auto_state *state)
{
	return READ_ONCE(ax_bs_input_hook_registered) &&
		time_before(jiffies, state->input_until) &&
		time_before(jiffies, state->max_until);
}

static bool ax_bs_auto_top_app_active(const struct ax_bs_auto_state *state)
{
	return READ_ONCE(ax_bs_input_hook_registered) &&
		READ_ONCE(ax_bs_enabled) && READ_ONCE(ax_bs_auto_top_app) &&
		time_before(jiffies, state->owner_until) &&
		time_before(jiffies, state->max_until);
}

static void ax_bs_note_auto_top_app(struct task_struct *task)
{
	unsigned long flags;
	unsigned long hold;
	unsigned long expires;
	unsigned long now;
	unsigned int hold_ms;
	struct ax_bs_auto_state state;
	bool active;
	bool input_active;
	bool systemui;
	pid_t owner;
	pid_t tgid;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_auto_top_app) || !task)
		return;

	ax_bs_read_auto_state(&state);
	active = ax_bs_auto_top_app_active(&state);
	input_active = ax_bs_input_window_active(&state);
	if (!input_active && !active)
		return;
	if (!ax_bs_task_is_primary_visual(task))
		return;

	tgid = task_tgid_nr(task);
	if (tgid <= 0)
		return;

	if (active && state.tgid == tgid) {
		systemui = state.systemui;
		hold_ms = max_t(unsigned int,
				READ_ONCE(ax_bs_auto_top_app_hold_ms), 1);
		hold = max_t(unsigned long, msecs_to_jiffies(hold_ms), 1);
		now = jiffies;
		if (time_after(state.owner_until,
			       now + max_t(unsigned long, hold / 2, 1)))
			return;
		expires = now + hold;
		if (time_after(expires, state.max_until))
			expires = state.max_until;
		if (!time_after(expires, state.owner_until))
			return;
	} else {
		if (active)
			return;
		systemui = ax_bs_task_in_cpu_group(task, "systemui");
		if (__kuid_val(task_uid(task)) < AX_BS_FIRST_APPLICATION_UID &&
		    !systemui)
			return;
		if (!systemui && !ax_bs_task_in_cpu_group(task, "top-app"))
			return;
	}

	raw_spin_lock_irqsave(&ax_bs_auto_lock, flags);
	state.input_until = READ_ONCE(ax_bs_auto_state.input_until);
	state.owner_until = READ_ONCE(ax_bs_auto_state.owner_until);
	state.max_until = READ_ONCE(ax_bs_auto_state.max_until);
	state.tgid = READ_ONCE(ax_bs_auto_state.tgid);
	state.cpu = READ_ONCE(ax_bs_auto_state.cpu);
	state.systemui = READ_ONCE(ax_bs_auto_state.systemui);
	active = ax_bs_auto_top_app_active(&state);
	input_active = ax_bs_input_window_active(&state);
	owner = state.tgid;
	if (active && owner != tgid)
		goto unlock;
	else if (!active && !input_active)
		goto unlock;
	else if (active)
		systemui = state.systemui;

	hold_ms = max_t(unsigned int, READ_ONCE(ax_bs_auto_top_app_hold_ms), 1);
	hold = max_t(unsigned long, msecs_to_jiffies(hold_ms), 1);
	now = jiffies;
	expires = now + hold;
	if (time_after(expires, state.max_until))
		expires = state.max_until;
	if (!time_after(expires, now) ||
	    (active && owner == tgid &&
	     !time_after(expires, state.owner_until)))
		goto unlock;

	write_seqcount_begin(&ax_bs_auto_seq);
	WRITE_ONCE(ax_bs_auto_state.tgid, tgid);
	if (!active)
		WRITE_ONCE(ax_bs_auto_state.cpu, AX_SCHED_CPU_NONE);
	WRITE_ONCE(ax_bs_auto_state.systemui, systemui);
	WRITE_ONCE(ax_bs_auto_state.owner_until, expires);
	write_seqcount_end(&ax_bs_auto_seq);

unlock:
	raw_spin_unlock_irqrestore(&ax_bs_auto_lock, flags);
}

static void ax_bs_input_sync(void *unused, struct input_dev *dev)
{
	unsigned long flags;
	unsigned long hold;
	unsigned long max_hold;
	unsigned long now;
	unsigned int hold_ms;
	unsigned int max_ms;
	bool new_burst;

	(void)unused;

	if (!dev || !READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_auto_top_app) ||
	    !test_bit(EV_ABS, dev->evbit) ||
	    !test_bit(ABS_MT_POSITION_X, dev->absbit) ||
	    !test_bit(ABS_MT_POSITION_Y, dev->absbit))
		return;

	hold_ms = max_t(unsigned int, READ_ONCE(ax_bs_auto_top_app_hold_ms), 1);
	max_ms = clamp_t(unsigned int, READ_ONCE(ax_bs_auto_top_app_max_ms),
			 1, 2000);
	hold = max_t(unsigned long, msecs_to_jiffies(hold_ms), 1);
	max_hold = max_t(unsigned long, msecs_to_jiffies(max_ms), 1);
	now = jiffies;
	raw_spin_lock_irqsave(&ax_bs_auto_lock, flags);
	new_burst = !time_before(now, READ_ONCE(ax_bs_auto_state.input_until));
	write_seqcount_begin(&ax_bs_auto_seq);
	WRITE_ONCE(ax_bs_auto_state.input_until, now + hold);
	if (new_burst) {
		WRITE_ONCE(ax_bs_auto_state.owner_until, 0);
		WRITE_ONCE(ax_bs_auto_state.max_until, now + max_hold);
		WRITE_ONCE(ax_bs_auto_state.tgid, -1);
		WRITE_ONCE(ax_bs_auto_state.cpu, AX_SCHED_CPU_NONE);
		WRITE_ONCE(ax_bs_auto_state.systemui, false);
	}
	write_seqcount_end(&ax_bs_auto_seq);
	raw_spin_unlock_irqrestore(&ax_bs_auto_lock, flags);
}
#endif

static bool ax_bs_read_active_auto_state(struct ax_bs_auto_state *state)
{
#if defined(AX_BS_HAS_INPUT_HOOK)
	ax_bs_read_auto_state(state);
	return ax_bs_auto_top_app_active(state);
#else
	(void)state;
	return false;
#endif
}

static unsigned int ax_bs_auto_target_util_for(
		const struct ax_bs_auto_state *state)
{
	if (state->systemui)
		return ax_sched_clamp_util(READ_ONCE(ax_bs_system_ui_util));

	return ax_sched_clamp_util(READ_ONCE(ax_bs_auto_top_app_util));
}

static unsigned int ax_bs_auto_target_util(void)
{
	struct ax_bs_auto_state state;

	if (!ax_bs_read_active_auto_state(&state))
		return 0;
	return ax_bs_auto_target_util_for(&state);
}

static bool ax_bs_auto_top_app_task_matches(
		struct task_struct *task, const struct ax_bs_auto_state *state)
{
	return task && state->tgid == task_tgid_nr(task) &&
		ax_bs_task_is_primary_visual(task);
}

static unsigned int ax_bs_auto_top_app_task_util(struct task_struct *task)
{
	struct ax_bs_auto_state state;

	if (!ax_bs_read_active_auto_state(&state) ||
	    !ax_bs_auto_top_app_task_matches(task, &state))
		return 0;

	return ax_bs_auto_target_util_for(&state);
}

static void ax_bs_note_auto_cpu(struct task_struct *task, int cpu)
{
#if defined(AX_BS_HAS_INPUT_HOOK)
	unsigned long flags;
	struct ax_bs_auto_state state;

	if (cpu < 0 || cpu >= nr_cpu_ids || !ax_bs_read_active_auto_state(&state) ||
	    state.cpu == cpu || !ax_bs_auto_top_app_task_matches(task, &state))
		return;

	raw_spin_lock_irqsave(&ax_bs_auto_lock, flags);
	if (ax_bs_auto_top_app_active(&ax_bs_auto_state) &&
	    READ_ONCE(ax_bs_auto_state.tgid) == task_tgid_nr(task) &&
	    READ_ONCE(ax_bs_auto_state.cpu) != cpu) {
		write_seqcount_begin(&ax_bs_auto_seq);
		WRITE_ONCE(ax_bs_auto_state.cpu, cpu);
		write_seqcount_end(&ax_bs_auto_seq);
	}
	raw_spin_unlock_irqrestore(&ax_bs_auto_lock, flags);
#else
	(void)task;
	(void)cpu;
#endif
}

static bool ax_bs_policy_matches_cpu(struct cpufreq_policy *policy, int cpu)
{
#if defined(AX_BS_MAP_UTIL_HAS_POLICY)
	if (!policy)
		return true;

	return cpu >= 0 && cpu < nr_cpu_ids &&
		cpumask_test_cpu(cpu, policy->related_cpus);
#else
	(void)policy;
	(void)cpu;
	return true;
#endif
}

static unsigned long ax_bs_active_target_util(struct cpufreq_policy *policy)
{
	unsigned long util = 0;
	int i;

	if (!READ_ONCE(ax_bs_enabled))
		return 0;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		unsigned int target_util;
		unsigned int seq;
		int cpu;

		do {
			seq = read_seqcount_begin(&target->cpu_seq);
			if (READ_ONCE(target->mode) == AX_BS_MODE_TOP_APP) {
				target_util = 0;
			} else {
				cpu = READ_ONCE(target->cpu);
				if (ax_bs_policy_matches_cpu(policy, cpu))
					target_util = ax_bs_target_util(target);
				else
					target_util = 0;
			}
		} while (read_seqcount_retry(&target->cpu_seq, seq));
		if (target_util)
			util = max_t(unsigned long, util, target_util);
	}
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		if (ax_bs_lock_target_active(&ax_bs_lock_targets[i]))
			util = max_t(unsigned long, util,
				     READ_ONCE(ax_bs_lock_targets[i].util));
	}
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	util = max_t(unsigned long, util, ax_game_boost_active_util());
#endif

	return util;
}

static unsigned long ax_bs_active_scene_util(void)
{
	return max_t(unsigned long, ax_bs_active_target_util(NULL),
		     ax_bs_auto_target_util());
}

static bool ax_bs_has_transient_scene(void)
{
#if defined(AX_BS_HAS_FRAME_BOOST)
	if (READ_ONCE(ax_bs_enabled) && ax_frame_boost_active_util())
		return true;
#endif

	return ax_bs_active_scene_util() != 0;
}

static bool ax_bs_target_recent_wakeup(struct ax_bs_target *target)
{
	unsigned int hold_ms;
	unsigned long last;
	unsigned long window;

	if (!target)
		return false;

	hold_ms = READ_ONCE(ax_bs_migration_hold_ms);
	last = READ_ONCE(target->last_wake_jiffies);
	if (!hold_ms || !last)
		return false;

	window = max_t(unsigned long, msecs_to_jiffies(hold_ms), 1);
	return time_before(jiffies, last + window);
}

static bool ax_bs_should_hold_migration(struct ax_bs_target *target)
{
	unsigned int hot_score;

	if (!target)
		return false;

	if (READ_ONCE(target->mode) != AX_BS_MODE_TOP_APP)
		return true;
	if (!ax_bs_target_recent_wakeup(target))
		return false;

	hot_score = ax_bs_clamp_score(READ_ONCE(ax_bs_top_app_hot_score));
	return atomic_read(&target->score) >= hot_score;
}

static bool ax_bs_task_is_system_helper(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, AX_SCHED_ANDROID_ANIM) ||
		 !strcmp(task->comm, AX_SCHED_ANDROID_ANIM_LF) ||
		 !strcmp(task->comm, AX_SCHED_ANDROID_DISPLAY) ||
		 !strcmp(task->comm, AX_SCHED_ANDROID_UI) ||
		 !strcmp(task->comm, AX_SCHED_POWER_MANAGER) ||
		 !strcmp(task->comm, AX_SCHED_PHOTONIC_MODULATOR) ||
		 !strcmp(task->comm, "InputReader") ||
		 !strcmp(task->comm, "InputDispatcher"));
}

static bool ax_bs_task_is_systemui_background_worker(struct task_struct *task);
static bool ax_bs_task_is_launcher_background_worker(struct task_struct *task);

static bool ax_bs_task_is_launcher_ui_worker(struct task_struct *task)
{
	return task && !strcmp(task->comm, AX_SCHED_LAUNCHER_UI_HELPER);
}

static bool ax_bs_task_is_top_app_important(struct task_struct *task)
{
	unsigned int prio;

	if (!task)
		return false;

	prio = READ_ONCE(ax_bs_top_app_prio);
	return prio < MAX_PRIO && task->prio <= prio;
}

static bool ax_bs_systemui_preempts_target(struct ax_bs_target *target)
{
	struct ax_bs_auto_state state;

	if (!READ_ONCE(ax_bs_systemui_preempts_top_app))
		return false;
	if (READ_ONCE(target->role) != AX_BS_ROLE_TOP_APP ||
	    READ_ONCE(target->mode) == AX_BS_MODE_LAUNCH)
		return false;
	if (ax_bs_read_active_auto_state(&state) && state.systemui)
		return true;

	return ax_bs_has_transient_role(AX_BS_ROLE_SYSTEM_UI);
}

static bool ax_bs_target_matches(struct ax_bs_target *target,
					 struct task_struct *task)
{
	pid_t pid;
	pid_t tid;
	int mode;
	int role;
	int count;
	int i;

	pid = READ_ONCE(target->pid);
	if (pid <= 0 || task_tgid_nr(task) != pid)
		return false;

	tid = task_pid_nr(task);
	count = min_t(int, READ_ONCE(target->tid_count), AX_BS_MAX_TIDS);
	for (i = 0; i < count; i++) {
		if (tid == READ_ONCE(target->tids[i]))
			return true;
	}

	if (READ_ONCE(target->source) == AX_BS_SOURCE_START_ACTIVITY_BINDER)
		return false;

	if (ax_bs_systemui_preempts_target(target))
		return false;

	mode = READ_ONCE(target->mode);
	role = READ_ONCE(target->role);
	if (role == AX_BS_ROLE_SYSTEM_SERVER && ax_bs_task_is_system_helper(task))
		return true;

	if (role == AX_BS_ROLE_LAUNCHER &&
	    ax_bs_task_is_launcher_ui_worker(task))
		return true;

	if (mode == AX_BS_MODE_TOP_APP && role == AX_BS_ROLE_TOP_APP &&
	    ax_bs_task_is_top_app_important(task))
		return true;

	if (tid == pid)
		return true;

	return ax_sched_task_is_render_helper(task);
}

static unsigned int ax_bs_match_target_util(struct task_struct *task,
					    struct ax_bs_target **match,
					    unsigned int *match_rank)
{
	struct ax_bs_target *best = NULL;
	unsigned int best_rank = 0;
	unsigned int best_util = 0;
	int i;

	if (match)
		*match = NULL;
	if (match_rank)
		*match_rank = 0;
	if (!task || !READ_ONCE(ax_bs_enabled))
		return 0;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		unsigned int seq;
		unsigned int rank;
		unsigned int util;
		bool matches;

		do {
			seq = read_seqcount_begin(&target->cpu_seq);
			matches = ax_bs_target_active(target) &&
				ax_bs_target_matches(target, task);
			if (matches) {
				rank = ax_bs_target_rank(target);
				util = ax_bs_target_util(target);
			} else {
				rank = 0;
				util = 0;
			}
		} while (read_seqcount_retry(&target->cpu_seq, seq));

		if (matches && (!best || rank > best_rank)) {
			best = target;
			best_rank = rank;
			best_util = util;
		}
	}

	if (match)
		*match = best;
	if (match_rank)
		*match_rank = best_rank;
	return best_util;
}

static void ax_bs_note_target_cpu(struct ax_bs_target *target,
				  struct task_struct *task, int cpu)
{
	unsigned long flags;

	if (!target || !task || cpu < 0 || cpu >= nr_cpu_ids)
		return;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	if (ax_bs_target_util(target) && ax_bs_target_matches(target, task)) {
		write_seqcount_begin(&target->cpu_seq);
		WRITE_ONCE(target->cpu, cpu);
		write_seqcount_end(&target->cpu_seq);
	}
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);
}

static void ax_bs_note_matching_target_cpu(struct task_struct *task, int cpu)
{
	struct ax_bs_target *target;

	ax_bs_match_target_util(task, &target, NULL);
	ax_bs_note_target_cpu(target, task, cpu);
}

static bool ax_bs_task_has_role(struct task_struct *task, int role)
{
	pid_t pid;
	int i;

	if (!task || !READ_ONCE(ax_bs_enabled))
		return false;

	pid = task_tgid_nr(task);
	if (pid <= 0)
		return false;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];

		if (!ax_bs_target_active(target))
			continue;
		if (READ_ONCE(target->pid) != pid)
			continue;
		if (READ_ONCE(target->role) == role)
			return true;
	}

	return false;
}

#if defined(AX_BS_HAS_LOCK_BOOST)
static unsigned int ax_bs_lock_task_util(struct task_struct *task)
{
	struct pid *pid_ref;
	pid_t tid;
	unsigned int util = 0;
	int i;

	if (!task || !READ_ONCE(ax_bs_lock_boost))
		return 0;

	tid = task_pid_nr(task);
	pid_ref = task_pid(task);
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		struct ax_bs_lock_target *target = &ax_bs_lock_targets[i];

		if (!ax_bs_lock_target_active(target))
			continue;
		if (READ_ONCE(target->tid) != tid)
			continue;
		if (READ_ONCE(target->pid_ref) != pid_ref)
			continue;

		util = max_t(unsigned int, util, READ_ONCE(target->util));
	}

	return ax_sched_clamp_util(util);
}
#else
static unsigned int ax_bs_lock_task_util(struct task_struct *task)
{
	return 0;
}
#endif

static unsigned int ax_bs_support_task_util(struct task_struct *task);
static bool ax_bs_task_is_launch_worker(struct task_struct *task);
static bool ax_bs_target_is_launch_worker_scene(struct ax_bs_target *target);
static unsigned int ax_bs_launch_worker_util_for(struct ax_bs_target *target);
static unsigned int ax_bs_launch_worker_task_util(struct task_struct *task);
#if defined(AX_BS_HAS_SVP_POLICY)
static unsigned int ax_svp_task_util(struct task_struct *task);
#endif

static unsigned int ax_bs_task_util(struct task_struct *task)
{
	unsigned int util;

	util = max_t(unsigned int,
		     ax_bs_match_target_util(task, NULL, NULL),
		     max_t(unsigned int, ax_bs_lock_task_util(task),
			   ax_bs_support_task_util(task)));
	util = max_t(unsigned int, util, ax_bs_launch_worker_task_util(task));
	util = max_t(unsigned int, util, ax_bs_auto_top_app_task_util(task));
#if defined(AX_BS_HAS_GAME_BOOST)
	util = max_t(unsigned int, util, ax_game_boost_task_util(task));
#endif

	return util;
}

static bool ax_bs_note_wakeup(struct ax_bs_target *target,
			      struct task_struct *task, bool launch_worker,
			      bool hit)
{
	unsigned long flags;
	unsigned long now = jiffies;
	unsigned long last;
	unsigned long delta_ms;
	unsigned int score;
	unsigned int gain;
	unsigned int decay;
	bool updated = false;

	if (!target || !task)
		return false;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	if (!ax_bs_target_util(target) ||
	    (!ax_bs_target_matches(target, task) &&
	     (!launch_worker ||
	      READ_ONCE(target->pid) != task_tgid_nr(task) ||
	      !ax_bs_task_is_launch_worker(task) ||
	      !ax_bs_target_is_launch_worker_scene(target))))
		goto unlock;
	last = READ_ONCE(target->last_wake_jiffies);
	score = ax_bs_clamp_score(atomic_read(&target->score));
	gain = READ_ONCE(ax_bs_score_gain);
	decay = READ_ONCE(ax_bs_score_decay);

	if (last) {
		delta_ms = jiffies_to_msecs(now - last);
		if (delta_ms <= READ_ONCE(ax_bs_score_window_ms)) {
			score = ax_bs_clamp_score(score + gain);
		} else if (decay) {
			score -= min_t(unsigned int, score,
				       max_t(unsigned int, 1, delta_ms / decay));
		}
	}

	WRITE_ONCE(target->last_wake_jiffies, now);
	atomic_set(&target->score, ax_bs_clamp_score(score));
	atomic64_inc(&target->wakeups);
	if (hit)
		atomic64_inc(&target->hits);
	updated = true;

unlock:
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);
	return updated;
}

static unsigned int ax_bs_get_cpu_score(int cpu)
{
	unsigned int score = READ_ONCE(ax_bs_cpu_score[cpu]);

	return score ? score : cpu + 1;
}

static unsigned int ax_bs_get_cpu_capacity(int cpu)
{
	unsigned int capacity = READ_ONCE(ax_bs_cpu_capacity[cpu]);

	return capacity ? capacity : SCHED_CAPACITY_SCALE;
}

static unsigned int ax_bs_cpu_pressure_score(int cpu)
{
	unsigned int pressure = atomic_read(&ax_bs_cpu_pressure[cpu]);
	unsigned long last = READ_ONCE(ax_bs_cpu_pressure_jiffies[cpu]);
	unsigned long window;

	if (!READ_ONCE(ax_bs_spread_active) || !pressure)
		return 0;

	window = max_t(unsigned long,
		       msecs_to_jiffies(max_t(unsigned int,
					      READ_ONCE(ax_bs_spread_window_ms),
					      1)),
		       1);
	if (time_after_eq(jiffies, last + window)) {
		atomic_set(&ax_bs_cpu_pressure[cpu], 0);
		return 0;
	}

	return min_t(unsigned int, pressure, AX_BS_CPU_PRESSURE_MAX);
}

static void ax_bs_note_cpu_pick(int cpu)
{
	unsigned int pressure;

	if (!READ_ONCE(ax_bs_spread_active))
		return;

	WRITE_ONCE(ax_bs_cpu_pressure_jiffies[cpu], jiffies);
	pressure = atomic_read(&ax_bs_cpu_pressure[cpu]);
	if (pressure < AX_BS_CPU_PRESSURE_MAX)
		atomic_inc(&ax_bs_cpu_pressure[cpu]);
}

static void ax_bs_init_cpu_scores(void)
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
		WRITE_ONCE(ax_bs_cpu_score[cpu], score);
		max_score = max_t(unsigned int, max_score, score);
	}

	for_each_possible_cpu(cpu) {
		unsigned int capacity = SCHED_CAPACITY_SCALE;
		unsigned int score = ax_bs_get_cpu_score(cpu);

		if (max_score)
			capacity = (unsigned int)((u64)score * SCHED_CAPACITY_SCALE /
						  max_score);
		WRITE_ONCE(ax_bs_cpu_capacity[cpu], max_t(unsigned int, capacity, 1));
		WRITE_ONCE(ax_bs_cpu_capacity_valid[cpu], false);
		atomic_set(&ax_bs_cpu_pressure[cpu], 0);
		WRITE_ONCE(ax_bs_cpu_pressure_jiffies[cpu], 0);
	}
}

#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
static void ax_bs_update_cpu_capacity(void *unused, int cpu,
				      unsigned long *capacity)
{
	bool ready = true;
	unsigned int cpu_capacity;
	unsigned int score;
	int i;

	(void)unused;

	if (!capacity || cpu < 0 || cpu >= nr_cpu_ids)
		return;

	cpu_capacity = clamp_t(unsigned long, *capacity, 1, SCHED_CAPACITY_SCALE);
	if (!READ_ONCE(ax_bs_cpu_capacity_valid[cpu]))
		score = cpu_capacity;
	else
		score = max_t(unsigned int, READ_ONCE(ax_bs_cpu_score[cpu]),
			      cpu_capacity);

	WRITE_ONCE(ax_bs_cpu_score[cpu], score);
	WRITE_ONCE(ax_bs_cpu_capacity[cpu], cpu_capacity);
	WRITE_ONCE(ax_bs_cpu_capacity_valid[cpu], true);
	if (READ_ONCE(ax_bs_cpu_capacity_ready))
		return;

	for_each_online_cpu(i) {
		if (!READ_ONCE(ax_bs_cpu_capacity_valid[i])) {
			ready = false;
			break;
		}
	}
	if (ready)
		WRITE_ONCE(ax_bs_cpu_capacity_ready, true);
}
#endif

static bool ax_bs_cpu_topology_ready(void)
{
#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
	return READ_ONCE(ax_bs_cpu_capacity_hook_registered) &&
		READ_ONCE(ax_bs_cpu_capacity_ready);
#else
	return false;
#endif
}

static int ax_bs_spread_cpu_mask(struct task_struct *task,
				 const struct cpumask *cpu_mask, bool *spilled)
{
	unsigned int peak_pressure = ~0U;
	unsigned int peak_score = 0;
	unsigned int spill_pressure = ~0U;
	unsigned int spill_score = 0;
	unsigned int threshold;
	int peak_cpu = AX_SCHED_CPU_NONE;
	int spill_cpu = AX_SCHED_CPU_NONE;
	int cpu;

	if (spilled)
		*spilled = false;
	if (!task)
		return AX_SCHED_CPU_NONE;

	for_each_online_cpu(cpu) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
		if (!cpumask_test_cpu(cpu, task->cpus_ptr))
#else
		if (!cpumask_test_cpu(cpu, &task->cpus_allowed))
#endif
			continue;
		if (cpu_mask && !cpumask_test_cpu(cpu, cpu_mask))
			continue;
		peak_score = max_t(unsigned int, peak_score,
				   ax_bs_get_cpu_score(cpu));
	}

	for_each_online_cpu(cpu) {
		unsigned int pressure;
		unsigned int score;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
                if (!cpumask_test_cpu(cpu, task->cpus_ptr))
#else
                if (!cpumask_test_cpu(cpu, &task->cpus_allowed))
#endif
			continue;
		if (cpu_mask && !cpumask_test_cpu(cpu, cpu_mask))
			continue;

		score = ax_bs_get_cpu_score(cpu);
		pressure = ax_bs_cpu_pressure_score(cpu);
		if (score == peak_score) {
			if (pressure <= peak_pressure) {
				peak_pressure = pressure;
				peak_cpu = cpu;
			}
		} else if (score > spill_score ||
			   (score == spill_score && pressure <= spill_pressure)) {
			spill_score = score;
			spill_pressure = pressure;
			spill_cpu = cpu;
		}
	}

	threshold = READ_ONCE(ax_bs_spill_pressure);
	if (!READ_ONCE(ax_bs_spread_active) || !threshold || spill_cpu < 0 ||
	    peak_pressure < threshold)
		return peak_cpu;

	if (spilled)
		*spilled = true;
	return spill_cpu;
}

static bool ax_bs_can_spill_to(struct task_struct *task, int src_cpu,
			       int dst_cpu)
{
	int spill_cpu;

	if (!task || ax_bs_task_is_primary_visual(task))
		return false;

	spill_cpu = ax_bs_spread_cpu_mask(task, NULL, NULL);
	if (spill_cpu < 0)
		return false;

	return ax_bs_get_cpu_score(spill_cpu) < ax_bs_get_cpu_score(src_cpu) &&
		ax_bs_get_cpu_score(dst_cpu) >= ax_bs_get_cpu_score(spill_cpu);
}

static int ax_bs_best_cpu_mask(struct task_struct *task,
			       const struct cpumask *cpu_mask)
{
	unsigned int best_score = 0;
	unsigned int best_pressure = ~0U;
	int best_cpu = -1;
	int cpu;

	for_each_online_cpu(cpu) {
		unsigned int pressure;
		unsigned int score;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
                if (!cpumask_test_cpu(cpu, task->cpus_ptr))
#else
                if (!cpumask_test_cpu(cpu, &task->cpus_allowed))
#endif
			continue;
		if (cpu_mask && !cpumask_test_cpu(cpu, cpu_mask))
			continue;

		score = ax_bs_get_cpu_score(cpu);
		pressure = ax_bs_cpu_pressure_score(cpu);
		if (score > best_score ||
		    (score == best_score && pressure <= best_pressure)) {
			best_score = score;
			best_pressure = pressure;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static int ax_bs_fit_cpu_mask(struct task_struct *task,
			      const struct cpumask *cpu_mask,
			      unsigned int util)
{
	unsigned int fallback_score = 0;
	unsigned int best_score = ~0U;
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
		if (cpu_mask && !cpumask_test_cpu(cpu, cpu_mask))
			continue;

		score = ax_bs_get_cpu_score(cpu);
		pressure = ax_bs_cpu_pressure_score(cpu);
		if (score > fallback_score) {
			fallback_score = score;
			fallback_cpu = cpu;
		}

		if (util && ax_bs_get_cpu_capacity(cpu) < util)
			continue;

		if (score < best_score ||
		    (score == best_score && pressure <= best_pressure)) {
			best_score = score;
			best_pressure = pressure;
			best_cpu = cpu;
		}
	}

	return best_cpu >= 0 ? best_cpu : fallback_cpu;
}

static int ax_bs_lowest_cpu(struct task_struct *task)
{
	unsigned int best_score = ~0U;
	unsigned int best_pressure = ~0U;
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

		score = ax_bs_get_cpu_score(cpu);
		pressure = ax_bs_cpu_pressure_score(cpu);
		if (score < best_score ||
		    (score == best_score && pressure <= best_pressure)) {
			best_score = score;
			best_pressure = pressure;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static int ax_bs_pick_cpu_mask(struct task_struct *task,
			       const struct cpumask *cpu_mask,
			       unsigned int util, bool prefer_peak,
			       bool allow_spill)
{
	int cpu;

	if (allow_spill)
		cpu = ax_bs_spread_cpu_mask(task, cpu_mask, NULL);
	else if (prefer_peak)
		cpu = ax_bs_best_cpu_mask(task, cpu_mask);
	else
		cpu = ax_bs_fit_cpu_mask(task, cpu_mask, util);

	if (cpu >= 0)
		ax_bs_note_cpu_pick(cpu);

	return cpu;
}

static int ax_bs_pick_cpu(struct task_struct *task, unsigned int util,
			  bool prefer_peak, bool allow_spill)
{
	return ax_bs_pick_cpu_mask(task, NULL, util, prefer_peak, allow_spill);
}

static bool ax_bs_task_is_reclaim_worker(struct task_struct *task)
{
	return task &&
		(ax_sched_comm_has_prefix(task->comm, "kswapd",
					  sizeof("kswapd") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "kcompactd",
					  sizeof("kcompactd") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "zram",
					  sizeof("zram") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "zswap",
					  sizeof("zswap") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "vmscan",
					  sizeof("vmscan") - 1));
}

static bool ax_bs_task_group_comm_has_prefix(struct task_struct *task,
					     const char *prefix, size_t len)
{
	struct task_struct *leader;

	if (!task)
		return false;

	leader = task->group_leader;
	return leader && ax_sched_comm_has_prefix(leader->comm, prefix, len);
}

static bool ax_bs_task_is_binder_worker(struct task_struct *task)
{
	return task &&
		(ax_sched_comm_has_prefix(task->comm, "binder:",
					  sizeof("binder:") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "HwBinder:",
					  sizeof("HwBinder:") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "hwbinder:",
					  sizeof("hwbinder:") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "vndbinder:",
					  sizeof("vndbinder:") - 1));
}

static bool ax_bs_task_is_composer_worker(struct task_struct *task)
{
	pid_t composer_tgid;

	if (!task)
		return false;

	if (!strcmp(task->comm, AX_SCHED_COMPOSER_SERVICE) ||
	    ax_bs_task_group_comm_has_prefix(task, AX_SCHED_COMPOSER_SERVICE,
					     sizeof(AX_SCHED_COMPOSER_SERVICE) - 1)) {
		atomic_set(&ax_bs_composer_tgid, task_tgid_nr(task));
		return true;
	}

	composer_tgid = atomic_read(&ax_bs_composer_tgid);
	return composer_tgid > 0 &&
		task_tgid_nr(task) == composer_tgid &&
		(ax_sched_comm_has_prefix(task->comm, "HwBinder:",
					  sizeof("HwBinder:") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "vndbinder:",
					  sizeof("vndbinder:") - 1) ||
		 !strcmp(task->comm, "HWC_UeventThrea") ||
		 !strcmp(task->comm, "SDM_EventThread") ||
		 !strcmp(task->comm, "DPPS_THREAD"));
}

static bool ax_bs_task_is_display_support_worker(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, "kgsl-events") ||
		 !strcmp(task->comm, AX_SCHED_KGSL_WORKER) ||
		 !strcmp(task->comm, "surfaceflinger") ||
		 !strcmp(task->comm, AX_SCHED_HWC_ASYNC_WORKER) ||
		 !strcmp(task->comm, AX_SCHED_RE_COMPLETION) ||
		 !strcmp(task->comm, AX_SCHED_GPU_COMPLETION) ||
		 ax_bs_task_is_composer_worker(task) ||
		 ax_sched_comm_has_prefix(task->comm, AX_SCHED_SF_BACKGROUND_EXEC,
					  sizeof(AX_SCHED_SF_BACKGROUND_EXEC) - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "RenderEngine",
					  sizeof("RenderEngine") - 1));
}

static bool ax_bs_task_is_platform_media_worker(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, "C2NodeImpl") ||
		 !strcmp(task->comm, "codec_looper") ||
		 !strcmp(task->comm, "media.hwcodec") ||
		 !strcmp(task->comm, "media.swcodec") ||
		 !strcmp(task->comm, "mediaserver") ||
		 !strcmp(task->comm, "recorder_looper") ||
		 !strcmp(task->comm, "MP4WtrVidTrkThr") ||
		 ax_sched_comm_has_prefix(task->comm, "EvtQ_c2.",
					  sizeof("EvtQ_c2.") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "EvtQ_codecPipe",
					  sizeof("EvtQ_codecPipe") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "poll_hevc",
					  sizeof("poll_hevc") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "vendor.qti.medi",
					  sizeof("vendor.qti.medi") - 1));
}

static bool ax_bs_task_is_launcher_render_support(struct task_struct *task)
{
	return ax_sched_task_is_render_helper(task) &&
		ax_bs_task_group_comm_has_prefix(task, "droid.launcher",
						 sizeof("droid.launcher") - 1);
}

static bool ax_bs_task_is_support_worker(struct task_struct *task)
{
	return ax_bs_task_is_display_support_worker(task) ||
		ax_bs_task_is_platform_media_worker(task) ||
		ax_bs_task_is_launcher_render_support(task) ||
		(task &&
		 ax_sched_comm_has_prefix(task->comm, "f2fs_ckpt-",
					  sizeof("f2fs_ckpt-") - 1));
}

static unsigned int ax_bs_support_task_util(struct task_struct *task)
{
	if (!ax_bs_has_transient_scene() || !ax_bs_task_is_support_worker(task))
		return 0;

	return ax_sched_clamp_util(READ_ONCE(ax_bs_support_worker_util));
}

static bool ax_bs_target_is_launch_worker_scene(struct ax_bs_target *target)
{
	int mode;
	int role;
	int source;

	if (!ax_bs_target_active(target))
		return false;

	role = READ_ONCE(target->role);
	if (role == AX_BS_ROLE_SYSTEM_SERVER)
		return false;

	mode = READ_ONCE(target->mode);
	source = READ_ONCE(target->source);
	return mode == AX_BS_MODE_LAUNCH ||
		source == AX_BS_SOURCE_POWER_LAUNCH ||
		source == AX_BS_SOURCE_GAME_LOADING ||
		source == AX_BS_SOURCE_TOP_APP_SWITCH;
}

static bool ax_bs_task_is_launch_worker(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, "Blocking Thread") ||
		 !strcmp(task->comm, "Jit") ||
		 !strcmp(task->comm, "Jit thread pool") ||
		 !strcmp(task->comm, "HeapTaskDaemon") ||
		 ax_sched_comm_has_prefix(task->comm, "BG Thread #",
					  sizeof("BG Thread #") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "Lite Thread #",
					  sizeof("Lite Thread #") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "DefaultDispatch",
					  sizeof("DefaultDispatch") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "GoogleApiHandle",
					  sizeof("GoogleApiHandle") - 1));
}

static struct ax_bs_target *ax_bs_match_launch_worker_target(struct task_struct *task)
{
	struct ax_bs_target *best = NULL;
	unsigned int best_rank = 0;
	pid_t pid;
	int i;

	if (!task || !READ_ONCE(ax_bs_enabled) || !ax_bs_task_is_launch_worker(task))
		return NULL;

	pid = task_tgid_nr(task);
	if (pid <= 0)
		return NULL;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		unsigned int rank;

		if (READ_ONCE(target->pid) != pid)
			continue;
		if (!ax_bs_target_is_launch_worker_scene(target))
			continue;

		rank = ax_bs_target_rank(target);
		if (!best || rank > best_rank) {
			best = target;
			best_rank = rank;
		}
	}

	return best;
}

static unsigned int ax_bs_launch_worker_util_for(struct ax_bs_target *target)
{
	if (!target)
		return 0;

	return ax_sched_clamp_util(READ_ONCE(ax_bs_launch_worker_util));
}

static unsigned int ax_bs_launch_worker_task_util(struct task_struct *task)
{
	return ax_bs_launch_worker_util_for(ax_bs_match_launch_worker_target(task));
}

static bool ax_bs_task_is_systemui_background_worker(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, "SysUiBg") ||
		 ax_sched_comm_has_prefix(task->comm, "SystemUIBg-",
					  sizeof("SystemUIBg-") - 1));
}

static bool ax_bs_task_is_launcher_background_worker(struct task_struct *task)
{
	return (ax_bs_task_has_role(task, AX_BS_ROLE_LAUNCHER) ||
		ax_bs_task_has_role(task, AX_BS_ROLE_TOP_APP)) &&
		(!strcmp(task->comm, "BackgroundExecu") ||
		 !strcmp(task->comm, "launcher-loader") ||
		 ax_sched_comm_has_prefix(task->comm, "TaskThumbnail",
					  sizeof("TaskThumbnail") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "LauncherBg",
					  sizeof("LauncherBg") - 1));
}

static bool ax_bs_target_uses_background_guard(struct ax_bs_target *target)
{
	int mode;
	int role;
	int source;

	if (!ax_bs_target_active(target))
		return false;

	mode = READ_ONCE(target->mode);
	if (mode == AX_BS_MODE_TOP_APP)
		return false;

	role = READ_ONCE(target->role);
	if (role != AX_BS_ROLE_SYSTEM_UI && role != AX_BS_ROLE_LAUNCHER)
		return false;

	source = READ_ONCE(target->source);
	switch (source) {
	case AX_BS_SOURCE_START_ACTIVITY_BINDER:
	case AX_BS_SOURCE_POWER_LAUNCH:
	case AX_BS_SOURCE_GAME_LOADING:
	case AX_BS_SOURCE_TOP_APP_SWITCH:
		return false;
	default:
		return true;
	}
}

static bool ax_bs_has_ui_background_guard_scene(bool auto_systemui)
{
	int i;

	if (auto_systemui)
		return true;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		if (ax_bs_target_uses_background_guard(&ax_bs_targets[i]))
			return true;
	}

	return false;
}

static bool ax_bs_task_has_visual_role(struct task_struct *task,
					bool guard_scene, pid_t auto_tgid)
{
	pid_t pid;
	int i;

	if (!task || !READ_ONCE(ax_bs_enabled))
		return false;

	pid = task_tgid_nr(task);
	if (pid <= 0)
		return false;
	if (auto_tgid == pid)
		return true;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		int mode;
		int role;

		if (!ax_bs_target_active(target))
			continue;
		if (READ_ONCE(target->pid) != pid)
			continue;

		mode = READ_ONCE(target->mode);
		role = READ_ONCE(target->role);
		if ((mode == AX_BS_MODE_LAUNCH &&
		     role != AX_BS_ROLE_SYSTEM_SERVER) ||
		    role == AX_BS_ROLE_SYSTEM_UI ||
		    role == AX_BS_ROLE_LAUNCHER)
			return true;
		if (role == AX_BS_ROLE_TOP_APP)
			return !guard_scene;
	}

	return false;
}

static void ax_bs_get_task_scene(struct task_struct *task,
				 struct ax_bs_task_scene *scene)
{
	struct ax_bs_auto_state auto_state;
	pid_t auto_tgid = -1;
	bool auto_systemui = false;

	if (ax_bs_read_active_auto_state(&auto_state)) {
		auto_tgid = auto_state.tgid;
		auto_systemui = auto_state.systemui;
	}
	scene->guard = ax_bs_has_ui_background_guard_scene(auto_systemui);
	scene->display_support = ax_bs_task_is_display_support_worker(task);
	scene->visual = ax_bs_task_has_visual_role(task, scene->guard,
						    auto_tgid);
	scene->offscreen = task && scene->guard && !scene->visual &&
		!scene->display_support;
}

static bool ax_bs_task_is_offscreen_render_worker(struct task_struct *task,
						  bool offscreen)
{
	return offscreen &&
		!ax_bs_task_is_launcher_render_support(task) &&
		ax_sched_task_is_render_helper(task);
}

static bool ax_bs_task_is_offscreen_webview_worker(struct task_struct *task,
						   bool offscreen)
{
	return offscreen &&
		ax_sched_comm_has_prefix(task->comm, AX_SCHED_CHROME_THREAD_POOL,
					 sizeof(AX_SCHED_CHROME_THREAD_POOL) - 1);
}

static bool ax_bs_task_is_offscreen_app_worker(struct task_struct *task,
					       bool offscreen)
{
	return offscreen &&
		(!strcmp(task->comm, "Blocking Thread") ||
		 ax_sched_comm_has_prefix(task->comm, "Timer-",
					  sizeof("Timer-") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "BG Thread #",
					  sizeof("BG Thread #") - 1));
}

static bool ax_bs_task_is_offscreen_pool_worker(struct task_struct *task,
						bool offscreen)
{
	return offscreen &&
		(ax_sched_comm_has_prefix(task->comm, "highpool[",
					  sizeof("highpool[") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "lowpool[",
					  sizeof("lowpool[") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "actvpool[",
					  sizeof("actvpool[") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "punchpool-",
					  sizeof("punchpool-") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "DefaultDispatch",
					  sizeof("DefaultDispatch") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "GlobalDispatch",
					  sizeof("GlobalDispatch") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "GlobalScheduler",
					  sizeof("GlobalScheduler") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "GoogleApiHandle",
					  sizeof("GoogleApiHandle") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "queued-work-loo",
					  sizeof("queued-work-loo") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "arch_disk_io_",
					  sizeof("arch_disk_io_") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "CronetFile",
					  sizeof("CronetFile") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "CronetNet",
					  sizeof("CronetNet") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "common_PhantomF",
					  sizeof("common_PhantomF") - 1) ||
		 ax_sched_comm_has_prefix(task->comm, "ProcessStablePh",
					  sizeof("ProcessStablePh") - 1));
}

static bool ax_bs_task_is_offscreen_media_worker(struct task_struct *task,
						 bool offscreen)
{
	return offscreen &&
		!ax_bs_task_is_platform_media_worker(task) &&
		ax_sched_comm_has_prefix(task->comm, "ExoPlayer:",
					 sizeof("ExoPlayer:") - 1);
}

static bool ax_bs_task_is_runtime_background_worker(struct task_struct *task)
{
	return task &&
		(!strcmp(task->comm, "Profile") ||
		 !strcmp(task->comm, "Profile Saver") ||
		 !strcmp(task->comm, "perfetto_hprof_") ||
		 !strcmp(task->comm, "Jit") ||
		 !strcmp(task->comm, "Jit thread pool") ||
		 !strcmp(task->comm, "HeapTaskDaemon") ||
		 !strcmp(task->comm, "FinalizerDaemon") ||
		 !strcmp(task->comm, "ReferenceQueueD"));
}

static bool ax_bs_task_is_background_worker(
		struct task_struct *task, const struct ax_bs_task_scene *scene)
{
	if (!task || !scene->guard)
		return false;

	return !strcmp(task->comm, "ll.splashworker") ||
		ax_bs_task_is_systemui_background_worker(task) ||
		ax_bs_task_is_launcher_background_worker(task) ||
		ax_bs_task_is_runtime_background_worker(task) ||
		ax_bs_task_is_offscreen_render_worker(task, scene->offscreen) ||
		ax_bs_task_is_offscreen_webview_worker(task, scene->offscreen) ||
		ax_bs_task_is_offscreen_app_worker(task, scene->offscreen) ||
		ax_bs_task_is_offscreen_pool_worker(task, scene->offscreen) ||
		ax_bs_task_is_offscreen_media_worker(task, scene->offscreen);
}

static unsigned int ax_bs_latency_task_util(struct task_struct *task)
{
	unsigned int util;

	if (!task)
		return 0;

	util = max_t(unsigned int, ax_bs_match_target_util(task, NULL, NULL),
		     ax_bs_lock_task_util(task));
	util = max_t(unsigned int, util, ax_bs_auto_top_app_task_util(task));
	util = max_t(unsigned int, util, ax_bs_launch_worker_task_util(task));
#if defined(AX_BS_HAS_FRAME_BOOST)
	util = max_t(unsigned int, util, ax_frame_boost_task_util(task));
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	util = max_t(unsigned int, util, ax_svp_task_util(task));
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	util = max_t(unsigned int, util, ax_game_boost_task_util(task));
#endif

	return util;
}

static bool ax_bs_task_is_latency_exempt(
		struct task_struct *task, const struct ax_bs_task_scene *scene)
{
	if (!task)
		return false;

	if (ax_bs_latency_task_util(task))
		return true;
	if (ax_bs_support_task_util(task))
		return true;
	if (READ_ONCE(ax_bs_scene_focus) && ax_bs_task_is_binder_worker(task))
		return true;
	if (ax_sched_task_is_render_helper(task))
		return !ax_bs_task_is_offscreen_render_worker(task,
							      scene->offscreen);

	return ax_bs_task_group_comm_has_prefix(task, "system_server",
						sizeof("system_server") - 1) ||
		ax_bs_task_group_comm_has_prefix(task, "surfaceflinger",
						 sizeof("surfaceflinger") - 1) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_UI) ||
		!strcmp(task->comm, AX_SCHED_ANDROID_DISPLAY) ||
		!strcmp(task->comm, "system_server") ||
		!strcmp(task->comm, "surfaceflinger");
}

static bool ax_bs_task_is_background_guard_candidate(struct task_struct *task)
{
	struct ax_bs_task_scene scene;

	if (!task || !ax_bs_has_transient_scene())
		return false;
	ax_bs_get_task_scene(task, &scene);
	if (!scene.guard)
		return false;
	if (ax_bs_task_is_latency_exempt(task, &scene))
		return false;
	if (ax_bs_task_is_reclaim_worker(task))
		return false;
	if (task->flags & PF_KTHREAD)
		return false;
	if (task->policy != SCHED_NORMAL || task_nice(task) < 0)
		return false;
	if (ax_bs_task_is_background_worker(task, &scene))
		return true;

	return task_nice(task) > 0 && scene.offscreen;
}

#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
static unsigned int ax_bs_task_rank(struct task_struct *task)
{
	struct ax_bs_auto_state auto_state;
	struct ax_bs_task_scene scene;
	unsigned int target_rank;
	unsigned int target_util;
	unsigned int rank = 0;

	if (!task || !READ_ONCE(ax_bs_enabled))
		return 0;

	target_util = ax_bs_match_target_util(task, NULL, &target_rank);
	if (target_util)
		rank = max_t(unsigned int, rank, target_rank);
	if (ax_bs_read_active_auto_state(&auto_state) &&
	    ax_bs_auto_top_app_task_matches(task, &auto_state))
		rank = max_t(unsigned int, rank,
				     auto_state.systemui ?
				     AX_BS_RANK_LAUNCH : AX_BS_RANK_VISUAL);
	if (ax_bs_lock_task_util(task))
		rank = max_t(unsigned int, rank, AX_BS_RANK_LOCK);
	if (!target_util && ax_bs_launch_worker_task_util(task))
		rank = max_t(unsigned int, rank, AX_BS_RANK_LAUNCH);
	if (ax_bs_task_is_support_worker(task)) {
		if (ax_bs_task_is_display_support_worker(task))
			rank = max_t(unsigned int, rank, AX_BS_RANK_DISPLAY);
		else
			rank = max_t(unsigned int, rank, AX_BS_RANK_SUPPORT);
	}
#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_task_util(task))
		rank = max_t(unsigned int, rank, AX_BS_RANK_FRAME);
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	if (ax_svp_task_util(task))
		rank = max_t(unsigned int, rank, AX_BS_RANK_VISUAL);
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	if (ax_game_boost_task_util(task))
		rank = max_t(unsigned int, rank, AX_BS_RANK_DISPLAY);
#endif
	if (!rank && ax_bs_task_is_reclaim_worker(task))
		rank = AX_BS_RANK_RECLAIM;
	if (!rank) {
		ax_bs_get_task_scene(task, &scene);
		if (ax_bs_task_is_background_worker(task, &scene))
			rank = AX_BS_RANK_BACKGROUND;
	}

	return rank;
}

static void ax_bs_dynamic_svp_preempt(void *unused, struct task_struct *task,
				      struct task_struct *curr, bool *preempt)
{
	unsigned int curr_rank;
	unsigned int min_delta;
	unsigned int task_rank;

	(void)unused;

	if (!preempt || *preempt || !task || !curr || task == curr)
		return;
	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_dynamic_svp))
		return;
	if (!ax_bs_has_transient_scene())
		return;
	if (task->policy != SCHED_NORMAL || curr->policy != SCHED_NORMAL)
		return;

	task_rank = ax_bs_task_rank(task);
	if (task_rank < READ_ONCE(ax_bs_dynamic_svp_min_rank))
		return;

	curr_rank = ax_bs_task_rank(curr);
	min_delta = READ_ONCE(ax_bs_dynamic_svp_min_delta);
	if (task_rank <= curr_rank || task_rank - curr_rank < min_delta)
		return;

	*preempt = true;
	atomic64_inc(&ax_bs_dynamic_svp_preempts);
}
#endif

#if defined(AX_BS_HAS_IOLIMIT_HOOK)
static void ax_bs_iolimit_rw(void *unused, struct file *file, size_t count,
			     int rw, unsigned int *delay_ms)
{
	unsigned int delay;
	unsigned int min_bytes;

	(void)unused;
	(void)file;
	(void)rw;

	if (!delay_ms || !READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_io_guard))
		return;

	delay = READ_ONCE(ax_bs_io_limit_delay_ms);
	min_bytes = READ_ONCE(ax_bs_io_limit_min_bytes);
	if (!delay || count < min_bytes)
		return;
	if (!ax_bs_task_is_background_guard_candidate(current))
		return;

	delay = clamp_t(unsigned int, delay, 1, 16);
	*delay_ms = max_t(unsigned int, *delay_ms, delay);
	atomic64_inc(&ax_bs_io_limit_delays);
}
#endif

#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
static bool ax_bs_task_should_boost_gpu_context(struct task_struct *task)
{
	struct ax_bs_task_scene scene;

	if (!task || !READ_ONCE(ax_bs_enabled))
		return false;
	ax_bs_get_task_scene(task, &scene);
	if (ax_bs_task_is_background_worker(task, &scene))
		return false;
	if (scene.visual)
		return true;
	if (scene.display_support)
		return true;
#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_task_util(task))
		return true;
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	if (ax_game_boost_task_util(task))
		return true;
#endif
	return false;
}

static void ax_bs_gpu_context_create(void *unused, unsigned int flags)
{
	unsigned int duration_ms;

	(void)unused;
	(void)flags;

	if (!READ_ONCE(ax_bs_gpu_context_boost))
		return;
	if (!ax_bs_task_should_boost_gpu_context(current))
		return;

	duration_ms = clamp_t(unsigned int,
			      READ_ONCE(ax_bs_gpu_context_boost_ms), 1, 1000);
	ax_sched_boost(AX_BOOST_SOURCE_LAUNCH, AX_BOOST_LEVEL_HEAVY,
		       AX_BOOST_GPU | AX_BOOST_MEMLAT |
		       AX_BOOST_DDR | AX_BOOST_L3, duration_ms);
	atomic64_inc(&ax_bs_gpu_context_boosts);
}
#endif

static int ax_bs_task_ioprio(struct task_struct *task)
{
	int ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0);

	task_lock(task);
	if (task->io_context)
		ioprio = task->io_context->ioprio;
	task_unlock(task);

	return ioprio;
}

static int ax_bs_set_task_ioprio(struct task_struct *task, int ioprio)
{
#if defined(AX_BS_HAS_IOPRIO)
	return set_task_ioprio(task, ioprio);
#else
	return -EOPNOTSUPP;
#endif
}

static int ax_bs_io_guard_ioprio(void)
{
	unsigned int class = READ_ONCE(ax_bs_io_guard_class);
	unsigned int prio = READ_ONCE(ax_bs_io_guard_prio);

	if (class < IOPRIO_CLASS_BE || class > IOPRIO_CLASS_IDLE)
		class = AX_BS_DEFAULT_IO_GUARD_CLASS;
	prio = min_t(unsigned int, prio, IOPRIO_BE_NR - 1);

	return IOPRIO_PRIO_VALUE(class, prio);
}

static struct ax_bs_io_guard *ax_bs_find_io_guard_locked(pid_t tid)
{
	int i;

	for (i = 0; i < AX_BS_MAX_IO_GUARDS; i++) {
		if (READ_ONCE(ax_bs_io_guards[i].tid) == tid)
			return &ax_bs_io_guards[i];
	}

	return NULL;
}

static struct ax_bs_io_guard *ax_bs_alloc_io_guard_locked(pid_t tid)
{
	struct ax_bs_io_guard *guard;
	int i;

	guard = ax_bs_find_io_guard_locked(tid);
	if (guard)
		return guard;

	for (i = 0; i < AX_BS_MAX_IO_GUARDS; i++) {
		guard = &ax_bs_io_guards[i];
		if (READ_ONCE(guard->tid) <= 0)
			return guard;
	}

	for (i = 0; i < AX_BS_MAX_IO_GUARDS; i++) {
		guard = &ax_bs_io_guards[i];
		if (time_after_eq(jiffies, READ_ONCE(guard->expire_jiffies)))
			return guard;
	}

	return NULL;
}

static struct pid *ax_bs_detach_io_guard(struct ax_bs_io_guard *guard,
					 int *old_ioprio, int *guard_ioprio)
{
	struct pid *pid_ref = READ_ONCE(guard->pid_ref);

	if (old_ioprio)
		*old_ioprio = READ_ONCE(guard->old_ioprio);
	if (guard_ioprio)
		*guard_ioprio = READ_ONCE(guard->guard_ioprio);
	WRITE_ONCE(guard->tid, -1);
	WRITE_ONCE(guard->old_ioprio, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0));
	WRITE_ONCE(guard->guard_ioprio, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0));
	WRITE_ONCE(guard->expire_jiffies, 0);
	smp_wmb();
	WRITE_ONCE(guard->pid_ref, NULL);

	return pid_ref;
}

static void ax_bs_restore_io_guard_pid(struct pid *pid_ref, int old_ioprio,
				       int guard_ioprio)
{
	struct task_struct *task;

	if (!pid_ref)
		return;

	rcu_read_lock();
	task = pid_task(pid_ref, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (task) {
		if (ax_bs_task_ioprio(task) == guard_ioprio)
			ax_bs_set_task_ioprio(task, old_ioprio);
		put_task_struct(task);
	}
	ax_bs_release_pid(pid_ref);
}

static void ax_bs_restore_io_guards(bool force)
{
	struct pid *pid_ref;
	int guard_ioprio;
	int old_ioprio;
	int i;

	mutex_lock(&ax_bs_io_guard_lock);
	for (i = 0; i < AX_BS_MAX_IO_GUARDS; i++) {
		struct ax_bs_io_guard *guard = &ax_bs_io_guards[i];

		if (READ_ONCE(guard->tid) <= 0)
			continue;
		if (!force && time_before(jiffies, READ_ONCE(guard->expire_jiffies)))
			continue;

		pid_ref = ax_bs_detach_io_guard(guard, &old_ioprio,
						&guard_ioprio);
		mutex_unlock(&ax_bs_io_guard_lock);
		ax_bs_restore_io_guard_pid(pid_ref, old_ioprio, guard_ioprio);
		mutex_lock(&ax_bs_io_guard_lock);
	}
	mutex_unlock(&ax_bs_io_guard_lock);
}

static void ax_bs_guard_task_io(struct task_struct *task, unsigned long expire)
{
	struct ax_bs_io_guard *guard;
	struct pid *old_pid = NULL;
	struct pid *pid_ref = NULL;
	int old_ioprio;
	int old_guard_ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0);
	int old_guard_target = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0);
	int target_ioprio;
	pid_t tid;
	bool tracked = false;
	bool guarded = false;

	if (!task || !READ_ONCE(ax_bs_io_guard))
		return;

	tid = task_pid_nr(task);
	if (tid <= 0)
		return;

	target_ioprio = ax_bs_io_guard_ioprio();
	mutex_lock(&ax_bs_io_guard_lock);
	guard = ax_bs_find_io_guard_locked(tid);
	if (guard)
		tracked = true;
	mutex_unlock(&ax_bs_io_guard_lock);
	if (tracked) {
		old_ioprio = ax_bs_task_ioprio(task);
		if (IOPRIO_PRIO_CLASS(old_ioprio) == IOPRIO_CLASS_RT)
			return;
		if (old_ioprio != target_ioprio) {
			if (ax_bs_set_task_ioprio(task, target_ioprio))
				return;
		}
		mutex_lock(&ax_bs_io_guard_lock);
		guard = ax_bs_find_io_guard_locked(tid);
		if (guard) {
			WRITE_ONCE(guard->expire_jiffies, expire);
			WRITE_ONCE(guard->guard_ioprio, target_ioprio);
		}
		mutex_unlock(&ax_bs_io_guard_lock);
		return;
	}

	old_ioprio = ax_bs_task_ioprio(task);
	if (IOPRIO_PRIO_CLASS(old_ioprio) == IOPRIO_CLASS_RT)
		return;
	if (old_ioprio == target_ioprio)
		return;

	pid_ref = get_task_pid(task, PIDTYPE_PID);
	if (!pid_ref)
		return;

	mutex_lock(&ax_bs_io_guard_lock);
	guard = ax_bs_alloc_io_guard_locked(tid);
	if (guard) {
		if (READ_ONCE(guard->tid) <= 0) {
			WRITE_ONCE(guard->old_ioprio, old_ioprio);
			WRITE_ONCE(guard->guard_ioprio, target_ioprio);
			WRITE_ONCE(guard->pid_ref, pid_ref);
			pid_ref = NULL;
			smp_wmb();
			WRITE_ONCE(guard->tid, tid);
		} else if (READ_ONCE(guard->tid) != tid) {
			old_pid = ax_bs_detach_io_guard(guard, &old_guard_ioprio,
							&old_guard_target);
			WRITE_ONCE(guard->old_ioprio, old_ioprio);
			WRITE_ONCE(guard->guard_ioprio, target_ioprio);
			WRITE_ONCE(guard->pid_ref, pid_ref);
			pid_ref = NULL;
			smp_wmb();
			WRITE_ONCE(guard->tid, tid);
		}
		WRITE_ONCE(guard->expire_jiffies, expire);
		guarded = true;
	}
	mutex_unlock(&ax_bs_io_guard_lock);

	ax_bs_release_pid(pid_ref);
	ax_bs_restore_io_guard_pid(old_pid, old_guard_ioprio, old_guard_target);
	if (guarded && ax_bs_set_task_ioprio(task, target_ioprio)) {
		mutex_lock(&ax_bs_io_guard_lock);
		guard = ax_bs_find_io_guard_locked(tid);
		if (guard)
			pid_ref = ax_bs_detach_io_guard(guard, &old_ioprio,
							&target_ioprio);
		mutex_unlock(&ax_bs_io_guard_lock);
		ax_bs_restore_io_guard_pid(pid_ref, old_ioprio, target_ioprio);
	}
}

static void ax_bs_scan_io_guard_tasks(unsigned long expire)
{
	struct task_struct *tasks[AX_BS_MAX_IO_GUARDS];
	struct task_struct *process;
	struct task_struct *task;
	int count = 0;
	int i;

	rcu_read_lock();
	for_each_process_thread(process, task) {
		if (count >= AX_BS_MAX_IO_GUARDS)
			break;
		if (!ax_bs_task_is_background_guard_candidate(task))
			continue;
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();

	for (i = 0; i < count; i++) {
		ax_bs_guard_task_io(tasks[i], expire);
		put_task_struct(tasks[i]);
	}
}

static unsigned long ax_bs_io_guard_delay(void)
{
	return max_t(unsigned long,
		     msecs_to_jiffies(max_t(unsigned int,
					    READ_ONCE(ax_bs_io_guard_scan_ms),
					    32)),
		     1);
}

static void ax_bs_io_guard_work_fn(struct work_struct *work)
{
	unsigned long duration;
	unsigned long delay;
	unsigned long expire;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_io_guard) ||
	    !ax_bs_has_transient_scene()) {
		ax_bs_restore_io_guards(true);
		return;
	}

	delay = ax_bs_io_guard_delay();
	duration = max_t(unsigned long,
			 msecs_to_jiffies(max_t(unsigned int,
						READ_ONCE(ax_bs_io_guard_duration_ms),
						READ_ONCE(ax_bs_io_guard_scan_ms))),
			 delay);
	expire = jiffies + duration;

	ax_bs_scan_io_guard_tasks(expire);
	ax_bs_restore_io_guards(false);

	if (ax_bs_has_transient_scene())
		mod_delayed_work(system_wq, &ax_bs_io_guard_work, delay);
	else
		ax_bs_restore_io_guards(true);
}

static void ax_bs_io_guard_kick(void)
{
	if (READ_ONCE(ax_bs_io_guard))
		mod_delayed_work(system_wq, &ax_bs_io_guard_work, 0);
	else
		ax_bs_restore_io_guards(true);
}

static void ax_bs_release_io_guards(void)
{
	cancel_delayed_work_sync(&ax_bs_io_guard_work);
	ax_bs_restore_io_guards(true);
}

static bool ax_bs_pick_background_rq(struct task_struct *task, int prev_cpu,
				     struct ax_sched_cpu_pick *pick)
{
	int cpu;

	if (!pick || !READ_ONCE(ax_bs_background_guard) ||
	    !ax_bs_task_is_background_guard_candidate(task))
		return false;

	cpu = ax_bs_lowest_cpu(task);
	if (cpu == prev_cpu ||
	    !ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_BURST))
		return false;

	ax_bs_note_cpu_pick(cpu);
	atomic64_inc(&ax_bs_background_guard_assists);
	return true;
}

static bool ax_bs_pick_reclaim_rq(struct task_struct *task, int prev_cpu,
				  struct ax_sched_cpu_pick *pick)
{
	int cpu;

	if (!pick || !READ_ONCE(ax_bs_reclaim_guard) ||
	    !ax_bs_task_is_reclaim_worker(task))
		return false;

	if (!ax_bs_has_transient_scene())
		return false;

	cpu = ax_bs_lowest_cpu(task);
	if (cpu == prev_cpu ||
	    !ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_BURST))
		return false;

	ax_bs_note_cpu_pick(cpu);
	atomic64_inc(&ax_bs_reclaim_assists);
	return true;
}

#if defined(AX_BS_HAS_SVP_POLICY)
#define AX_SVP_MAX_TARGETS 32
#define AX_SVP_MAX_THREADS 32
#define AX_SVP_MAX_TIDS 16
#define AX_SVP_CMD_SIZE 224
#define AX_SVP_PROC_TASKS "svp_tasks"
#define AX_SVP_PROC_THREADS "svp_threads"
#define AX_SVP_DEFAULT_UTIL 768
#define AX_SVP_DEFAULT_ANIMATION_UTIL 832
#define AX_SVP_DEFAULT_VISUAL_UTIL 896
#define AX_SVP_CLEANUP_INTERVAL_MS 5000
#define AX_SVP_MAX_DURATION_MS 2500
#define AX_SVP_LEVEL_MAX 3
#define AX_SVP_FLAG_RT BIT(1)

struct ax_svp_target {
	pid_t pid;
	int level;
	int tid_count;
	pid_t tids[AX_SVP_MAX_TIDS];
	struct pid *pid_ref;
	unsigned long expire_jiffies;
	bool sticky;
};

struct ax_svp_thread_target {
	pid_t tid;
	int level;
	unsigned int flags;
	struct pid *pid;
	int saved_policy;
	int saved_rt_priority;
	bool rt_active;
};

struct ax_svp_rt_state {
	struct pid *pid;
	int policy;
	int priority;
	bool active;
};

static unsigned int ax_svp_enabled = 1;
module_param_named(svp_enabled, ax_svp_enabled, uint, 0644);

static unsigned int ax_svp_util = AX_SVP_DEFAULT_UTIL;
module_param_named(svp_util, ax_svp_util, uint, 0644);

static unsigned int ax_svp_animation_util = AX_SVP_DEFAULT_ANIMATION_UTIL;
module_param_named(svp_animation_util, ax_svp_animation_util, uint, 0644);

static unsigned int ax_svp_visual_util = AX_SVP_DEFAULT_VISUAL_UTIL;
module_param_named(svp_visual_util, ax_svp_visual_util, uint, 0644);

static unsigned int ax_svp_big_only = 1;
module_param_named(svp_big_only, ax_svp_big_only, uint, 0644);

static unsigned int ax_svp_balance = 1;
module_param_named(svp_balance, ax_svp_balance, uint, 0644);

static unsigned int ax_svp_rt_enabled = 1;
module_param_named(svp_rt_enabled, ax_svp_rt_enabled, uint, 0644);

static unsigned int ax_svp_rt_prio = 1;
module_param_named(svp_rt_prio, ax_svp_rt_prio, uint, 0644);

static DEFINE_RAW_SPINLOCK(ax_svp_lock);
static struct delayed_work ax_svp_cleanup_work;
static struct ax_svp_target ax_svp_targets[AX_SVP_MAX_TARGETS];
static struct ax_svp_thread_target ax_svp_threads[AX_SVP_MAX_THREADS];
static atomic64_t ax_svp_wake_assists;
static atomic64_t ax_svp_fallback_assists;
static atomic64_t ax_svp_lowest_assists;
static atomic64_t ax_svp_balance_assists;
static atomic64_t ax_svp_migration_blocks;
static atomic64_t ax_svp_uclamp_assists;

static struct task_struct *ax_svp_get_task(struct pid *pid)
{
	struct task_struct *task;

	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}

static bool ax_svp_target_active(struct ax_svp_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	if (READ_ONCE(target->pid) <= 0 || READ_ONCE(target->level) <= 0 || !pid)
		return false;
	if (!READ_ONCE(target->sticky) &&
	    time_after_eq(jiffies, READ_ONCE(target->expire_jiffies)))
		return false;

	return ax_bs_pid_alive(pid);
}

static bool ax_svp_thread_active(struct ax_svp_thread_target *target)
{
	struct pid *pid = READ_ONCE(target->pid);

	return READ_ONCE(target->tid) > 0 &&
		READ_ONCE(target->level) > 0 &&
		pid &&
		ax_bs_pid_alive(pid);
}

static int ax_svp_task_level(struct task_struct *task)
{
	pid_t tid = task_pid_nr(task);
	pid_t tgid = task_tgid_nr(task);
	struct pid *task_pid_ref = task_pid(task);
	struct pid *task_tgid_ref = task_tgid(task);
	int i;
	int j;

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		struct ax_svp_thread_target *target = &ax_svp_threads[i];
		int level = READ_ONCE(target->level);

		if (READ_ONCE(target->tid) <= 0 ||
		    level <= 0 || READ_ONCE(target->pid) != task_pid_ref)
			continue;

		return level;
	}

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		struct ax_svp_target *target = &ax_svp_targets[i];
		struct pid *pid_ref = READ_ONCE(target->pid_ref);
		pid_t pid = READ_ONCE(target->pid);
		int level = READ_ONCE(target->level);
		int count;

		if (pid <= 0 || level <= 0 || tgid != pid ||
		    pid_ref != task_tgid_ref)
			continue;
		if (!READ_ONCE(target->sticky) &&
		    time_after_eq(jiffies, READ_ONCE(target->expire_jiffies)))
			continue;

		if (tid == pid)
			return level;

		count = min_t(int, READ_ONCE(target->tid_count), AX_SVP_MAX_TIDS);
		for (j = 0; j < count; j++) {
			if (tid == READ_ONCE(target->tids[j]))
				return level;
		}

		if (ax_sched_task_is_render_helper(task))
			return level;
	}

	return 0;
}

static unsigned int ax_svp_level_util(int level)
{
	if (level >= AX_SVP_LEVEL_MAX)
		return ax_sched_clamp_util(READ_ONCE(ax_svp_visual_util));
	if (level >= 2)
		return ax_sched_clamp_util(READ_ONCE(ax_svp_animation_util));
	if (level > 0)
		return ax_sched_clamp_util(READ_ONCE(ax_svp_util));
	return 0;
}

static unsigned int ax_svp_task_util(struct task_struct *task)
{
	if (!READ_ONCE(ax_svp_enabled))
		return 0;

	return ax_svp_level_util(ax_svp_task_level(task));
}

static int ax_svp_best_cpu_mask(struct task_struct *task,
				struct cpumask *local_cpu_mask, bool *balanced)
{
	unsigned int best_score = 0;
	unsigned int best_pressure = ~0U;
	bool balance = READ_ONCE(ax_svp_balance);
	bool spilled = false;
	int best_cpu = -1;
	int cpu;

	if (balanced)
		*balanced = false;
	if (balance && !ax_bs_task_is_primary_visual(task)) {
		best_cpu = ax_bs_spread_cpu_mask(task, local_cpu_mask, &spilled);
		if (best_cpu >= 0) {
			if (balanced)
				*balanced = spilled;
			return best_cpu;
		}
	}

	for_each_online_cpu(cpu) {
		unsigned int pressure = 0;
		unsigned int score;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
                if (!cpumask_test_cpu(cpu, task->cpus_ptr))
#else
                if (!cpumask_test_cpu(cpu, &task->cpus_allowed))
#endif
			continue;
		if (local_cpu_mask && !cpumask_test_cpu(cpu, local_cpu_mask))
			continue;

		score = ax_bs_get_cpu_score(cpu);
		if (!balance) {
			if (score >= best_score) {
				best_score = score;
				best_cpu = cpu;
			}
			continue;
		}

		pressure = ax_bs_cpu_pressure_score(cpu);
		if (score > best_score ||
		    (score == best_score && pressure <= best_pressure)) {
			if (balanced && best_cpu >= 0 && score == best_score &&
			    pressure < best_pressure)
				*balanced = true;
			best_score = score;
			best_pressure = pressure;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static int ax_svp_best_cpu(struct task_struct *task, bool *balanced)
{
	return ax_svp_best_cpu_mask(task, NULL, balanced);
}

static bool ax_svp_pick_task_rq(struct task_struct *task,
			       struct ax_sched_cpu_pick *pick)
{
	bool balanced = false;
	int cpu;

	if (!pick || !READ_ONCE(ax_svp_big_only) || !ax_svp_task_util(task))
		return false;

	cpu = ax_svp_best_cpu(task, &balanced);
	if (!ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_SVP))
		return false;

	ax_bs_note_matching_target_cpu(task, cpu);
	if (READ_ONCE(ax_svp_balance))
		ax_bs_note_cpu_pick(cpu);
	if (balanced)
		atomic64_inc(&ax_svp_balance_assists);
	atomic64_inc(&ax_svp_wake_assists);
	return true;
}

static bool ax_svp_pick_fallback_rq(int prev_cpu, struct task_struct *task,
				    struct ax_sched_cpu_pick *pick)
{
	bool balanced = false;
	int cpu;

	if (!pick || !READ_ONCE(ax_svp_big_only) || !ax_svp_task_util(task))
		return false;

	cpu = ax_svp_best_cpu(task, &balanced);
	if (cpu == prev_cpu)
		return false;

	if (!ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_SVP))
		return false;

	ax_bs_note_matching_target_cpu(task, cpu);
	if (READ_ONCE(ax_svp_balance))
		ax_bs_note_cpu_pick(cpu);
	if (balanced)
		atomic64_inc(&ax_svp_balance_assists);
	atomic64_inc(&ax_svp_fallback_assists);
	return true;
}

static bool ax_svp_pick_lowest_rq(struct task_struct *task,
				 struct cpumask *local_cpu_mask,
				 struct ax_sched_cpu_pick *pick)
{
	bool balanced = false;
	int cpu;

	if (!pick || !local_cpu_mask || !READ_ONCE(ax_svp_big_only) ||
	    !ax_svp_task_util(task))
		return false;

	cpu = ax_svp_best_cpu_mask(task, local_cpu_mask, &balanced);
	if (!ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_SVP))
		return false;

	ax_bs_note_matching_target_cpu(task, cpu);
	if (READ_ONCE(ax_svp_balance))
		ax_bs_note_cpu_pick(cpu);
	if (balanced)
		atomic64_inc(&ax_svp_balance_assists);
	atomic64_inc(&ax_svp_lowest_assists);
	return true;
}

#if defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
static void ax_svp_uclamp_eff_get(struct task_struct *task,
				  enum uclamp_id clamp_id,
				  struct uclamp_se *uclamp_max,
				  struct uclamp_se *uclamp_eff, int *ret)
{
	unsigned int util;

	if (!ret || !uclamp_eff || !task || clamp_id != UCLAMP_MIN)
		return;

	util = ax_svp_task_util(task);
	if (!util)
		return;

	util = ax_sched_merge_uclamp(uclamp_eff, util);
	util = max_t(unsigned int, util, task->uclamp_req[UCLAMP_MIN].value);
	util = max_t(unsigned int, util, task->uclamp[UCLAMP_MIN].value);
	if (uclamp_max)
		util = min_t(unsigned int, util, uclamp_max->value);

	ax_sched_set_uclamp(uclamp_eff, ax_sched_clamp_util(util));
	atomic64_inc(&ax_svp_uclamp_assists);
	*ret = 1;
}
#endif

static bool ax_svp_blocks_migration(struct task_struct *task, int src_cpu,
				    int dst_cpu)
{
	if (!READ_ONCE(ax_svp_enabled) || !READ_ONCE(ax_svp_balance) ||
	    !READ_ONCE(ax_svp_big_only) || !ax_svp_task_util(task))
		return false;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	if (!cpumask_test_cpu(dst_cpu, task->cpus_ptr))
#else
	if (!cpumask_test_cpu(dst_cpu, &task->cpus_allowed))
#endif
		return true;

	if (ax_bs_get_cpu_score(dst_cpu) >= ax_bs_get_cpu_score(src_cpu))
		return false;
	if (ax_bs_can_spill_to(task, src_cpu, dst_cpu))
		return false;

	atomic64_inc(&ax_svp_migration_blocks);
	return true;
}

static struct pid *ax_svp_detach_target(struct ax_svp_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	WRITE_ONCE(target->pid, -1);
	WRITE_ONCE(target->level, 0);
	WRITE_ONCE(target->tid_count, 0);
	WRITE_ONCE(target->expire_jiffies, 0);
	WRITE_ONCE(target->sticky, false);
	memset(target->tids, 0, sizeof(target->tids));
	smp_store_release(&target->pid_ref, NULL);

	return pid;
}

static void ax_svp_clear_target(struct ax_svp_target *target)
{
	ax_bs_release_pid(ax_svp_detach_target(target));
}

static void ax_svp_init_rt_state(struct ax_svp_rt_state *state)
{
	state->pid = NULL;
	state->policy = SCHED_NORMAL;
	state->priority = 0;
	state->active = false;
}

static void ax_svp_save_rt_state(struct ax_svp_rt_state *state,
					 struct ax_svp_thread_target *target)
{
	if (!READ_ONCE(target->rt_active))
		return;

	state->pid = get_pid(READ_ONCE(target->pid));
	state->policy = READ_ONCE(target->saved_policy);
	state->priority = READ_ONCE(target->saved_rt_priority);
	state->active = state->pid != NULL;
}

static struct pid *ax_svp_detach_thread_target(struct ax_svp_thread_target *target,
						       struct ax_svp_rt_state *rt_state)
{
	struct pid *pid = READ_ONCE(target->pid);

	if (rt_state)
		ax_svp_save_rt_state(rt_state, target);

	WRITE_ONCE(target->tid, -1);
	WRITE_ONCE(target->level, 0);
	WRITE_ONCE(target->flags, 0);
	WRITE_ONCE(target->saved_policy, SCHED_NORMAL);
	WRITE_ONCE(target->saved_rt_priority, 0);
	WRITE_ONCE(target->rt_active, false);
	smp_store_release(&target->pid, NULL);

	return pid;
}

static void ax_svp_clear_thread_target(struct ax_svp_thread_target *target)
{
	struct ax_svp_rt_state rt_state;
	struct pid *pid;

	ax_svp_init_rt_state(&rt_state);
	pid = ax_svp_detach_thread_target(target, &rt_state);
	ax_bs_release_pid(pid);
	ax_bs_release_pid(rt_state.pid);
}

static void ax_svp_restore_rt(struct ax_svp_rt_state *state)
{
	struct sched_param param;
	struct task_struct *task;

	if (!state->active || !state->pid)
		return;

	task = ax_svp_get_task(state->pid);
	if (!task)
		return;

	param.sched_priority = state->priority;
	sched_setscheduler_nocheck(task, state->policy, &param);
	put_task_struct(task);
}

static bool ax_svp_promote_rt(struct pid *pid, int *policy, int *priority)
{
	struct sched_param param;
	struct task_struct *task;
	unsigned int rt_prio;
	int ret;

	if (!pid || !READ_ONCE(ax_svp_rt_enabled))
		return false;

	task = ax_svp_get_task(pid);
	if (!task)
		return false;

	*policy = task->policy;
	*priority = task->rt_priority;
	if (*policy == SCHED_FIFO || *policy == SCHED_RR) {
		put_task_struct(task);
		return false;
	}
	rt_prio = clamp_t(unsigned int, READ_ONCE(ax_svp_rt_prio), 1,
			 MAX_RT_PRIO - 1);
	param.sched_priority = rt_prio;
	ret = sched_setscheduler_nocheck(task, SCHED_FIFO, &param);
	put_task_struct(task);

	return ret == 0;
}

static void ax_svp_clear_locked(pid_t pid)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		struct ax_svp_target *target = &ax_svp_targets[i];

		if (pid > 0 && READ_ONCE(target->pid) != pid)
			continue;

		ax_svp_clear_target(target);
	}
}

static void ax_svp_prune_locked(void)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		struct ax_svp_target *target = &ax_svp_targets[i];
		struct pid *pid = READ_ONCE(target->pid_ref);

		if (READ_ONCE(target->pid) > 0 &&
		    (!pid ||
		     (!READ_ONCE(target->sticky) &&
		      time_after_eq(jiffies, READ_ONCE(target->expire_jiffies))) ||
		     !ax_bs_pid_alive(pid)))
			ax_svp_clear_target(target);
	}

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		struct ax_svp_thread_target *target = &ax_svp_threads[i];
		struct pid *pid = READ_ONCE(target->pid);

		if (READ_ONCE(target->tid) > 0 &&
		    (!pid || !ax_bs_pid_alive(pid)))
			ax_svp_clear_thread_target(target);
	}
}

static struct ax_svp_thread_target *ax_svp_find_thread_locked(pid_t tid)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		if (READ_ONCE(ax_svp_threads[i].tid) == tid)
			return &ax_svp_threads[i];
	}

	return NULL;
}

static struct ax_svp_thread_target *ax_svp_alloc_thread_locked(void)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		if (READ_ONCE(ax_svp_threads[i].tid) <= 0)
			return &ax_svp_threads[i];
	}

	return NULL;
}

static struct pid *ax_svp_set_thread_locked(struct ax_svp_thread_target *target,
						    pid_t tid, int level,
						    unsigned int flags,
						    struct pid *pid,
						    struct ax_svp_rt_state *rt_state)
{
	struct pid *old_pid = ax_svp_detach_thread_target(target, rt_state);

	WRITE_ONCE(target->level, level);
	WRITE_ONCE(target->flags, flags);
	WRITE_ONCE(target->saved_policy, SCHED_NORMAL);
	WRITE_ONCE(target->saved_rt_priority, 0);
	WRITE_ONCE(target->rt_active, false);
	WRITE_ONCE(target->pid, pid);
	smp_store_release(&target->tid, tid);

	return old_pid;
}

static struct pid *ax_svp_clear_thread_locked(pid_t tid,
					      struct ax_svp_rt_state *rt_state)
{
	struct ax_svp_thread_target *target = ax_svp_find_thread_locked(tid);

	if (!target)
		return NULL;

	return ax_svp_detach_thread_target(target, rt_state);
}

static struct ax_svp_target *ax_svp_find_locked(pid_t pid)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		if (READ_ONCE(ax_svp_targets[i].pid) == pid)
			return &ax_svp_targets[i];
	}

	return NULL;
}

static struct ax_svp_target *ax_svp_alloc_locked(pid_t pid)
{
	struct ax_svp_target *target;
	int i;

	target = ax_svp_find_locked(pid);
	if (target)
		return target;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		target = &ax_svp_targets[i];
		if (READ_ONCE(target->pid) <= 0 || !ax_svp_target_active(target))
			return target;
	}

	return NULL;
}

static struct pid *ax_svp_set_target_locked(struct ax_svp_target *target,
							    pid_t pid, int level,
							    int duration_ms,
							    int requested_tids,
							    char **argv,
							    int argc,
							    struct pid *pid_ref)
{
	struct pid *old_pid = ax_svp_detach_target(target);
	bool sticky = duration_ms == 0;
	unsigned long delay;
	int parsed;
	int i;

	if (sticky) {
		delay = 0;
	} else {
		if (duration_ms < 0)
			duration_ms = 1;
		duration_ms = clamp_t(unsigned int, duration_ms, 1,
				       AX_SVP_MAX_DURATION_MS);
		delay = msecs_to_jiffies(duration_ms);
		if (!delay)
			delay = 1;
	}

	WRITE_ONCE(target->level, level);
	WRITE_ONCE(target->tid_count, 0);
	WRITE_ONCE(target->expire_jiffies, sticky ? 0 : jiffies + delay);
	WRITE_ONCE(target->sticky, sticky);
	memset(target->tids, 0, sizeof(target->tids));
	for (i = 0; i < requested_tids && i + 4 < argc; i++) {
		if (kstrtoint(strstrip(argv[i + 4]), 10, &parsed) || parsed <= 0)
			continue;
		target->tids[target->tid_count++] = parsed;
	}
	WRITE_ONCE(target->pid_ref, pid_ref);
	smp_store_release(&target->pid, pid);

	return old_pid;
}

static unsigned long ax_svp_target_delay(struct ax_svp_target *target)
{
	unsigned long expire = READ_ONCE(target->expire_jiffies);

	if (READ_ONCE(target->sticky))
		return msecs_to_jiffies(AX_SVP_CLEANUP_INTERVAL_MS);
	if (time_after(expire, jiffies))
		return max_t(unsigned long, expire - jiffies, 1);
	return 1;
}

static unsigned long ax_svp_next_delay_locked(void)
{
	unsigned long delay = 0;
	int i;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		struct ax_svp_target *target = &ax_svp_targets[i];

		if (ax_svp_target_active(target))
			delay = ax_bs_min_delay(delay, ax_svp_target_delay(target));
	}

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		if (ax_svp_thread_active(&ax_svp_threads[i]))
			delay = ax_bs_min_delay(delay,
						msecs_to_jiffies(AX_SVP_CLEANUP_INTERVAL_MS));
	}

	return delay;
}

static void ax_svp_queue_cleanup(unsigned long delay)
{
	if (delay)
		mod_delayed_work(system_wq, &ax_svp_cleanup_work, delay);
	else
		cancel_delayed_work(&ax_svp_cleanup_work);
}

static void ax_svp_cleanup_work_fn(struct work_struct *work)
{
	unsigned long flags;
	unsigned long delay;

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	ax_svp_prune_locked();
	delay = ax_svp_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);

	ax_svp_queue_cleanup(delay);
}

static int ax_svp_parse_int(char *text, int *value)
{
	return kstrtoint(strstrip(text), 10, value);
}

static ssize_t ax_svp_tasks_write(struct file *file, const char __user *buf,
					  size_t count, loff_t *ppos)
{
	char buffer[AX_SVP_CMD_SIZE];
	char *argv[5 + AX_SVP_MAX_TIDS];
	char *cursor;
	char *token;
	struct ax_svp_target *target;
	struct pid *pid_ref;
	struct pid *old_pid = NULL;
	unsigned long flags;
	unsigned long delay = 0;
	int argc = 0;
	int pid;
	int level;
	int duration_ms;
	int requested_tids;

	if (count == 0)
		return 0;

	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\n")) != NULL && argc < ARRAY_SIZE(argv)) {
		if (*token)
			argv[argc++] = token;
	}

	if (argc < 4)
		return -EINVAL;

	if (ax_svp_parse_int(argv[0], &pid) || ax_svp_parse_int(argv[1], &level) ||
	    ax_svp_parse_int(argv[2], &duration_ms) ||
	    ax_svp_parse_int(argv[3], &requested_tids))
		return -EINVAL;

	requested_tids = clamp(requested_tids, 0, AX_SVP_MAX_TIDS);

	if (level <= 0 || !READ_ONCE(ax_svp_enabled)) {
		raw_spin_lock_irqsave(&ax_svp_lock, flags);
		ax_svp_clear_locked(pid);
		delay = ax_svp_next_delay_locked();
		raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
		ax_svp_queue_cleanup(delay);
		return count;
	}

	if (pid <= 0)
		return -EINVAL;

	if (level > AX_SVP_LEVEL_MAX)
		level = AX_SVP_LEVEL_MAX;

	pid_ref = find_get_pid(pid);
	if (!pid_ref)
		return -ESRCH;

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	target = ax_svp_alloc_locked(pid);
	if (!target) {
		raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
		put_pid(pid_ref);
		return -ENOSPC;
	}
	old_pid = ax_svp_set_target_locked(target, pid, level, duration_ms,
						 requested_tids, argv, argc, pid_ref);
	delay = ax_svp_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);

	ax_bs_release_pid(old_pid);
	ax_svp_queue_cleanup(delay);
	return count;
}

static ssize_t ax_svp_threads_write(struct file *file, const char __user *buf,
					    size_t count, loff_t *ppos)
{
	char buffer[AX_SVP_CMD_SIZE];
	char *argv[4];
	char *cursor;
	char *token;
	struct ax_svp_thread_target *target;
	struct pid *pid;
	struct pid *old_pid = NULL;
	struct ax_svp_rt_state rt_state;
	unsigned long flags;
	unsigned long delay = 0;
	unsigned int thread_flags = 0;
	bool promote_rt = false;
	bool keep_rt = false;
	int saved_policy = SCHED_NORMAL;
	int saved_priority = 0;
	int keep_policy = SCHED_NORMAL;
	int keep_priority = 0;
	int argc = 0;
	int tid;
	int enabled;
	int level = AX_SVP_LEVEL_MAX;
	int parsed;

	if (count == 0)
		return 0;

	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\n")) != NULL && argc < ARRAY_SIZE(argv)) {
		if (*token)
			argv[argc++] = token;
	}

	if (argc < 2)
		return -EINVAL;

	if (ax_svp_parse_int(argv[0], &tid) || ax_svp_parse_int(argv[1], &enabled))
		return -EINVAL;

	if (argc > 2 && ax_svp_parse_int(argv[2], &level))
		return -EINVAL;

	if (argc > 3 && ax_svp_parse_int(argv[3], &parsed))
		return -EINVAL;
	if (argc > 3)
		thread_flags = parsed;

	if (tid <= 0)
		return -EINVAL;

	ax_svp_init_rt_state(&rt_state);
	if (enabled <= 0 || !READ_ONCE(ax_svp_enabled)) {
		raw_spin_lock_irqsave(&ax_svp_lock, flags);
		old_pid = ax_svp_clear_thread_locked(tid, &rt_state);
		delay = ax_svp_next_delay_locked();
		raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
		ax_svp_restore_rt(&rt_state);
		ax_bs_release_pid(rt_state.pid);
		ax_bs_release_pid(old_pid);
		ax_svp_queue_cleanup(delay);
		return count;
	}

	level = clamp(level, 1, AX_SVP_LEVEL_MAX);
	pid = find_get_pid(tid);
	if (!pid)
		return -ESRCH;

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	target = ax_svp_find_thread_locked(tid);
	if (target && READ_ONCE(target->rt_active)) {
		if (thread_flags & AX_SVP_FLAG_RT) {
			keep_rt = true;
			keep_policy = READ_ONCE(target->saved_policy);
			keep_priority = READ_ONCE(target->saved_rt_priority);
		} else {
			ax_svp_save_rt_state(&rt_state, target);
		}
	}
	if (!target)
		target = ax_svp_alloc_thread_locked();
	if (!target) {
		raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
		put_pid(pid);
		return -ENOSPC;
	}
	promote_rt = (thread_flags & AX_SVP_FLAG_RT) && !READ_ONCE(target->rt_active);
	old_pid = ax_svp_set_thread_locked(target, tid, level, thread_flags, pid, NULL);
	if (keep_rt) {
		WRITE_ONCE(target->saved_policy, keep_policy);
		WRITE_ONCE(target->saved_rt_priority, keep_priority);
		WRITE_ONCE(target->rt_active, true);
	}
	delay = ax_svp_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);

	if (rt_state.active) {
		ax_svp_restore_rt(&rt_state);
		ax_bs_release_pid(rt_state.pid);
	}
	ax_bs_release_pid(old_pid);

	if (promote_rt && ax_svp_promote_rt(pid, &saved_policy, &saved_priority)) {
		raw_spin_lock_irqsave(&ax_svp_lock, flags);
		target = ax_svp_find_thread_locked(tid);
		if (target && READ_ONCE(target->pid) == pid &&
		    (READ_ONCE(target->flags) & AX_SVP_FLAG_RT)) {
			WRITE_ONCE(target->saved_policy, saved_policy);
			WRITE_ONCE(target->saved_rt_priority, saved_priority);
			WRITE_ONCE(target->rt_active, true);
		} else {
			rt_state.pid = get_pid(pid);
			rt_state.policy = saved_policy;
			rt_state.priority = saved_priority;
			rt_state.active = true;
		}
		raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
		if (rt_state.active) {
			ax_svp_restore_rt(&rt_state);
			ax_bs_release_pid(rt_state.pid);
		}
	}

	ax_svp_queue_cleanup(delay);
	return count;
}

static int ax_svp_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	ax_svp_prune_locked();
	for (i = 0; i < AX_SVP_MAX_TARGETS; i++) {
		struct ax_svp_target *target = &ax_svp_targets[i];
		unsigned long remaining = 0;
		int count;
		int j;

		if (READ_ONCE(target->pid) <= 0)
			continue;

		if (time_after(READ_ONCE(target->expire_jiffies), jiffies))
			remaining = jiffies_to_msecs(READ_ONCE(target->expire_jiffies) - jiffies);
		count = min_t(int, target->tid_count, AX_SVP_MAX_TIDS);
		seq_printf(m, "%d %d %lu %d", target->pid, target->level,
			   remaining, count);
		for (j = 0; j < count; j++)
			seq_printf(m, " %d", target->tids[j]);
		seq_putc(m, '\n');
	}
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
	return 0;
}

static int ax_svp_threads_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		struct ax_svp_thread_target *target = &ax_svp_threads[i];
		struct pid *pid = READ_ONCE(target->pid);

		if (READ_ONCE(target->tid) <= 0 || !pid)
			continue;

		seq_printf(m, "%d %d %d %u\n", target->tid, target->level,
			   ax_svp_thread_active(target) ? 1 : 0, target->flags);
	}
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);
	return 0;
}

static int ax_svp_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_svp_show, NULL);
}

static int ax_svp_threads_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_svp_threads_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops ax_svp_tasks_fops = {
	.proc_open = ax_svp_open,
	.proc_read = seq_read,
	.proc_write = ax_svp_tasks_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops ax_svp_threads_fops = {
	.proc_open = ax_svp_threads_open,
	.proc_read = seq_read,
	.proc_write = ax_svp_threads_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations ax_svp_tasks_fops = {
	.owner = THIS_MODULE,
	.open = ax_svp_open,
	.read = seq_read,
	.write = ax_svp_tasks_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations ax_svp_threads_fops = {
	.owner = THIS_MODULE,
	.open = ax_svp_threads_open,
	.read = seq_read,
	.write = ax_svp_threads_write,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif
#endif

#if defined(AX_BS_HAS_LOCK_BOOST)
#define AX_BS_RWSEM_READER_OWNED BIT(0)
#define AX_BS_RWSEM_OWNER_FLAGS_MASK (BIT(0) | BIT(1) | BIT(2))
#define AX_BS_RWSEM_OWNER_UNKNOWN (~1UL)

static unsigned int ax_bs_lock_waiter_util(struct task_struct *task)
{
	unsigned int util = ax_bs_task_util(task);

#if defined(AX_BS_HAS_FRAME_BOOST)
	util = max_t(unsigned int, util, ax_frame_boost_task_util(task));
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	util = max_t(unsigned int, util, ax_svp_task_util(task));
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	util = max_t(unsigned int, util, ax_game_boost_task_util(task));
#endif

	return util;
}

static unsigned int ax_bs_lock_boost_util(unsigned int waiter_util)
{
	unsigned int util;
	unsigned int cap;

	if (!waiter_util || !READ_ONCE(ax_bs_lock_boost))
		return 0;

	util = max_t(unsigned int, waiter_util,
		     ax_sched_clamp_util(READ_ONCE(ax_bs_lock_util)));
	cap = ax_sched_clamp_util(READ_ONCE(ax_bs_lock_max_util));
	cap = min_t(unsigned int, cap, ax_sched_clamp_util(READ_ONCE(ax_bs_util_cap)));
	if (!cap)
		return 0;

	return min_t(unsigned int, util, cap);
}

static struct task_struct *ax_bs_rwsem_owner(struct rw_semaphore *sem)
{
	unsigned long owner;

	if (!sem)
		return NULL;

	owner = atomic_long_read(&sem->owner);
	if (!owner || owner == AX_BS_RWSEM_OWNER_UNKNOWN ||
	    (owner & AX_BS_RWSEM_READER_OWNED))
		return NULL;

	owner &= ~AX_BS_RWSEM_OWNER_FLAGS_MASK;
	if (!owner)
		return NULL;

	return (struct task_struct *)owner;
}

static bool ax_bs_track_lock_owner(struct task_struct *owner,
				   unsigned int waiter_util)
{
	struct ax_bs_lock_target *target;
	struct pid *pid;
	struct pid *old_pid = NULL;
	unsigned long expire;
	unsigned long flags;
	unsigned long delay;
	unsigned int timeout_ms;
	unsigned int util;
	pid_t tid;

	if (!owner || owner == current || owner->prio < MAX_RT_PRIO ||
	    !pid_alive(owner))
		return false;

	util = ax_bs_lock_boost_util(waiter_util);
	if (!util)
		return false;

	pid = get_task_pid(owner, PIDTYPE_PID);
	if (!pid)
		return false;

	tid = task_pid_nr(owner);
	if (tid <= 0) {
		put_pid(pid);
		return false;
	}

	timeout_ms = clamp_t(unsigned int, READ_ONCE(ax_bs_lock_timeout_ms), 1, 512);
	expire = jiffies + max_t(unsigned long, 1, msecs_to_jiffies(timeout_ms));

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	target = ax_bs_alloc_lock_target_locked(tid);
	if (target)
		old_pid = ax_bs_set_lock_target_locked(target, tid, pid, util, expire);
	delay = ax_bs_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);

	if (!target) {
		put_pid(pid);
		return false;
	}

	ax_bs_release_pid(old_pid);
	ax_bs_schedule_cleanup(delay);
	return true;
}

static void ax_bs_clear_lock_task(struct task_struct *task)
{
	struct pid *old_pids[AX_BS_MAX_LOCK_TARGETS];
	struct pid *pid_ref;
	unsigned long flags;
	unsigned long delay;
	pid_t tid;
	int count = 0;
	int i;

	if (!task || atomic_read(&ax_bs_lock_active_count) <= 0)
		return;

	tid = task_pid_nr(task);
	pid_ref = task_pid(task);
	if (tid <= 0 || !pid_ref)
		return;

	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++)
		old_pids[i] = NULL;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		struct ax_bs_lock_target *target = &ax_bs_lock_targets[i];

		if (READ_ONCE(target->tid) != tid ||
		    READ_ONCE(target->pid_ref) != pid_ref)
			continue;

		old_pids[count++] = ax_bs_detach_lock_target(target);
	}
	delay = ax_bs_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);

	for (i = 0; i < count; i++)
		ax_bs_release_pid(old_pids[i]);
	if (count)
		ax_bs_schedule_cleanup(delay);
}

static void ax_bs_rwsem_wake(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *owner;
	unsigned int util;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_lock_boost))
		return;

	util = ax_bs_lock_waiter_util(current);
	if (!util)
		return;

	owner = ax_bs_rwsem_owner(sem);
	if (ax_bs_track_lock_owner(owner, util))
		atomic64_inc(&ax_bs_lock_assists);
}

static void ax_bs_rwsem_write_finished(void *unused, struct rw_semaphore *sem)
{
	ax_bs_clear_lock_task(current);
}

#if defined(AX_BS_HAS_MUTEX_HOOK)
#define AX_BS_MUTEX_OWNER_FLAGS 0x07

static struct task_struct *ax_bs_mutex_owner(struct mutex *lock)
{
	unsigned long owner;

	if (!lock)
		return NULL;

	owner = atomic_long_read(&lock->owner);
	owner &= ~AX_BS_MUTEX_OWNER_FLAGS;
	if (!owner)
		return NULL;

	return (struct task_struct *)owner;
}

static void ax_bs_mutex_wait_start(void *unused, struct mutex *lock)
{
	struct task_struct *owner;
	unsigned int util;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_lock_boost))
		return;

	util = ax_bs_lock_waiter_util(current);
	if (!util)
		return;

	owner = ax_bs_mutex_owner(lock);
	if (ax_bs_track_lock_owner(owner, util))
		atomic64_inc(&ax_bs_lock_assists);
}

static void ax_bs_mutex_wait_finish(void *unused, struct mutex *lock)
{
	ax_bs_clear_lock_task(current);
}
#endif
#endif

static bool ax_bs_pick_task_rq(struct task_struct *task,
			       struct ax_sched_cpu_pick *pick)
{
	struct ax_bs_target *launch_worker_target;
	struct ax_bs_target *target;
	unsigned int auto_top_app_util;
	unsigned int launch_worker_util;
	unsigned int lock_util;
	unsigned int support_util;
	unsigned int target_util;
	unsigned int util;
	bool primary;
	int cpu;

	if (!pick || !READ_ONCE(ax_bs_big_only))
		return false;

	target_util = ax_bs_match_target_util(task, &target, NULL);
	auto_top_app_util = ax_bs_auto_top_app_task_util(task);
	launch_worker_target = ax_bs_match_launch_worker_target(task);
	launch_worker_util = ax_bs_launch_worker_util_for(launch_worker_target);
	lock_util = ax_bs_lock_task_util(task);
	support_util = ax_bs_support_task_util(task);
	primary = ax_bs_task_is_primary_visual(task);
	if (!target_util && !auto_top_app_util && !launch_worker_util &&
	    !lock_util && !support_util)
		return false;

	util = max_t(unsigned int, max(target_util, auto_top_app_util),
		     max_t(unsigned int, launch_worker_util,
			   max_t(unsigned int, lock_util, support_util)));
	cpu = ax_bs_pick_cpu(task, util, lock_util || primary,
			     !lock_util && !primary);
	if (!ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_BURST))
		return false;

	ax_bs_note_target_cpu(target, task, cpu);
	if (target_util)
		atomic64_inc(&target->hits);
	if (launch_worker_util)
		atomic64_inc(&launch_worker_target->hits);
#if defined(AX_BS_HAS_LOCK_BOOST)
	if (lock_util) {
		atomic64_inc(&ax_bs_lock_hits);
		atomic64_inc(&ax_bs_lock_wake_assists);
	}
#endif
	if (support_util)
		atomic64_inc(&ax_bs_support_worker_assists);
	atomic64_inc(&ax_bs_wake_assists);
	return true;
}

static bool ax_bs_pick_fallback_rq(int prev_cpu, struct task_struct *task,
				   struct ax_sched_cpu_pick *pick)
{
	struct ax_bs_target *launch_worker_target;
	struct ax_bs_target *target;
	unsigned int auto_top_app_util;
	unsigned int launch_worker_util;
	unsigned int lock_util;
	unsigned int support_util;
	unsigned int target_util;
	unsigned int util;
	bool primary;
	int cpu;

	if (!pick || !READ_ONCE(ax_bs_fallback_big))
		return false;

	target_util = ax_bs_match_target_util(task, &target, NULL);
	auto_top_app_util = ax_bs_auto_top_app_task_util(task);
	launch_worker_target = ax_bs_match_launch_worker_target(task);
	launch_worker_util = ax_bs_launch_worker_util_for(launch_worker_target);
	lock_util = ax_bs_lock_task_util(task);
	support_util = ax_bs_support_task_util(task);
	primary = ax_bs_task_is_primary_visual(task);
	if (!target_util && !auto_top_app_util && !launch_worker_util &&
	    !lock_util && !support_util)
		return false;

	util = max_t(unsigned int, max(target_util, auto_top_app_util),
		     max_t(unsigned int, launch_worker_util,
			   max_t(unsigned int, lock_util, support_util)));
	cpu = ax_bs_pick_cpu(task, util, lock_util || primary,
			     !lock_util && !primary);
	if (cpu == prev_cpu ||
	    !ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_BURST))
		return false;

	ax_bs_note_target_cpu(target, task, cpu);
	if (target_util)
		atomic64_inc(&target->hits);
	if (launch_worker_util)
		atomic64_inc(&launch_worker_target->hits);
#if defined(AX_BS_HAS_LOCK_BOOST)
	if (lock_util) {
		atomic64_inc(&ax_bs_lock_hits);
		atomic64_inc(&ax_bs_lock_fallback_assists);
	}
#endif
	if (support_util)
		atomic64_inc(&ax_bs_support_worker_assists);
	atomic64_inc(&ax_bs_fallback_assists);
	return true;
}

#if defined(AX_BS_HAS_FRAME_BOOST)
static bool ax_bs_pick_frame_rq(struct task_struct *task,
				const struct cpumask *cpu_mask, int prev_cpu,
				bool fallback,
				struct ax_sched_cpu_pick *pick)
{
	unsigned int util = ax_frame_boost_task_util(task);
	int cpu;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_big_only) ||
	    (fallback && !READ_ONCE(ax_bs_fallback_big)) || !util)
		return false;

	cpu = ax_bs_pick_cpu_mask(task, cpu_mask, util, true, false);
	if (cpu == prev_cpu)
		return false;

	if (!ax_sched_set_cpu_pick(pick, cpu, AX_SCHED_PRIO_FRAME))
		return false;
#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	ax_frame_boost_note_cpu(task, cpu);
#endif
	return true;
}
#endif

static void ax_bs_select_task_rq(void *unused, struct task_struct *task,
					 int prev_cpu, int sd_flag, int wake_flags,
					 int *new_cpu)
{
	struct ax_sched_cpu_pick pick;

	if (!new_cpu)
		return;

#if defined(AX_BS_HAS_INPUT_HOOK)
	ax_bs_note_auto_top_app(task);
#endif
	if (!ax_bs_cpu_topology_ready())
		return;
	ax_sched_init_cpu_pick(&pick);

#if defined(AX_BS_HAS_FRAME_BOOST)
	ax_bs_pick_frame_rq(task, NULL, AX_SCHED_CPU_NONE, false, &pick);
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_svp_pick_task_rq(task, &pick);
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_game_boost_pick_task_rq(task, &pick);
#endif
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_task_rq(task, &pick);
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_background_rq(task, prev_cpu, &pick);
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_reclaim_rq(task, prev_cpu, &pick);

	if (ax_sched_has_cpu_pick(&pick)) {
		ax_bs_note_auto_cpu(task, pick.cpu);
		*new_cpu = pick.cpu;
	}
}

static void ax_bs_select_fallback_rq(void *unused, int prev_cpu,
				     struct task_struct *task, int *new_cpu)
{
	struct ax_sched_cpu_pick pick;

	if (!new_cpu || !ax_bs_cpu_topology_ready())
		return;

	ax_sched_init_cpu_pick(&pick);

#if defined(AX_BS_HAS_FRAME_BOOST)
	ax_bs_pick_frame_rq(task, NULL, prev_cpu, true, &pick);
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_svp_pick_fallback_rq(prev_cpu, task, &pick);
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_game_boost_pick_fallback_rq(prev_cpu, task, &pick);
#endif
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_fallback_rq(prev_cpu, task, &pick);
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_background_rq(task, prev_cpu, &pick);
	if (!ax_sched_has_cpu_pick(&pick))
		ax_bs_pick_reclaim_rq(task, prev_cpu, &pick);

	if (ax_sched_has_cpu_pick(&pick)) {
		ax_bs_note_auto_cpu(task, pick.cpu);
		*new_cpu = pick.cpu;
	}
}

#if defined(AX_BS_HAS_FRAME_BOOST) || defined(AX_BS_HAS_SVP_POLICY) || defined(AX_BS_HAS_GAME_BOOST)
static void ax_bs_find_lowest_rq(void *unused, struct task_struct *task,
				 struct cpumask *local_cpu_mask,
#if defined(AX_BS_FIND_LOWEST_RQ_HAS_RET)
				 int ret,
#endif
				 int *lowest_cpu)
{
	struct ax_sched_cpu_pick pick;

#if defined(AX_BS_FIND_LOWEST_RQ_HAS_RET)
	(void)ret;
#endif
	if (!lowest_cpu || !ax_bs_cpu_topology_ready())
		return;

	ax_sched_init_cpu_pick(&pick);

#if defined(AX_BS_HAS_FRAME_BOOST)
	ax_bs_pick_frame_rq(task, local_cpu_mask, AX_SCHED_CPU_NONE, false, &pick);
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_svp_pick_lowest_rq(task, local_cpu_mask, &pick);
#endif
#if defined(AX_BS_HAS_GAME_BOOST)
	if (!ax_sched_has_cpu_pick(&pick))
		ax_game_boost_pick_lowest_rq(task, local_cpu_mask, &pick);
#endif

	if (ax_sched_has_cpu_pick(&pick)) {
		ax_bs_note_auto_cpu(task, pick.cpu);
		*lowest_cpu = pick.cpu;
	}
}
#endif

unsigned int ax_burst_sched_task_util(struct task_struct *task)
{
	return ax_bs_task_util(task);
}
EXPORT_SYMBOL_GPL(ax_burst_sched_task_util);

unsigned int ax_burst_sched_binder_util(struct task_struct *task)
{
	return ax_bs_task_util(task);
}
EXPORT_SYMBOL_GPL(ax_burst_sched_binder_util);

void ax_burst_sched_note_binder_assist(struct task_struct *task,
				       unsigned int util_min)
{
	struct ax_bs_target *target;

	if (!task || !util_min)
		return;

	if (!ax_bs_match_target_util(task, &target, NULL))
		return;

	if (!ax_bs_note_wakeup(target, task, false, true))
		return;
	atomic64_inc(&ax_bs_binder_assists);
}
EXPORT_SYMBOL_GPL(ax_burst_sched_note_binder_assist);

static void ax_bs_can_migrate_task(void *unused, struct task_struct *task,
				   int dst_cpu, int *can_migrate)
{
	struct ax_bs_target *launch_worker_target;
	struct ax_bs_target *target;
	unsigned int auto_top_app_util;
	unsigned int launch_worker_util;
	unsigned int lock_util;
	unsigned int game_util = 0;
	unsigned int support_util;
	unsigned int target_util;
	int src_cpu;

	if (!task || !can_migrate || !*can_migrate ||
	    !ax_bs_cpu_topology_ready())
		return;

	if (dst_cpu < 0 || dst_cpu >= nr_cpu_ids || !cpu_online(dst_cpu))
		return;

	src_cpu = task_cpu(task);
	if (src_cpu < 0 || src_cpu >= nr_cpu_ids || !cpu_online(src_cpu))
		return;

#if defined(AX_BS_HAS_SVP_POLICY)
	if (ax_svp_blocks_migration(task, src_cpu, dst_cpu)) {
		*can_migrate = 0;
		return;
	}
#endif

	if (!READ_ONCE(ax_bs_migration_assist) || !READ_ONCE(ax_bs_big_only))
		return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
        if (!cpumask_test_cpu(dst_cpu, task->cpus_ptr)) {
#else
        if (!cpumask_test_cpu(dst_cpu, &task->cpus_allowed)) {
#endif
		*can_migrate = 0;
		return;
	}

	if (READ_ONCE(ax_bs_background_guard) &&
	    READ_ONCE(ax_bs_scene_focus) &&
	    ax_bs_task_is_background_guard_candidate(task) &&
	    ax_bs_get_cpu_score(dst_cpu) > ax_bs_get_cpu_score(src_cpu)) {
		atomic64_inc(&ax_bs_background_guard_assists);
		*can_migrate = 0;
		return;
	}

	target_util = ax_bs_match_target_util(task, &target, NULL);
	auto_top_app_util = ax_bs_auto_top_app_task_util(task);
	launch_worker_target = ax_bs_match_launch_worker_target(task);
	launch_worker_util = ax_bs_launch_worker_util_for(launch_worker_target);
	lock_util = ax_bs_lock_task_util(task);
	support_util = ax_bs_support_task_util(task);
#if defined(AX_BS_HAS_GAME_BOOST)
	game_util = ax_game_boost_task_util(task);
#endif
	if (!target_util && !auto_top_app_util && !launch_worker_util && !lock_util &&
	    !support_util && !game_util)
		return;

	if (ax_bs_get_cpu_score(dst_cpu) < ax_bs_get_cpu_score(src_cpu)) {
		if (!lock_util && !game_util &&
		    ax_bs_can_spill_to(task, src_cpu, dst_cpu))
			return;
		if (target_util && !launch_worker_util && !lock_util && !game_util &&
		    !ax_bs_should_hold_migration(target)) {
			atomic64_inc(&ax_bs_migration_releases);
			return;
		}
		if (target_util)
			atomic64_inc(&target->hits);
		if (launch_worker_util)
			atomic64_inc(&launch_worker_target->hits);
#if defined(AX_BS_HAS_LOCK_BOOST)
		if (lock_util) {
			atomic64_inc(&ax_bs_lock_hits);
			atomic64_inc(&ax_bs_lock_migration_blocks);
		}
#endif
		if (support_util)
			atomic64_inc(&ax_bs_support_worker_assists);
		atomic64_inc(&ax_bs_migration_blocks);
		*can_migrate = 0;
	}
}

#if defined(AX_BS_ENQUEUE_TASK_HAS_FLAGS)
static void ax_bs_enqueue_task(void *unused, struct rq *rq,
			       struct task_struct *task, int flags)
#else
static void ax_bs_enqueue_task(void *unused, struct rq *rq,
			       struct task_struct *task)
#endif
{
	struct ax_bs_target *launch_worker_target;
	struct ax_bs_target *target;
	unsigned int auto_top_app_util;
	unsigned int launch_worker_util;
	unsigned int lock_util;
	unsigned int support_util;

#if defined(AX_BS_ENQUEUE_TASK_HAS_FLAGS)
	(void)flags;
#endif
	launch_worker_target = ax_bs_match_launch_worker_target(task);
	auto_top_app_util = ax_bs_auto_top_app_task_util(task);
	launch_worker_util = ax_bs_launch_worker_util_for(launch_worker_target);
	lock_util = ax_bs_lock_task_util(task);
	support_util = ax_bs_support_task_util(task);

	ax_bs_match_target_util(task, &target, NULL);

#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	if (target)
		ax_bs_note_target_cpu(target, task, task_cpu(task));
	if (auto_top_app_util)
		ax_bs_note_auto_cpu(task, task_cpu(task));
#endif

#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_task_util(task))
		ax_frame_boost_enqueue_task(task);
#endif

	if (target || auto_top_app_util || launch_worker_util || lock_util || support_util)
		atomic64_inc(&ax_bs_enqueue_events);
	ax_bs_note_wakeup(target ? target : launch_worker_target, task,
			  launch_worker_util != 0,
			  launch_worker_util != 0);
}

#if defined(AX_BS_DEQUEUE_TASK_HAS_FLAGS)
static void ax_bs_dequeue_task(void *unused, struct rq *rq,
			       struct task_struct *task, int flags)
#else
static void ax_bs_dequeue_task(void *unused, struct rq *rq,
			       struct task_struct *task)
#endif
{
	struct ax_bs_target *launch_worker_target =
		ax_bs_match_launch_worker_target(task);
	struct ax_bs_target *target;
	unsigned int launch_worker_util =
		ax_bs_launch_worker_util_for(launch_worker_target);
	unsigned int auto_top_app_util = ax_bs_auto_top_app_task_util(task);
	unsigned int lock_util = ax_bs_lock_task_util(task);
	unsigned int support_util = ax_bs_support_task_util(task);

#if defined(AX_BS_DEQUEUE_TASK_HAS_FLAGS)
	(void)flags;
#endif
	ax_bs_match_target_util(task, &target, NULL);

#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_task_util(task))
		ax_frame_boost_dequeue_task(task);
#endif

	if (target) {
		atomic64_inc(&target->hits);
		atomic64_inc(&ax_bs_dequeue_events);
	}
	if (launch_worker_util)
		atomic64_inc(&launch_worker_target->hits);
#if defined(AX_BS_HAS_LOCK_BOOST)
	if (lock_util)
		atomic64_inc(&ax_bs_lock_hits);
#endif
	if ((auto_top_app_util || launch_worker_util || lock_util || support_util) && !target)
		atomic64_inc(&ax_bs_dequeue_events);
}

#if defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
static void ax_bs_uclamp_eff_get(void *unused, struct task_struct *task,
				 enum uclamp_id clamp_id,
				 struct uclamp_se *uclamp_max,
				 struct uclamp_se *uclamp_eff, int *ret)
{
	struct ax_bs_target *launch_worker_target;
	unsigned int launch_worker_util;
	unsigned int lock_util;
	unsigned int game_util = 0;
	unsigned int support_util;
	unsigned int util;

	if (!ret || !uclamp_eff || !task || clamp_id != UCLAMP_MIN)
		return;

#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_task_util(task)) {
		ax_frame_boost_uclamp_eff_get(task, clamp_id, uclamp_max,
					      uclamp_eff, ret);
		if (*ret)
			return;
	}
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	ax_svp_uclamp_eff_get(task, clamp_id, uclamp_max, uclamp_eff, ret);
	if (*ret)
		return;
#endif

	util = ax_bs_match_target_util(task, NULL, NULL);
	util = max_t(unsigned int, util, ax_bs_auto_top_app_task_util(task));
	launch_worker_target = ax_bs_match_launch_worker_target(task);
	launch_worker_util = ax_bs_launch_worker_util_for(launch_worker_target);
	lock_util = ax_bs_lock_task_util(task);
	support_util = ax_bs_support_task_util(task);
#if defined(AX_BS_HAS_GAME_BOOST)
	game_util = ax_game_boost_task_util(task);
#endif
	util = max_t(unsigned int, util, launch_worker_util);
	util = max_t(unsigned int, util, lock_util);
	util = max_t(unsigned int, util, support_util);
	util = max_t(unsigned int, util, game_util);
	if (!util)
		return;

	util = ax_sched_merge_uclamp(uclamp_eff, util);
	util = max_t(unsigned int, util, task->uclamp_req[UCLAMP_MIN].value);
	util = max_t(unsigned int, util, task->uclamp[UCLAMP_MIN].value);
	if (uclamp_max)
		util = min_t(unsigned int, util, uclamp_max->value);

	ax_sched_set_uclamp(uclamp_eff, ax_sched_clamp_util(util));
#if defined(AX_BS_HAS_LOCK_BOOST)
	if (lock_util) {
		atomic64_inc(&ax_bs_lock_hits);
		atomic64_inc(&ax_bs_lock_uclamp_assists);
	}
#endif
	if (launch_worker_util)
		atomic64_inc(&launch_worker_target->hits);
	if (support_util)
		atomic64_inc(&ax_bs_support_worker_assists);
	atomic64_inc(&ax_bs_uclamp_assists);
	*ret = 1;
}
#endif

#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
static unsigned long ax_bs_active_util(struct cpufreq_policy *policy)
{
	struct ax_bs_auto_state state;
	unsigned long util;

	if (!READ_ONCE(ax_bs_enabled) || !READ_ONCE(ax_bs_freq_assist))
		return 0;

	util = ax_bs_active_target_util(policy);
	if (!ax_bs_read_active_auto_state(&state))
		return util;

	if (ax_bs_policy_matches_cpu(policy, state.cpu))
		util = max_t(unsigned long, util,
			     ax_bs_auto_target_util_for(&state));

	return util;
}

static void ax_bs_map_util_freq(void *unused, unsigned long util,
				unsigned long freq, unsigned long cap,
				unsigned long *next_freq
#if defined(AX_BS_MAP_UTIL_HAS_POLICY)
				, struct cpufreq_policy *policy
#endif
#if defined(AX_BS_MAP_UTIL_HAS_NEED_FREQ_UPDATE)
				, bool *need_freq_update
#endif
				)
{
	unsigned long active_util;
	unsigned long requested;
	struct cpufreq_policy *active_policy = NULL;

#if defined(AX_BS_MAP_UTIL_HAS_POLICY)
	active_policy = policy;
#endif
#if defined(AX_BS_MAP_UTIL_HAS_NEED_FREQ_UPDATE)
	(void)need_freq_update;
#endif

	if (!next_freq || !freq || !cap)
		return;

#if defined(AX_BS_HAS_FRAME_BOOST)
	if (ax_frame_boost_active_util())
		ax_frame_boost_map_util_freq(util, freq, cap, next_freq,
					     active_policy);
#endif

	active_util = ax_bs_active_util(active_policy);
	if (!active_util)
		return;

	active_util = min_t(unsigned long, max(util, active_util), cap);
	requested = freq * active_util / cap;
	if (requested > *next_freq) {
		*next_freq = requested;
		atomic64_inc(&ax_bs_freq_assists);
	}
}
#endif

static int ax_bs_parse_int(char *text, int *value)
{
	return kstrtoint(strstrip(text), 10, value);
}

static ssize_t ax_bs_scene_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char buffer[AX_BS_CMD_SIZE];
	char *argv[8 + AX_BS_MAX_TIDS];
	char *cursor;
	char *token;
	struct ax_bs_target *target;
	struct pid *pid_ref;
	struct pid *old_pid = NULL;
	unsigned int clear_sources[AX_BS_MAX_TARGETS];
	unsigned int old_resource_source;
	unsigned int resource_source;
	pid_t clear_pids[AX_BS_MAX_TARGETS];
#if defined(AX_BS_HAS_GAME_BOOST)
	pid_t game_tids[AX_BS_MAX_TIDS];
#endif
	unsigned long flags;
	unsigned long delay = 0;
	unsigned long expire;
	unsigned int duration_ms;
	bool sticky;
	int argc = 0;
	int pid;
	int uid;
	int mode;
	int source;
	int severity;
	int role = AX_BS_ROLE_APP;
	int parsed;
	int requested_tids;
#if defined(AX_BS_HAS_GAME_BOOST)
	int game_tid_count = 0;
#endif
	int clear_source_count = 0;
	int clear_pid_count = 0;
	int i;

	if (count == 0)
		return 0;

	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\n")) != NULL && argc < ARRAY_SIZE(argv)) {
		if (*token)
			argv[argc++] = token;
	}

	if (argc < 7)
		return -EINVAL;

	if (ax_bs_parse_int(argv[0], &pid) || ax_bs_parse_int(argv[1], &uid) ||
	    ax_bs_parse_int(argv[2], &mode) || ax_bs_parse_int(argv[3], &source) ||
	    ax_bs_parse_int(argv[4], &severity) ||
	    ax_bs_parse_int(argv[5], &parsed) ||
	    ax_bs_parse_int(argv[6], &requested_tids))
		return -EINVAL;

	duration_ms = parsed > 0 ? parsed : 0;
	requested_tids = clamp(requested_tids, 0, AX_BS_MAX_TIDS);
	if (argc > 7 + requested_tids &&
	    !ax_bs_parse_int(argv[7 + requested_tids], &parsed))
		role = parsed;
	role = clamp(role, AX_BS_ROLE_APP, AX_BS_ROLE_MAX);
	if (source <= 0)
		source = mode;
	sticky = (mode == AX_BS_MODE_TOP_APP ||
		  source == AX_BS_SOURCE_GAME) && duration_ms == 0;

	if (mode <= 0 || (!sticky && duration_ms == 0) || !READ_ONCE(ax_bs_enabled)) {
		raw_spin_lock_irqsave(&ax_bs_lock, flags);
		ax_bs_clear_locked(pid, clear_sources, &clear_source_count,
				   clear_pids, &clear_pid_count);
		delay = ax_bs_next_delay_locked();
		raw_spin_unlock_irqrestore(&ax_bs_lock, flags);
		for (i = 0; i < clear_pid_count; i++) {
			ax_sched_thread_snooper_track(clear_pids[i], false);
#if defined(AX_BS_HAS_GAME_BOOST)
			ax_game_boost_clear(clear_pids[i]);
#endif
		}
		for (i = 0; i < clear_source_count; i++)
			ax_sched_boost_clear(clear_sources[i]);
		ax_bs_schedule_cleanup(delay);
		if (!READ_ONCE(ax_bs_enabled))
			ax_sched_boost_clear(0);
#if defined(AX_BS_HAS_GAME_BOOST)
		if (!READ_ONCE(ax_bs_enabled))
			ax_game_boost_clear(0);
#endif
		ax_bs_io_guard_kick();
		return count;
	}

	if (pid <= 0 || uid < 0)
		return -EINVAL;

	if (mode > AX_BS_MODE_MAX)
		mode = AX_BS_MODE_MAX;
	if (source > AX_BS_SOURCE_MAX)
		source = AX_BS_SOURCE_MAX;
	severity = clamp(severity, AX_BS_SEVERITY_LIGHT, AX_BS_SEVERITY_MAX);
	if (mode == AX_BS_MODE_TOP_APP) {
		sticky = true;
		duration_ms = 0;
		source = AX_BS_SOURCE_TOP_APP;
		severity = AX_BS_SEVERITY_LIGHT;
		role = AX_BS_ROLE_TOP_APP;
	}
#if defined(AX_BS_HAS_GAME_BOOST)
	for (i = 0; i < requested_tids && i + 7 < argc; i++) {
		if (ax_bs_parse_int(argv[i + 7], &parsed) || parsed <= 0)
			continue;
		game_tids[game_tid_count++] = parsed;
	}
#endif

	if (sticky) {
		expire = 0;
	} else {
		duration_ms = min(duration_ms, READ_ONCE(ax_bs_max_duration_ms));
		expire = jiffies + max_t(unsigned long, 1, msecs_to_jiffies(duration_ms));
	}
	pid_ref = find_get_pid(pid);
	if (!pid_ref)
		return -ESRCH;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	target = ax_bs_alloc_target(pid);
	if (!target) {
		raw_spin_unlock_irqrestore(&ax_bs_lock, flags);
		put_pid(pid_ref);
		return -ENOSPC;
	}
	old_resource_source = READ_ONCE(target->resource_source);
	old_pid = ax_bs_set_target_locked(target, pid, uid, mode, source,
					  severity, role, requested_tids, argv,
					  argc, pid_ref, expire, sticky);
	resource_source = READ_ONCE(target->resource_source);
	delay = ax_bs_next_delay_locked();
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);

	ax_bs_release_pid(old_pid);
	ax_bs_schedule_cleanup(delay);
	ax_bs_io_guard_kick();
	ax_sched_thread_snooper_track(pid, true);
	if (old_resource_source)
		ax_sched_boost_clear(old_resource_source);
	ax_sched_boost(resource_source, severity,
				ax_bs_resource_mask(mode, source, severity),
				duration_ms);
#if defined(AX_BS_HAS_GAME_BOOST)
	ax_bs_update_game_boost(pid, mode, source, severity, duration_ms, sticky,
				game_tid_count, game_tids);
#endif
	return count;
}

static int ax_bs_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		struct ax_bs_target *target = &ax_bs_targets[i];
		unsigned long remaining = 0;
		bool active;
		int count;
		int j;

		if (READ_ONCE(target->pid) <= 0)
			continue;

		active = ax_bs_target_active(target);
		if (time_before(jiffies, READ_ONCE(target->expire_jiffies)))
			remaining = jiffies_to_msecs(target->expire_jiffies - jiffies);

		count = min_t(int, READ_ONCE(target->tid_count), AX_BS_MAX_TIDS);
		seq_printf(m, "%d %d %d %d %d %d %lu %d %lld %lld",
			   READ_ONCE(target->pid), READ_ONCE(target->uid),
			   active ? READ_ONCE(target->mode) : 0,
			   active ? READ_ONCE(target->source) : 0,
			   active ? READ_ONCE(target->severity) : 0,
			   active ? READ_ONCE(target->role) : 0,
			   remaining,
			   atomic_read(&target->score),
			   (long long)atomic64_read(&target->wakeups),
			   (long long)atomic64_read(&target->hits));
		for (j = 0; j < count; j++)
			seq_printf(m, " %d", target->tids[j]);
		seq_putc(m, '\n');
	}
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);
	return 0;
}

static int ax_bs_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_bs_show, NULL);
}

static int ax_bs_stats_show(struct seq_file *m, void *v)
{
	int active = 0;
#if defined(AX_BS_HAS_LOCK_BOOST)
	int lock_active = 0;
#endif
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		if (ax_bs_target_active(&ax_bs_targets[i]))
			active++;
	}
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++) {
		if (ax_bs_lock_target_active(&ax_bs_lock_targets[i]))
			lock_active++;
	}
#endif

	seq_printf(m, "active %d\n", active);
#if defined(AX_BS_HAS_LOCK_BOOST)
	seq_printf(m, "lock_active %d\n", lock_active);
#endif
	seq_printf(m, "updates %lld\n",
		   (long long)atomic64_read(&ax_bs_session_updates));
	seq_printf(m, "clears %lld\n",
		   (long long)atomic64_read(&ax_bs_session_clears));
	seq_printf(m, "prunes %lld\n",
		   (long long)atomic64_read(&ax_bs_session_prunes));
	seq_printf(m, "wake_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_wake_assists));
	seq_printf(m, "uclamp_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_uclamp_assists));
	seq_printf(m, "fallback_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_fallback_assists));
	seq_printf(m, "migration_blocks %lld\n",
		   (long long)atomic64_read(&ax_bs_migration_blocks));
	seq_printf(m, "migration_releases %lld\n",
		   (long long)atomic64_read(&ax_bs_migration_releases));
	seq_printf(m, "enqueue_events %lld\n",
		   (long long)atomic64_read(&ax_bs_enqueue_events));
	seq_printf(m, "dequeue_events %lld\n",
		   (long long)atomic64_read(&ax_bs_dequeue_events));
	seq_printf(m, "binder_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_binder_assists));
	seq_printf(m, "reclaim_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_reclaim_assists));
	seq_printf(m, "support_worker_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_support_worker_assists));
	seq_printf(m, "background_guard_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_background_guard_assists));
#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
	seq_printf(m, "dynamic_svp_hook %d\n",
		   ax_bs_dynamic_svp_hook_registered ? 1 : 0);
	seq_printf(m, "dynamic_svp_preempts %lld\n",
		   (long long)atomic64_read(&ax_bs_dynamic_svp_preempts));
#endif
#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
	seq_printf(m, "cpu_capacity_hook %d\n",
		   ax_bs_cpu_capacity_hook_registered ? 1 : 0);
	seq_printf(m, "cpu_capacity_ready %d\n",
		   ax_bs_cpu_topology_ready() ? 1 : 0);
#endif
#if defined(AX_BS_HAS_INPUT_HOOK)
	seq_printf(m, "input_hook %d\n", ax_bs_input_hook_registered ? 1 : 0);
#endif
#if defined(AX_BS_HAS_IOLIMIT_HOOK)
	seq_printf(m, "iolimit_hook %d\n",
		   ax_bs_iolimit_hook_registered ? 1 : 0);
	seq_printf(m, "io_limit_delays %lld\n",
		   (long long)atomic64_read(&ax_bs_io_limit_delays));
#endif
#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
	seq_printf(m, "gpu_context_hook %d\n",
		   ax_bs_gpu_context_hook_registered ? 1 : 0);
	seq_printf(m, "gpu_context_boosts %lld\n",
		   (long long)atomic64_read(&ax_bs_gpu_context_boosts));
#endif
#if defined(AX_BS_HAS_LOCK_BOOST)
	seq_printf(m, "lock_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_assists));
	seq_printf(m, "lock_hits %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_hits));
	seq_printf(m, "lock_wake_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_wake_assists));
	seq_printf(m, "lock_fallback_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_fallback_assists));
	seq_printf(m, "lock_migration_blocks %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_migration_blocks));
	seq_printf(m, "lock_uclamp_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_uclamp_assists));
	seq_printf(m, "lock_prunes %lld\n",
		   (long long)atomic64_read(&ax_bs_lock_prunes));
#endif
#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	seq_printf(m, "freq_assists %lld\n",
		   (long long)atomic64_read(&ax_bs_freq_assists));
#endif
#if defined(AX_BS_HAS_SVP_POLICY)
	seq_printf(m, "svp_wake_assists %lld\n",
		   (long long)atomic64_read(&ax_svp_wake_assists));
	seq_printf(m, "svp_fallback_assists %lld\n",
		   (long long)atomic64_read(&ax_svp_fallback_assists));
	seq_printf(m, "svp_lowest_assists %lld\n",
		   (long long)atomic64_read(&ax_svp_lowest_assists));
	seq_printf(m, "svp_balance_assists %lld\n",
		   (long long)atomic64_read(&ax_svp_balance_assists));
	seq_printf(m, "svp_migration_blocks %lld\n",
		   (long long)atomic64_read(&ax_svp_migration_blocks));
	seq_printf(m, "svp_uclamp_assists %lld\n",
		   (long long)atomic64_read(&ax_svp_uclamp_assists));
#endif
	return 0;
}

static int ax_bs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_bs_stats_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops ax_bs_scene_fops = {
	.proc_open = ax_bs_open,
	.proc_read = seq_read,
	.proc_write = ax_bs_scene_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops ax_bs_stats_fops = {
	.proc_open = ax_bs_stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations ax_bs_scene_fops = {
	.owner = THIS_MODULE,
	.open = ax_bs_open,
	.read = seq_read,
	.write = ax_bs_scene_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations ax_bs_stats_fops = {
	.owner = THIS_MODULE,
	.open = ax_bs_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

#if defined(AX_BS_HAS_SVP_POLICY)
static int ax_svp_create_proc(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry;

	entry = proc_create(AX_SVP_PROC_TASKS, 0660, parent, &ax_svp_tasks_fops);
	if (!entry)
		return -ENOMEM;

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	entry = proc_create(AX_SVP_PROC_THREADS, 0660, parent,
			    &ax_svp_threads_fops);
	if (!entry) {
		remove_proc_entry(AX_SVP_PROC_TASKS, parent);
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return 0;
}

static void ax_svp_remove_proc(struct proc_dir_entry *parent)
{
	remove_proc_entry(AX_SVP_PROC_THREADS, parent);
	remove_proc_entry(AX_SVP_PROC_TASKS, parent);
}
#endif

static int ax_bs_create_proc(void)
{
	struct proc_dir_entry *entry;

	ax_bs_proc_dir = proc_mkdir(AX_BS_PROC_DIR, NULL);
	if (!ax_bs_proc_dir)
		return -ENOMEM;

	entry = proc_create(AX_BS_PROC_SCENE, 0660, ax_bs_proc_dir,
			    &ax_bs_scene_fops);
	if (!entry) {
		remove_proc_entry(AX_BS_PROC_DIR, NULL);
		ax_bs_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	entry = proc_create(AX_BS_PROC_STATS, 0444, ax_bs_proc_dir,
			    &ax_bs_stats_fops);
	if (!entry) {
		remove_proc_entry(AX_BS_PROC_SCENE, ax_bs_proc_dir);
		remove_proc_entry(AX_BS_PROC_DIR, NULL);
		ax_bs_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

#if defined(AX_BS_HAS_SVP_POLICY)
	if (ax_svp_create_proc(ax_bs_proc_dir)) {
		remove_proc_entry(AX_BS_PROC_STATS, ax_bs_proc_dir);
		remove_proc_entry(AX_BS_PROC_SCENE, ax_bs_proc_dir);
		remove_proc_entry(AX_BS_PROC_DIR, NULL);
		ax_bs_proc_dir = NULL;
		return -ENOMEM;
	}
#endif
	return 0;
}

static void ax_bs_remove_proc(void)
{
	if (!ax_bs_proc_dir)
		return;

#if defined(AX_BS_HAS_SVP_POLICY)
	ax_svp_remove_proc(ax_bs_proc_dir);
#endif
	remove_proc_entry(AX_BS_PROC_STATS, ax_bs_proc_dir);
	remove_proc_entry(AX_BS_PROC_SCENE, ax_bs_proc_dir);
	remove_proc_entry(AX_BS_PROC_DIR, NULL);
	ax_bs_proc_dir = NULL;
}

#if defined(AX_BS_HAS_SVP_POLICY)
static void ax_svp_init_state(void)
{
	int i;

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++)
		ax_svp_clear_target(&ax_svp_targets[i]);
	for (i = 0; i < AX_SVP_MAX_THREADS; i++)
		ax_svp_clear_thread_target(&ax_svp_threads[i]);

	INIT_DELAYED_WORK(&ax_svp_cleanup_work, ax_svp_cleanup_work_fn);
}

static void ax_svp_release_all(void)
{
	struct ax_svp_rt_state rt_state[AX_SVP_MAX_THREADS];
	struct pid *thread_pid[AX_SVP_MAX_THREADS];
	struct pid *target_pid[AX_SVP_MAX_TARGETS];
	unsigned long flags;
	int i;

	cancel_delayed_work_sync(&ax_svp_cleanup_work);

	for (i = 0; i < AX_SVP_MAX_TARGETS; i++)
		target_pid[i] = NULL;
	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		thread_pid[i] = NULL;
		ax_svp_init_rt_state(&rt_state[i]);
	}

	raw_spin_lock_irqsave(&ax_svp_lock, flags);
	for (i = 0; i < AX_SVP_MAX_TARGETS; i++)
		target_pid[i] = ax_svp_detach_target(&ax_svp_targets[i]);
	for (i = 0; i < AX_SVP_MAX_THREADS; i++)
		thread_pid[i] = ax_svp_detach_thread_target(&ax_svp_threads[i],
							    &rt_state[i]);
	raw_spin_unlock_irqrestore(&ax_svp_lock, flags);

	for (i = 0; i < AX_SVP_MAX_THREADS; i++) {
		ax_svp_restore_rt(&rt_state[i]);
		ax_bs_release_pid(rt_state[i].pid);
		ax_bs_release_pid(thread_pid[i]);
	}
	for (i = 0; i < AX_SVP_MAX_TARGETS; i++)
		ax_bs_release_pid(target_pid[i]);
}
#endif

static void ax_bs_register_optional_hooks(void)
{
#if defined(AX_BS_HAS_INPUT_HOOK)
	if (!register_trace_android_vh_input_sync(ax_bs_input_sync, NULL))
		ax_bs_input_hook_registered = true;
#endif
#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
	if (!register_trace_android_rvh_update_cpu_capacity(ax_bs_update_cpu_capacity,
							    NULL))
		ax_bs_cpu_capacity_hook_registered = true;
#endif
#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
	if (!register_trace_android_vh_dynamic_svp_preempt(ax_bs_dynamic_svp_preempt,
							   NULL))
		ax_bs_dynamic_svp_hook_registered = true;
#endif
#if defined(AX_BS_HAS_IOLIMIT_HOOK)
	if (READ_ONCE(ax_bs_io_limit_delay_ms) &&
	    !register_trace_android_vh_iolimit_rw(ax_bs_iolimit_rw, NULL))
		ax_bs_iolimit_hook_registered = true;
#endif
#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
	if (!register_trace_android_vh_gpu_context_create(ax_bs_gpu_context_create,
							  NULL))
		ax_bs_gpu_context_hook_registered = true;
#endif
}

static void ax_bs_unregister_optional_hooks(void)
{
#if defined(AX_BS_HAS_INPUT_HOOK)
	if (ax_bs_input_hook_registered) {
		unsigned long flags;

		ax_sched_unregister_hook(android_vh_input_sync, ax_bs_input_sync,
					 NULL);
		raw_spin_lock_irqsave(&ax_bs_auto_lock, flags);
		write_seqcount_begin(&ax_bs_auto_seq);
		WRITE_ONCE(ax_bs_input_hook_registered, false);
		WRITE_ONCE(ax_bs_auto_state.input_until, 0);
		WRITE_ONCE(ax_bs_auto_state.owner_until, 0);
		WRITE_ONCE(ax_bs_auto_state.max_until, 0);
		WRITE_ONCE(ax_bs_auto_state.tgid, -1);
		WRITE_ONCE(ax_bs_auto_state.cpu, AX_SCHED_CPU_NONE);
		WRITE_ONCE(ax_bs_auto_state.systemui, false);
		write_seqcount_end(&ax_bs_auto_seq);
		raw_spin_unlock_irqrestore(&ax_bs_auto_lock, flags);
	}
#endif
#if defined(AX_BS_HAS_CPU_CAPACITY_HOOK)
	if (ax_bs_cpu_capacity_hook_registered) {
		ax_sched_unregister_hook(android_rvh_update_cpu_capacity,
					 ax_bs_update_cpu_capacity, NULL);
		WRITE_ONCE(ax_bs_cpu_capacity_hook_registered, false);
		WRITE_ONCE(ax_bs_cpu_capacity_ready, false);
	}
#endif
#if defined(AX_BS_HAS_DYNAMIC_SVP_HOOK)
	if (ax_bs_dynamic_svp_hook_registered) {
		ax_sched_unregister_hook(android_vh_dynamic_svp_preempt,
					 ax_bs_dynamic_svp_preempt, NULL);
		ax_bs_dynamic_svp_hook_registered = false;
	}
#endif
#if defined(AX_BS_HAS_IOLIMIT_HOOK)
	if (ax_bs_iolimit_hook_registered) {
		ax_sched_unregister_hook(android_vh_iolimit_rw,
					 ax_bs_iolimit_rw, NULL);
		ax_bs_iolimit_hook_registered = false;
	}
#endif
#if defined(AX_BS_HAS_GPU_CONTEXT_HOOK)
	if (ax_bs_gpu_context_hook_registered) {
		ax_sched_unregister_hook(android_vh_gpu_context_create,
					 ax_bs_gpu_context_create, NULL);
		ax_bs_gpu_context_hook_registered = false;
	}
#endif
}

static int __init ax_burst_sched_init(void)
{
	int ret;
	int i;

	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		seqcount_init(&ax_bs_targets[i].cpu_seq);
		WRITE_ONCE(ax_bs_targets[i].pid, -1);
		WRITE_ONCE(ax_bs_targets[i].cpu, AX_SCHED_CPU_NONE);
	}
	ax_bs_init_cpu_scores();
	INIT_DELAYED_WORK(&ax_bs_cleanup_work, ax_bs_cleanup_work_fn);
	INIT_DELAYED_WORK(&ax_bs_io_guard_work, ax_bs_io_guard_work_fn);
#if defined(AX_BS_HAS_SVP_POLICY)
	ax_svp_init_state();
#endif

	ret = ax_bs_create_proc();
	if (ret)
		return ret;

	ax_bs_register_optional_hooks();

#if defined(AX_BS_HAS_LOCK_BOOST)
	ret = register_trace_android_vh_rwsem_wake(ax_bs_rwsem_wake, NULL);
	if (ret) {
		ax_bs_unregister_optional_hooks();
		tracepoint_synchronize_unregister();
		ax_bs_remove_proc();
		return ret;
	}

	ret = register_trace_android_vh_rwsem_write_finished(
			ax_bs_rwsem_write_finished, NULL);
	if (ret)
		goto unregister_rwsem_wake;

#if defined(AX_BS_HAS_MUTEX_HOOK)
	ret = register_trace_android_vh_mutex_wait_start(ax_bs_mutex_wait_start,
							 NULL);
	if (ret)
		goto unregister_rwsem_write_finished;

	ret = register_trace_android_vh_mutex_wait_finish(ax_bs_mutex_wait_finish,
							  NULL);
	if (ret)
		goto unregister_mutex_wait_start;
#endif
#endif

#if defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
	ret = register_trace_android_rvh_uclamp_eff_get(ax_bs_uclamp_eff_get,
						       NULL);
	if (ret) {
#if defined(AX_BS_HAS_LOCK_BOOST)
#if defined(AX_BS_HAS_MUTEX_HOOK)
		goto unregister_mutex_wait_finish;
#else
		goto unregister_rwsem_write_finished;
#endif
#else
		ax_bs_unregister_optional_hooks();
		tracepoint_synchronize_unregister();
		ax_bs_remove_proc();
		return ret;
#endif
	}
#endif

#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	ret = register_trace_android_vh_map_util_freq(ax_bs_map_util_freq, NULL);
	if (ret)
		goto unregister_uclamp;
#endif

	ret = register_trace_android_rvh_select_task_rq_fair(ax_bs_select_task_rq,
							    NULL);
	if (ret)
		goto unregister_map_util;

	ret = register_trace_android_rvh_select_task_rq_rt(ax_bs_select_task_rq,
							  NULL);
	if (ret)
		goto unregister_select_fair;

	ret = register_trace_android_rvh_select_fallback_rq(
			ax_bs_select_fallback_rq, NULL);
	if (ret)
		goto unregister_select_rt;

#if defined(AX_BS_HAS_FRAME_BOOST) || defined(AX_BS_HAS_SVP_POLICY) || defined(AX_BS_HAS_GAME_BOOST)
	ret = register_trace_android_rvh_find_lowest_rq(ax_bs_find_lowest_rq,
						       NULL);
	if (ret)
		goto unregister_select_fallback;
#endif

	ret = register_trace_android_rvh_enqueue_task(ax_bs_enqueue_task, NULL);
	if (ret)
		goto unregister_find_lowest;

	ret = register_trace_android_rvh_dequeue_task(ax_bs_dequeue_task, NULL);
	if (ret)
		goto unregister_enqueue;

	ret = register_trace_android_rvh_can_migrate_task(
			ax_bs_can_migrate_task, NULL);
	if (ret)
		goto unregister_dequeue;

	return 0;

unregister_dequeue:
	ax_sched_unregister_hook(android_rvh_dequeue_task, ax_bs_dequeue_task, NULL);
unregister_enqueue:
	ax_sched_unregister_hook(android_rvh_enqueue_task, ax_bs_enqueue_task, NULL);
unregister_find_lowest:
#if defined(AX_BS_HAS_FRAME_BOOST) || defined(AX_BS_HAS_SVP_POLICY) || defined(AX_BS_HAS_GAME_BOOST)
	ax_sched_unregister_hook(android_rvh_find_lowest_rq,
				 ax_bs_find_lowest_rq, NULL);
#endif
unregister_select_fallback:
	ax_sched_unregister_hook(android_rvh_select_fallback_rq,
				 ax_bs_select_fallback_rq, NULL);
unregister_select_rt:
	ax_sched_unregister_hook(android_rvh_select_task_rq_rt,
				 ax_bs_select_task_rq, NULL);
unregister_select_fair:
	ax_sched_unregister_hook(android_rvh_select_task_rq_fair,
				 ax_bs_select_task_rq, NULL);
unregister_map_util:
#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	ax_sched_unregister_hook(android_vh_map_util_freq, ax_bs_map_util_freq, NULL);
#endif
unregister_uclamp:
#if defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
	ax_sched_unregister_hook(android_rvh_uclamp_eff_get,
				 ax_bs_uclamp_eff_get, NULL);
#endif
#if defined(AX_BS_HAS_LOCK_BOOST) && defined(AX_BS_HAS_MUTEX_HOOK) && defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
unregister_mutex_wait_finish:
#endif
#if defined(AX_BS_HAS_LOCK_BOOST) && defined(AX_BS_HAS_MUTEX_HOOK)
	ax_sched_unregister_hook(android_vh_mutex_wait_finish,
				 ax_bs_mutex_wait_finish, NULL);
unregister_mutex_wait_start:
	ax_sched_unregister_hook(android_vh_mutex_wait_start,
				 ax_bs_mutex_wait_start, NULL);
#endif
#if defined(AX_BS_HAS_LOCK_BOOST) && (defined(AX_BS_HAS_MUTEX_HOOK) || (defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)))
unregister_rwsem_write_finished:
#endif
#if defined(AX_BS_HAS_LOCK_BOOST)
	ax_sched_unregister_hook(android_vh_rwsem_write_finished,
				 ax_bs_rwsem_write_finished, NULL);
unregister_rwsem_wake:
	ax_sched_unregister_hook(android_vh_rwsem_wake, ax_bs_rwsem_wake, NULL);
#endif
	ax_bs_unregister_optional_hooks();
	tracepoint_synchronize_unregister();
	ax_bs_remove_proc();
	return ret;
}

module_init(ax_burst_sched_init);

static void __exit ax_burst_sched_exit(void)
{
	struct pid *pids[AX_BS_MAX_TARGETS];
#if defined(AX_BS_HAS_LOCK_BOOST)
	struct pid *lock_pids[AX_BS_MAX_LOCK_TARGETS];
#endif
	unsigned int clear_sources[AX_BS_MAX_TARGETS];
	pid_t clear_pids[AX_BS_MAX_TARGETS];
	unsigned long flags;
	int clear_source_count = 0;
	int clear_pid_count = 0;
	int i;

	cancel_delayed_work_sync(&ax_bs_cleanup_work);
	ax_bs_release_io_guards();
	ax_bs_unregister_optional_hooks();
	ax_sched_unregister_hook(android_rvh_can_migrate_task,
				 ax_bs_can_migrate_task, NULL);
	ax_sched_unregister_hook(android_rvh_dequeue_task, ax_bs_dequeue_task, NULL);
	ax_sched_unregister_hook(android_rvh_enqueue_task, ax_bs_enqueue_task, NULL);
#if defined(AX_BS_HAS_FRAME_BOOST) || defined(AX_BS_HAS_SVP_POLICY) || defined(AX_BS_HAS_GAME_BOOST)
	ax_sched_unregister_hook(android_rvh_find_lowest_rq,
				 ax_bs_find_lowest_rq, NULL);
#endif
	ax_sched_unregister_hook(android_rvh_select_fallback_rq,
				 ax_bs_select_fallback_rq, NULL);
	ax_sched_unregister_hook(android_rvh_select_task_rq_rt,
				 ax_bs_select_task_rq, NULL);
	ax_sched_unregister_hook(android_rvh_select_task_rq_fair,
				 ax_bs_select_task_rq, NULL);
#if defined(AX_BS_HAS_MAP_UTIL_FREQ)
	ax_sched_unregister_hook(android_vh_map_util_freq, ax_bs_map_util_freq, NULL);
#endif
#if defined(AX_BS_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
	ax_sched_unregister_hook(android_rvh_uclamp_eff_get,
				 ax_bs_uclamp_eff_get, NULL);
#endif
#if defined(AX_BS_HAS_LOCK_BOOST)
#if defined(AX_BS_HAS_MUTEX_HOOK)
	ax_sched_unregister_hook(android_vh_mutex_wait_finish,
				 ax_bs_mutex_wait_finish, NULL);
	ax_sched_unregister_hook(android_vh_mutex_wait_start,
				 ax_bs_mutex_wait_start, NULL);
#endif
	ax_sched_unregister_hook(android_vh_rwsem_write_finished,
				 ax_bs_rwsem_write_finished, NULL);
	ax_sched_unregister_hook(android_vh_rwsem_wake, ax_bs_rwsem_wake, NULL);
#endif
	tracepoint_synchronize_unregister();

	for (i = 0; i < AX_BS_MAX_TARGETS; i++)
		pids[i] = NULL;
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++)
		lock_pids[i] = NULL;
#endif
	for (i = 0; i < AX_BS_MAX_TARGETS; i++)
		clear_sources[i] = 0;
	for (i = 0; i < AX_BS_MAX_TARGETS; i++)
		clear_pids[i] = 0;

	raw_spin_lock_irqsave(&ax_bs_lock, flags);
	for (i = 0; i < AX_BS_MAX_TARGETS; i++) {
		ax_bs_add_clear_source(clear_sources, &clear_source_count,
				       READ_ONCE(ax_bs_targets[i].resource_source));
		ax_bs_add_clear_pid(clear_pids, &clear_pid_count,
				    READ_ONCE(ax_bs_targets[i].pid));
		pids[i] = ax_bs_detach_target(&ax_bs_targets[i]);
	}
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++)
		lock_pids[i] = ax_bs_detach_lock_target(&ax_bs_lock_targets[i]);
#endif
	raw_spin_unlock_irqrestore(&ax_bs_lock, flags);

	for (i = 0; i < AX_BS_MAX_TARGETS; i++)
		ax_bs_release_pid(pids[i]);
#if defined(AX_BS_HAS_LOCK_BOOST)
	for (i = 0; i < AX_BS_MAX_LOCK_TARGETS; i++)
		ax_bs_release_pid(lock_pids[i]);
#endif
	for (i = 0; i < clear_pid_count; i++) {
		ax_sched_thread_snooper_track(clear_pids[i], false);
#if defined(AX_BS_HAS_GAME_BOOST)
		ax_game_boost_clear(clear_pids[i]);
#endif
	}
	for (i = 0; i < clear_source_count; i++)
		ax_sched_boost_clear(clear_sources[i]);

#if defined(AX_BS_HAS_SVP_POLICY)
	ax_svp_release_all();
#endif
	ax_bs_remove_proc();
}

module_exit(ax_burst_sched_exit);

MODULE_LICENSE("GPL");
