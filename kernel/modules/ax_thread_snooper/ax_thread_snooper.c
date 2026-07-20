// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <ax_sched_common.h>
#include <trace/events/task.h>

#define AX_TS_MAX_TARGETS 64
#define AX_TS_MAX_TASKS 256
#define AX_TS_CMD_SIZE 64
#define AX_TS_PROC_DIR "ax_thread_snooper"
#define AX_TS_PROC_TARGETS "targets"
#define AX_TS_PROC_TASKS "tasks"

struct ax_ts_target {
	pid_t pid;
	struct pid *pid_ref;
};

struct ax_ts_task {
	pid_t pid;
	pid_t tid;
	char comm[TASK_COMM_LEN];
	struct pid *pid_ref;
};

static DEFINE_RAW_SPINLOCK(ax_ts_lock);
static struct proc_dir_entry *ax_ts_proc_dir;
static struct ax_ts_target ax_ts_targets[AX_TS_MAX_TARGETS];
static struct ax_ts_task ax_ts_tasks[AX_TS_MAX_TASKS];

static bool ax_ts_pid_alive(struct pid *pid)
{
	bool alive;

	rcu_read_lock();
	alive = pid_task(pid, PIDTYPE_PID) != NULL;
	rcu_read_unlock();

	return alive;
}

static void ax_ts_release_pid(struct pid *pid)
{
	if (pid)
		put_pid(pid);
}

static struct pid *ax_ts_detach_target(struct ax_ts_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	WRITE_ONCE(target->pid, -1);
	smp_wmb();
	WRITE_ONCE(target->pid_ref, NULL);

	return pid;
}

static struct pid *ax_ts_detach_task(struct ax_ts_task *task)
{
	struct pid *pid = READ_ONCE(task->pid_ref);

	WRITE_ONCE(task->pid, -1);
	WRITE_ONCE(task->tid, -1);
	task->comm[0] = '\0';
	smp_wmb();
	WRITE_ONCE(task->pid_ref, NULL);

	return pid;
}

static void ax_ts_clear_target(struct ax_ts_target *target)
{
	ax_ts_release_pid(ax_ts_detach_target(target));
}

static void ax_ts_clear_task(struct ax_ts_task *task)
{
	ax_ts_release_pid(ax_ts_detach_task(task));
}

static bool ax_ts_target_alive(struct ax_ts_target *target)
{
	struct pid *pid = READ_ONCE(target->pid_ref);

	return READ_ONCE(target->pid) > 0 && pid && ax_ts_pid_alive(pid);
}

static bool ax_ts_task_alive(struct ax_ts_task *task)
{
	struct pid *pid = READ_ONCE(task->pid_ref);

	return READ_ONCE(task->tid) > 0 && pid && ax_ts_pid_alive(pid);
}

static bool ax_ts_comm_matches(const char *comm)
{
	return ax_sched_comm_matches_render_helper(comm);
}

static bool ax_ts_target_tracked_locked(pid_t pid)
{
	int i;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		struct ax_ts_target *target = &ax_ts_targets[i];

		if (READ_ONCE(target->pid) == pid && ax_ts_target_alive(target))
			return true;
	}

	return false;
}

static struct ax_ts_target *ax_ts_find_target_locked(pid_t pid)
{
	int i;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		if (READ_ONCE(ax_ts_targets[i].pid) == pid)
			return &ax_ts_targets[i];
	}

	return NULL;
}

static struct ax_ts_target *ax_ts_alloc_target_locked(pid_t pid)
{
	struct ax_ts_target *target;
	int i;

	target = ax_ts_find_target_locked(pid);
	if (target)
		return target;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		target = &ax_ts_targets[i];
		if (READ_ONCE(target->pid) <= 0 || !ax_ts_target_alive(target))
			return target;
	}

	return NULL;
}

static struct ax_ts_task *ax_ts_find_task_locked(pid_t tid)
{
	int i;

	for (i = 0; i < AX_TS_MAX_TASKS; i++) {
		if (READ_ONCE(ax_ts_tasks[i].tid) == tid)
			return &ax_ts_tasks[i];
	}

	return NULL;
}

static struct ax_ts_task *ax_ts_alloc_task_locked(pid_t tid)
{
	struct ax_ts_task *task;
	int i;

	task = ax_ts_find_task_locked(tid);
	if (task)
		return task;

	for (i = 0; i < AX_TS_MAX_TASKS; i++) {
		task = &ax_ts_tasks[i];
		if (READ_ONCE(task->tid) <= 0 || !ax_ts_task_alive(task))
			return task;
	}

	return NULL;
}

static void ax_ts_clear_pid_locked(pid_t pid)
{
	int i;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		struct ax_ts_target *target = &ax_ts_targets[i];

		if (pid > 0 && READ_ONCE(target->pid) != pid)
			continue;

		ax_ts_clear_target(target);
	}

	for (i = 0; i < AX_TS_MAX_TASKS; i++) {
		struct ax_ts_task *task = &ax_ts_tasks[i];

		if (pid > 0 && READ_ONCE(task->pid) != pid)
			continue;

		ax_ts_clear_task(task);
	}
}

static void ax_ts_prune_locked(void)
{
	int i;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		struct ax_ts_target *target = &ax_ts_targets[i];

		if (READ_ONCE(target->pid) > 0 && !ax_ts_target_alive(target))
			ax_ts_clear_target(target);
	}

	for (i = 0; i < AX_TS_MAX_TASKS; i++) {
		struct ax_ts_task *task = &ax_ts_tasks[i];

		if (READ_ONCE(task->tid) > 0 && !ax_ts_task_alive(task))
			ax_ts_clear_task(task);
	}
}

static struct pid *ax_ts_set_target_locked(struct ax_ts_target *target,
						   pid_t pid, struct pid *pid_ref)
{
	struct pid *old_pid = ax_ts_detach_target(target);

	WRITE_ONCE(target->pid_ref, pid_ref);
	smp_wmb();
	WRITE_ONCE(target->pid, pid);

	return old_pid;
}

static struct pid *ax_ts_set_task_locked(struct ax_ts_task *task, pid_t pid,
						 pid_t tid, const char *comm,
						 struct pid *pid_ref)
{
	struct pid *old_pid = ax_ts_detach_task(task);

	strscpy(task->comm, comm, sizeof(task->comm));
	WRITE_ONCE(task->pid, pid);
	WRITE_ONCE(task->pid_ref, pid_ref);
	smp_wmb();
	WRITE_ONCE(task->tid, tid);

	return old_pid;
}

static void ax_ts_update_task(pid_t pid, pid_t tid, const char *comm)
{
	struct ax_ts_task *task;
	struct pid *pid_ref = NULL;
	struct pid *old_pid = NULL;
	unsigned long flags;
	bool matched;

	if (pid <= 0 || tid <= 0)
		return;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	matched = ax_ts_target_tracked_locked(pid) && ax_ts_comm_matches(comm);
	if (!matched) {
		task = ax_ts_find_task_locked(tid);
		if (task)
			old_pid = ax_ts_detach_task(task);
		raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
		ax_ts_release_pid(old_pid);
		return;
	}
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);

	pid_ref = find_get_pid(tid);
	if (!pid_ref)
		return;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	if (!ax_ts_target_tracked_locked(pid)) {
		raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
		put_pid(pid_ref);
		return;
	}
	task = ax_ts_alloc_task_locked(tid);
	if (task)
		old_pid = ax_ts_set_task_locked(task, pid, tid, comm, pid_ref);
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);

	if (!task)
		put_pid(pid_ref);
	ax_ts_release_pid(old_pid);
}

static void ax_ts_prime_target(pid_t pid)
{
	struct task_struct *leader;
	struct task_struct *thread;

	rcu_read_lock();
	leader = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!leader) {
		rcu_read_unlock();
		return;
	}

	for_each_thread(leader, thread) {
		char comm[TASK_COMM_LEN];

		get_task_comm(comm, thread);
		ax_ts_update_task(task_tgid_nr(thread), task_pid_nr(thread), comm);
	}
	rcu_read_unlock();
}

static void ax_ts_task_newtask(void *unused, struct task_struct *task,
			       unsigned long clone_flags)
{
	char comm[TASK_COMM_LEN];

	if (!task)
		return;

	get_task_comm(comm, task);
	ax_ts_update_task(task_tgid_nr(task), task_pid_nr(task), comm);
}

static void ax_ts_task_rename(void *unused, struct task_struct *task,
			      const char *comm)
{
	if (!task || !comm)
		return;

	ax_ts_update_task(task_tgid_nr(task), task_pid_nr(task), comm);
}

static int ax_ts_parse_int(char *text, int *value)
{
	return kstrtoint(strstrip(text), 10, value);
}

void ax_thread_snooper_track(pid_t pid, bool enabled)
{
	struct ax_ts_target *target;
	struct pid *pid_ref = NULL;
	struct pid *old_pid = NULL;
	unsigned long flags;

	if (!enabled) {
		raw_spin_lock_irqsave(&ax_ts_lock, flags);
		ax_ts_clear_pid_locked(pid);
		raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
		return;
	}

	if (pid <= 0)
		return;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	if (ax_ts_target_tracked_locked(pid)) {
		raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
		return;
	}
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);

	pid_ref = find_get_pid(pid);
	if (!pid_ref)
		return;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	target = ax_ts_alloc_target_locked(pid);
	if (!target) {
		raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
		put_pid(pid_ref);
		return;
	}
	old_pid = ax_ts_set_target_locked(target, pid, pid_ref);
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);

	ax_ts_release_pid(old_pid);
	ax_ts_prime_target(pid);
}
EXPORT_SYMBOL_GPL(ax_thread_snooper_track);

static ssize_t ax_ts_targets_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	char buffer[AX_TS_CMD_SIZE];
	char *argv[2];
	char *cursor;
	char *token;
	int argc = 0;
	int pid;
	int enabled;

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

	if (ax_ts_parse_int(argv[0], &pid) || ax_ts_parse_int(argv[1], &enabled))
		return -EINVAL;

	ax_thread_snooper_track(pid, enabled > 0);
	return count;
}

static int ax_ts_targets_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	ax_ts_prune_locked();
	for (i = 0; i < AX_TS_MAX_TARGETS; i++) {
		struct ax_ts_target *target = &ax_ts_targets[i];

		if (READ_ONCE(target->pid) > 0)
			seq_printf(m, "%d\n", target->pid);
	}
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
	return 0;
}

static int ax_ts_tasks_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	ax_ts_prune_locked();
	for (i = 0; i < AX_TS_MAX_TASKS; i++) {
		struct ax_ts_task *task = &ax_ts_tasks[i];

		if (READ_ONCE(task->tid) > 0)
			seq_printf(m, "%d %d %s\n", task->pid, task->tid, task->comm);
	}
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
	return 0;
}

static int ax_ts_targets_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_ts_targets_show, NULL);
}

static int ax_ts_tasks_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_ts_tasks_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops ax_ts_targets_fops = {
	.proc_open = ax_ts_targets_open,
	.proc_read = seq_read,
	.proc_write = ax_ts_targets_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops ax_ts_tasks_fops = {
	.proc_open = ax_ts_tasks_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations ax_ts_targets_fops = {
	.owner = THIS_MODULE,
	.open = ax_ts_targets_open,
	.read = seq_read,
	.write = ax_ts_targets_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations ax_ts_tasks_fops = {
	.owner = THIS_MODULE,
	.open = ax_ts_tasks_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int ax_ts_create_proc(void)
{
	struct proc_dir_entry *entry;

	ax_ts_proc_dir = proc_mkdir(AX_TS_PROC_DIR, NULL);
	if (!ax_ts_proc_dir)
		return -ENOMEM;

	entry = proc_create(AX_TS_PROC_TARGETS, 0660, ax_ts_proc_dir,
			    &ax_ts_targets_fops);
	if (!entry) {
		remove_proc_entry(AX_TS_PROC_DIR, NULL);
		ax_ts_proc_dir = NULL;
		return -ENOMEM;
	}
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	entry = proc_create(AX_TS_PROC_TASKS, 0440, ax_ts_proc_dir,
			    &ax_ts_tasks_fops);
	if (!entry) {
		remove_proc_entry(AX_TS_PROC_TARGETS, ax_ts_proc_dir);
		remove_proc_entry(AX_TS_PROC_DIR, NULL);
		ax_ts_proc_dir = NULL;
		return -ENOMEM;
	}
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	return 0;
}

static void ax_ts_remove_proc(void)
{
	if (!ax_ts_proc_dir)
		return;

	remove_proc_entry(AX_TS_PROC_TARGETS, ax_ts_proc_dir);
	remove_proc_entry(AX_TS_PROC_TASKS, ax_ts_proc_dir);
	remove_proc_entry(AX_TS_PROC_DIR, NULL);
	ax_ts_proc_dir = NULL;
}

static int __init ax_thread_snooper_init(void)
{
	int ret;
	int i;

	for (i = 0; i < AX_TS_MAX_TARGETS; i++)
		ax_ts_clear_target(&ax_ts_targets[i]);
	for (i = 0; i < AX_TS_MAX_TASKS; i++)
		ax_ts_clear_task(&ax_ts_tasks[i]);

	ret = ax_ts_create_proc();
	if (ret)
		return ret;

	ret = register_trace_task_newtask(ax_ts_task_newtask, NULL);
	if (ret)
		goto remove_proc;

	ret = register_trace_task_rename(ax_ts_task_rename, NULL);
	if (ret)
		goto unregister_newtask;

	return 0;

unregister_newtask:
	unregister_trace_task_newtask(ax_ts_task_newtask, NULL);
remove_proc:
	ax_ts_remove_proc();
	return ret;
}

module_init(ax_thread_snooper_init);

static void __exit ax_thread_snooper_exit(void)
{
	unsigned long flags;

	unregister_trace_task_rename(ax_ts_task_rename, NULL);
	unregister_trace_task_newtask(ax_ts_task_newtask, NULL);
	raw_spin_lock_irqsave(&ax_ts_lock, flags);
	ax_ts_clear_pid_locked(0);
	raw_spin_unlock_irqrestore(&ax_ts_lock, flags);
	ax_ts_remove_proc();
}

module_exit(ax_thread_snooper_exit);

MODULE_LICENSE("GPL");
