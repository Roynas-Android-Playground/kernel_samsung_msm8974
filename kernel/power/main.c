/*
 * kernel/power/main.c - PM subsystem core functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/export.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/resume-trace.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hrtimer.h>

#define CONFIG_SUSPEND_HELPER //etinum.test
//#define SUSPEND_WAKEUP_BOOST

#ifdef SUSPEND_WAKEUP_BOOST
#include <linux/sched.h>
#endif

#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
#include <linux/cpufreq.h>
#include <linux/cpufreq_limit.h>
#endif
#ifdef CONFIG_SEC_DVFS
#include <linux/cpufreq.h>
#include <linux/rq_stats.h>
#endif
#include "power.h"

#define MAX_BUF 100

DEFINE_MUTEX(pm_mutex);

#ifdef CONFIG_PM_SLEEP

/* Routines for PM-transition notifications */

static BLOCKING_NOTIFIER_HEAD(pm_chain_head);

static void touch_event_fn(struct work_struct *work);
static DECLARE_WORK(touch_event_struct, touch_event_fn);

static struct hrtimer tc_ev_timer;
static int tc_ev_processed;
static ktime_t touch_evt_timer_val;

int register_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_pm_notifier);

int unregister_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_pm_notifier);

int pm_notifier_call_chain(unsigned long val)
{
	int ret = blocking_notifier_call_chain(&pm_chain_head, val, NULL);

	return notifier_to_errno(ret);
}

/* If set, devices may be suspended and resumed asynchronously. */
int pm_async_enabled = 1;

static ssize_t pm_async_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_async_enabled);
}

static ssize_t pm_async_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	pm_async_enabled = val;
	return n;
}

power_attr(pm_async);

static void touch_event_fn(struct work_struct *work)
{
	/* wakeup the userspace poll */
	tc_ev_processed = 1;
	sysfs_notify(power_kobj, NULL, "touch_event");

	return;
}

static enum hrtimer_restart tc_ev_stop(struct hrtimer *hrtimer)
{

	schedule_work(&touch_event_struct);

	return HRTIMER_NORESTART;
}

#ifdef CONFIG_PM_DEBUG
int pm_test_level = TEST_NONE;

static const char * const pm_tests[__TEST_AFTER_LAST] = {
	[TEST_NONE] = "none",
	[TEST_CORE] = "core",
	[TEST_CPUS] = "processors",
	[TEST_PLATFORM] = "platform",
	[TEST_DEVICES] = "devices",
	[TEST_FREEZER] = "freezer",
};

static ssize_t pm_test_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	char *s = buf;
	int level;

	for (level = TEST_FIRST; level <= TEST_MAX; level++)
		if (pm_tests[level]) {
			if (level == pm_test_level)
				s += sprintf(s, "[%s] ", pm_tests[level]);
			else
				s += sprintf(s, "%s ", pm_tests[level]);
		}

	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';

	return (s - buf);
}

static ssize_t pm_test_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	const char * const *s;
	int level;
	char *p;
	int len;
	int error = -EINVAL;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	lock_system_sleep();

	level = TEST_FIRST;
	for (s = &pm_tests[level]; level <= TEST_MAX; s++, level++)
		if (*s && len == strlen(*s) && !strncmp(buf, *s, len)) {
			pm_test_level = level;
			error = 0;
			break;
		}

	unlock_system_sleep();

	return error ? error : n;
}

power_attr(pm_test);
#endif /* CONFIG_PM_DEBUG */

#ifdef CONFIG_DEBUG_FS
static char *suspend_step_name(enum suspend_stat_step step)
{
	switch (step) {
	case SUSPEND_FREEZE:
		return "freeze";
	case SUSPEND_PREPARE:
		return "prepare";
	case SUSPEND_SUSPEND:
		return "suspend";
	case SUSPEND_SUSPEND_NOIRQ:
		return "suspend_noirq";
	case SUSPEND_RESUME_NOIRQ:
		return "resume_noirq";
	case SUSPEND_RESUME:
		return "resume";
	default:
		return "";
	}
}

static int suspend_stats_show(struct seq_file *s, void *unused)
{
	int i, index, last_dev, last_errno, last_step;

	last_dev = suspend_stats.last_failed_dev + REC_FAILED_NUM - 1;
	last_dev %= REC_FAILED_NUM;
	last_errno = suspend_stats.last_failed_errno + REC_FAILED_NUM - 1;
	last_errno %= REC_FAILED_NUM;
	last_step = suspend_stats.last_failed_step + REC_FAILED_NUM - 1;
	last_step %= REC_FAILED_NUM;
	seq_printf(s, "%s: %d\n%s: %d\n%s: %d\n%s: %d\n%s: %d\n"
			"%s: %d\n%s: %d\n%s: %d\n%s: %d\n%s: %d\n",
			"success", suspend_stats.success,
			"fail", suspend_stats.fail,
			"failed_freeze", suspend_stats.failed_freeze,
			"failed_prepare", suspend_stats.failed_prepare,
			"failed_suspend", suspend_stats.failed_suspend,
			"failed_suspend_late",
				suspend_stats.failed_suspend_late,
			"failed_suspend_noirq",
				suspend_stats.failed_suspend_noirq,
			"failed_resume", suspend_stats.failed_resume,
			"failed_resume_early",
				suspend_stats.failed_resume_early,
			"failed_resume_noirq",
				suspend_stats.failed_resume_noirq);
	seq_printf(s,	"failures:\n  last_failed_dev:\t%-s\n",
			suspend_stats.failed_devs[last_dev]);
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_dev + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-s\n",
			suspend_stats.failed_devs[index]);
	}
	seq_printf(s,	"  last_failed_errno:\t%-d\n",
			suspend_stats.errno[last_errno]);
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_errno + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-d\n",
			suspend_stats.errno[index]);
	}
	seq_printf(s,	"  last_failed_step:\t%-s\n",
			suspend_step_name(
				suspend_stats.failed_steps[last_step]));
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_step + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-s\n",
			suspend_step_name(
				suspend_stats.failed_steps[index]));
	}

	return 0;
}

static int suspend_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, suspend_stats_show, NULL);
}

static const struct file_operations suspend_stats_operations = {
	.open           = suspend_stats_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int __init pm_debugfs_init(void)
{
	debugfs_create_file("suspend_stats", S_IFREG | S_IRUGO,
			NULL, NULL, &suspend_stats_operations);
	return 0;
}

late_initcall(pm_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

#endif /* CONFIG_PM_SLEEP */

struct kobject *power_kobj;

/**
 *	state - control system power state.
 *
 *	show() returns what states are supported, which is hard-coded to
 *	'standby' (Power-On Suspend), 'mem' (Suspend-to-RAM), and
 *	'disk' (Suspend-to-Disk).
 *
 *	store() accepts one of those strings, translates it into the
 *	proper enumerated value, and initiates a suspend transition.
 */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	char *s = buf;
#ifdef CONFIG_SUSPEND
	suspend_state_t i;

	for (i = PM_SUSPEND_MIN; i < PM_SUSPEND_MAX; i++)
		if (pm_states[i].state)
			s += sprintf(s,"%s ", pm_states[i].label);

#endif
#ifdef CONFIG_HIBERNATION
	s += sprintf(s, "%s\n", "disk");
#else
	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';
#endif
	return (s - buf);
}

static suspend_state_t decode_state(const char *buf, size_t n)
{
#ifdef CONFIG_SUSPEND
	suspend_state_t state = PM_SUSPEND_MIN;
	struct pm_sleep_state *s;
#endif
	char *p;
	int len;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	/* Check hibernation first. */
	if (len == 4 && !strncmp(buf, "disk", len))
		return PM_SUSPEND_MAX;

#ifdef CONFIG_SUSPEND
	for (s = &pm_states[state]; state < PM_SUSPEND_MAX; s++, state++)
		if (s->state && len == strlen(s->label)
		    && !strncmp(buf, s->label, len))
			return s->state;
#endif

	return PM_SUSPEND_ON;
}



#ifdef CONFIG_SUSPEND_HELPER
static struct workqueue_struct *suspend_helper_wq;
struct state_store_params {
	const char *buf;
	size_t n;
};

struct suspend_helper_data {
	struct work_struct work;
	struct completion done;
	struct state_store_params params;
	int result;
};
struct suspend_helper_data *suspend_helper_data;

static void suspend_helper(struct work_struct *work)
{
	struct suspend_helper_data *data = (struct suspend_helper_data *)
		container_of(work, struct suspend_helper_data, work);
	const char *buf = data->params.buf;
	size_t n = data->params.n;
	suspend_state_t state;
	int error = 0;

	pr_info("[suspend helper] %s: start!\n", __func__);

	error = pm_autosleep_lock();
	if (error) {
		goto out_nolock;
	}

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

	state = decode_state(buf, n);
	if (state < PM_SUSPEND_MAX)
		error = pm_suspend(state);
	else if (state == PM_SUSPEND_MAX)
		error = hibernate();
	else
		error = -EINVAL;

out:
	pm_autosleep_unlock();

out_nolock:
	// set result and notify completion
	data->result = error;
	complete(&data->done);

	pr_info("[suspend helper] %s: result = %d\n", __func__, error);
}

static ssize_t state_store_helper(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t n)
{
	int error;
	int freezable = 0;

	// we don't need to freeze. so tell the freezer
	if (!freezer_should_skip(current)) {
		freezable = 1;
		freezer_do_not_count();
		pr_info("[suspend helper] %s: freezer should skip me (%s:%d)\n",
				__func__, current->comm, current->pid);
	}

	suspend_helper_data->params.buf = buf;
	suspend_helper_data->params.n = n;
	INIT_COMPLETION(suspend_helper_data->done);

	// use kworker for suspend resume
	queue_work(suspend_helper_wq, &suspend_helper_data->work);

	// wait for suspend/resume work to be complete
	wait_for_completion(&suspend_helper_data->done);

	if (freezable) {
		// set ourself as freezable
		freezer_count();
	}

	error = suspend_helper_data->result;
	pr_info("[suspend helper] %s: suspend_helper returned %d\n", __func__, error);

	return error ? error : n;
}

static int suspend_helper_init(void)
{
	int ret = 0;

	suspend_helper_wq = alloc_ordered_workqueue("suspend_helper", 0);
	if (!suspend_helper_wq)
		return -ENOMEM;

	suspend_helper_data = kzalloc(sizeof(struct suspend_helper_data), GFP_KERNEL);
	if (!suspend_helper_data) {
		ret = -ENOMEM;
		goto out_destroy_wq;
	}

	INIT_WORK(&suspend_helper_data->work, suspend_helper);
	init_completion(&suspend_helper_data->done);

	pr_info("[suspend helper] %s: init done\n", __func__);

	return 0;
out_destroy_wq:
	destroy_workqueue(suspend_helper_wq);

	return ret;
}
#endif

#ifdef SUSPEND_WAKEUP_BOOST
static void pr_sched_state(const char *msg)
{
	pr_debug("[sched state] %s: (%s:%d) %pS policy=%d, prio=%d, static_prio=%d, normal_prio=%d, rt_priority=%d\n",
		msg, current->comm, current->pid,
		current->sched_class, current->policy,
		current->prio, current->static_prio, current->normal_prio, current->rt_priority);
}
#endif

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t n)
{
	suspend_state_t state;
	int error;
#ifdef SUSPEND_WAKEUP_BOOST
	int orig_policy = current->policy;
	int orig_nice = task_nice(current);
	struct sched_param param = { .sched_priority = 1 };
#endif

#ifdef CONFIG_SUSPEND_HELPER
	if (suspend_helper_data) {
		pr_info("[suspend helper] %s: Let our helper do the real work!\n", __func__);
		return state_store_helper(kobj, attr, buf, n);
	}
	pr_info("[suspend helper] %s: helper data not avaialbe.. Fall back to the legacy code..\n", __func__);
#endif

	error = pm_autosleep_lock();
	if (error)
		return error;

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

#ifdef SUSPEND_WAKEUP_BOOST
	pr_sched_state("before boost");
	sched_setscheduler_nocheck(current, SCHED_FIFO, &param);
	pr_sched_state("after boost");
#endif
	state = decode_state(buf, n);
	if (state < PM_SUSPEND_MAX)
		error = pm_suspend(state);
	else if (state == PM_SUSPEND_MAX)
		error = hibernate();
	else
		error = -EINVAL;
#ifdef SUSPEND_WAKEUP_BOOST
	pr_sched_state("before restore");
	param.sched_priority = 0;
	sched_setscheduler_nocheck(current, orig_policy, &param);
	set_user_nice(current, orig_nice);
	pr_sched_state("after restore");
#endif
 out:
	pm_autosleep_unlock();
	return error ? error : n;
}

power_attr(state);

#ifdef CONFIG_PM_SLEEP
/*
 * The 'wakeup_count' attribute, along with the functions defined in
 * drivers/base/power/wakeup.c, provides a means by which wakeup events can be
 * handled in a non-racy way.
 *
 * If a wakeup event occurs when the system is in a sleep state, it simply is
 * woken up.  In turn, if an event that would wake the system up from a sleep
 * state occurs when it is undergoing a transition to that sleep state, the
 * transition should be aborted.  Moreover, if such an event occurs when the
 * system is in the working state, an attempt to start a transition to the
 * given sleep state should fail during certain period after the detection of
 * the event.  Using the 'state' attribute alone is not sufficient to satisfy
 * these requirements, because a wakeup event may occur exactly when 'state'
 * is being written to and may be delivered to user space right before it is
 * frozen, so the event will remain only partially processed until the system is
 * woken up by another event.  In particular, it won't cause the transition to
 * a sleep state to be aborted.
 *
 * This difficulty may be overcome if user space uses 'wakeup_count' before
 * writing to 'state'.  It first should read from 'wakeup_count' and store
 * the read value.  Then, after carrying out its own preparations for the system
 * transition to a sleep state, it should write the stored value to
 * 'wakeup_count'.  If that fails, at least one wakeup event has occurred since
 * 'wakeup_count' was read and 'state' should not be written to.  Otherwise, it
 * is allowed to write to 'state', but the transition will be aborted if there
 * are any wakeup events detected after 'wakeup_count' was written to.
 */

static ssize_t wakeup_count_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	unsigned int val;

	return pm_get_wakeup_count(&val, true) ?
		sprintf(buf, "%u\n", val) : -EINTR;
}

static ssize_t wakeup_count_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned int val;
	int error;

	error = pm_autosleep_lock();
	if (error)
		return error;

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

	error = -EINVAL;
	if (sscanf(buf, "%u", &val) == 1) {
		if (pm_save_wakeup_count(val))
			error = n;
		else
			pm_print_active_wakeup_sources();
	}

 out:
	pm_autosleep_unlock();
	return error;
}

power_attr(wakeup_count);

#ifdef CONFIG_PM_AUTOSLEEP
static ssize_t autosleep_show(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	suspend_state_t state = pm_autosleep_state();

	if (state == PM_SUSPEND_ON)
		return sprintf(buf, "off\n");

#ifdef CONFIG_SUSPEND
	if (state < PM_SUSPEND_MAX)
		return sprintf(buf, "%s\n", pm_states[state].state ?
					pm_states[state].label : "error");
#endif
#ifdef CONFIG_HIBERNATION
	return sprintf(buf, "disk\n");
#else
	return sprintf(buf, "error");
#endif
}

static ssize_t autosleep_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t n)
{
	suspend_state_t state = decode_state(buf, n);
	int error;

	if (state == PM_SUSPEND_ON
	    && strcmp(buf, "off") && strcmp(buf, "off\n"))
		return -EINVAL;

	error = pm_autosleep_set_state(state);
	return error ? error : n;
}

power_attr(autosleep);
#endif /* CONFIG_PM_AUTOSLEEP */

#ifdef CONFIG_PM_WAKELOCKS
static ssize_t wake_lock_show(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	return pm_show_wakelocks(buf, true);
}

static ssize_t wake_lock_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t n)
{
	int error = pm_wake_lock(buf);
	return error ? error : n;
}

power_attr(wake_lock);

static ssize_t wake_unlock_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	return pm_show_wakelocks(buf, false);
}

static ssize_t wake_unlock_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t n)
{
	int error = pm_wake_unlock(buf);
	return error ? error : n;
}

power_attr(wake_unlock);

#endif /* CONFIG_PM_WAKELOCKS */
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
static int cpufreq_max_limit_val = -1;
static int cpufreq_min_limit_val = -1;
struct cpufreq_limit_handle *cpufreq_max_hd;
struct cpufreq_limit_handle *cpufreq_min_hd;
DEFINE_MUTEX(cpufreq_limit_mutex);

static ssize_t cpufreq_table_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i, count = 0;
	unsigned int freq;

	struct cpufreq_frequency_table *table;

	table = cpufreq_frequency_get_table(0);
	if (table == NULL)
		return 0;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++)
		count = i;

	for (i = count; i >= 0; i--) {
		freq = table[i].frequency;

		if (freq < MIN_FREQ_LIMIT || freq > MAX_FREQ_LIMIT)
			continue;

		len += sprintf(buf + len, "%u ", freq);
	}

	len--;
	len += sprintf(buf + len, "\n");

	return len;
}

static ssize_t cpufreq_table_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	pr_err("%s: cpufreq_table is read-only\n", __func__);
	return -EINVAL;
}

static ssize_t cpufreq_max_limit_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", cpufreq_max_limit_val);
}

static ssize_t cpufreq_max_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int val;
	ssize_t ret = -EINVAL;

	if (sscanf(buf, "%d", &val) != 1) {
		pr_err("%s: Invalid cpufreq format\n", __func__);
		goto out;
	}

	mutex_lock(&cpufreq_limit_mutex);
	if (cpufreq_max_hd) {
		cpufreq_limit_put(cpufreq_max_hd);
		cpufreq_max_hd = NULL;
	}

	if (val != -1) {
		cpufreq_max_hd = cpufreq_limit_max_freq(val, "user lock(max)");
		if (IS_ERR(cpufreq_max_hd)) {
			pr_err("%s: fail to get the handle\n", __func__);
			cpufreq_max_hd = NULL;
		}
	}

	cpufreq_max_hd ?
		(cpufreq_max_limit_val = val) : (cpufreq_max_limit_val = -1);

	mutex_unlock(&cpufreq_limit_mutex);
	ret = n;
out:
	return ret;
}

static ssize_t cpufreq_min_limit_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", cpufreq_min_limit_val);
}

static ssize_t cpufreq_min_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int val;
	ssize_t ret = -EINVAL;

	if (sscanf(buf, "%d", &val) != 1) {
		pr_err("%s: Invalid cpufreq format\n", __func__);
		goto out;
	}

	mutex_lock(&cpufreq_limit_mutex);
	if (cpufreq_min_hd) {
		cpufreq_limit_put(cpufreq_min_hd);
		cpufreq_min_hd = NULL;
	}

	if (val != -1) {
		cpufreq_min_hd = cpufreq_limit_min_freq(val, "user lock(min)");
		if (IS_ERR(cpufreq_min_hd)) {
			pr_err("%s: fail to get the handle\n", __func__);
			cpufreq_min_hd = NULL;
		}
	}

	cpufreq_min_hd ?
		(cpufreq_min_limit_val = val) : (cpufreq_min_limit_val = -1);

	mutex_unlock(&cpufreq_limit_mutex);
	ret = n;
out:
	return ret;
}

power_attr(cpufreq_table);
power_attr(cpufreq_max_limit);
power_attr(cpufreq_min_limit);

struct cpufreq_limit_handle *cpufreq_min_touch;
struct cpufreq_limit_handle *cpufreq_min_camera;
struct cpufreq_limit_handle *cpufreq_min_sensor;

int set_freq_limit(unsigned long id, unsigned int freq)
{
	ssize_t ret = -EINVAL;

	mutex_lock(&cpufreq_limit_mutex);

	if (cpufreq_min_touch) {
		cpufreq_limit_put(cpufreq_min_touch);
		cpufreq_min_touch = NULL;
	}

	if (cpufreq_min_camera) {
		cpufreq_limit_put(cpufreq_min_camera);
		cpufreq_min_camera = NULL;
	}

	if (cpufreq_min_sensor) {
		cpufreq_limit_put(cpufreq_min_sensor);
		cpufreq_min_sensor = NULL;
	}

	pr_debug("%s: id=%d freq=%d\n", __func__, (int)id, freq);

	/* min lock */
	if (id & DVFS_TOUCH_ID) {
		if (freq != -1) {
			cpufreq_min_touch = cpufreq_limit_min_freq(freq, "touch min");
			if (IS_ERR(cpufreq_min_touch)) {
				pr_err("%s: fail to get the handle\n", __func__);
				goto out;
			}
		}
	}

	if (id & DVFS_CAMERA_ID) {
		if (freq != -1) {
			cpufreq_min_camera = cpufreq_limit_min_freq(freq, "camera min");
			if (IS_ERR(cpufreq_min_camera)) {
				pr_err("%s: fail to get the handle\n", __func__);
				goto out;
			}
		}
	}

	if (id & DVFS_SENSOR_ID) {
		if (freq != -1) {
			cpufreq_min_sensor = cpufreq_limit_min_freq(freq, "sensor min");
			if (IS_ERR(cpufreq_min_sensor)) {
				pr_err("%s: fail to get the handle\n", __func__);
				goto out;
			}
		}
	}

	ret = 0;
out:
	mutex_unlock(&cpufreq_limit_mutex);
	return ret;
}

#endif

#ifdef CONFIG_PM_TRACE
int pm_trace_enabled;

static ssize_t pm_trace_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_trace_enabled);
}

static ssize_t
pm_trace_store(struct kobject *kobj, struct kobj_attribute *attr,
	       const char *buf, size_t n)
{
	int val;

	if (sscanf(buf, "%d", &val) == 1) {
		pm_trace_enabled = !!val;
		if (pm_trace_enabled) {
			pr_warn("PM: Enabling pm_trace changes system date and time during resume.\n"
				"PM: Correct system time has to be restored manually after resume.\n");
		}
		return n;
	}
	return -EINVAL;
}

power_attr(pm_trace);

static ssize_t pm_trace_dev_match_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return show_trace_dev_match(buf, PAGE_SIZE);
}

static ssize_t
pm_trace_dev_match_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t n)
{
	return -EINVAL;
}

power_attr(pm_trace_dev_match);

#endif /* CONFIG_PM_TRACE */

#ifdef CONFIG_SEC_DVFS
DEFINE_MUTEX(dvfs_mutex);
static unsigned long dvfs_id;
static unsigned long apps_min_freq;
static unsigned long apps_max_freq;
static unsigned long thermald_max_freq;

static unsigned long touch_min_freq = MIN_TOUCH_LIMIT;
static unsigned long unicpu_max_freq = MAX_UNICPU_LIMIT;
static unsigned long sensor_min_freq = MIN_SENSOR_LIMIT;

static int verify_cpufreq_target(unsigned int target)
{
	int i;
	struct cpufreq_frequency_table *table;

	table = cpufreq_frequency_get_table(BOOT_CPU);
	if (table == NULL)
		return -EFAULT;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (table[i].frequency < MIN_FREQ_LIMIT ||
				table[i].frequency > MAX_FREQ_LIMIT)
			continue;

		if (target == table[i].frequency)
			return 0;
	}

	return -EINVAL;
}

int set_freq_limit(unsigned long id, unsigned int freq)
{
	unsigned int min = MIN_FREQ_LIMIT;
	unsigned int max = MAX_FREQ_LIMIT;

	if (freq != 0 && freq != -1 && verify_cpufreq_target(freq))
		return -EINVAL;

	mutex_lock(&dvfs_mutex);

	if (freq == -1)
		dvfs_id &= ~id;
	else
		dvfs_id |= id;

	/* update freq for apps/thermald */
	if (id == DVFS_APPS_MIN_ID)
		apps_min_freq = freq;
	else if (id == DVFS_APPS_MAX_ID)
		apps_max_freq = freq;
	else if (id == DVFS_THERMALD_ID)
		thermald_max_freq = freq;
	else if (id == DVFS_TOUCH_ID)
		touch_min_freq = freq;
	else if (id == DVFS_SENSOR_ID)
		sensor_min_freq = freq;

	/* set min - apps */
	if (dvfs_id & DVFS_APPS_MIN_ID && min < apps_min_freq)
		min = apps_min_freq;
	if (dvfs_id & DVFS_TOUCH_ID && min < touch_min_freq)
		min = touch_min_freq;
	if (dvfs_id & DVFS_SENSOR_ID && min < sensor_min_freq)
		min = sensor_min_freq;

	/* set max */
	if (dvfs_id & DVFS_APPS_MAX_ID && max > apps_max_freq)
		max = apps_max_freq;
	if (dvfs_id & DVFS_THERMALD_ID && max > thermald_max_freq)
		max = thermald_max_freq;
	if (dvfs_id & DVFS_UNICPU_ID && max > unicpu_max_freq)
		max = unicpu_max_freq;

	/* check min max*/
	if (min > max)
		min = max;

	/* update */
	set_min_lock(min);
	set_max_lock(max);

	pr_info("%s: ,dvfs-id:0x%lu ,id:-0x%lu %d, min %d, max %d\n",
				__func__,dvfs_id, id, freq, min, max);

	/* need to update now */
	if (id & UPDATE_NOW_BITS) {
		int cpu;
		unsigned int cur = 0;

		for_each_online_cpu(cpu) {
			cur = cpufreq_quick_get(cpu);
			if (cur) {
				struct cpufreq_policy policy;
				policy.cpu = cpu;

				if (cur < min)
					cpufreq_driver_target(&policy,
						min, CPUFREQ_RELATION_H);
				else if (cur > max)
					cpufreq_driver_target(&policy,
						max, CPUFREQ_RELATION_L);
			}
		}
	}

	mutex_unlock(&dvfs_mutex);

	return 0;
}

static ssize_t cpufreq_min_limit_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int freq;

	freq = get_min_lock();
	if (!freq)
		freq = -1;

	return sprintf(buf, "%d\n", freq);
}

static ssize_t cpufreq_min_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int freq_min_limit, ret = 0;

	ret = sscanf(buf, "%d", &freq_min_limit);

	if (ret != 1)
		return -EINVAL;

	set_freq_limit(DVFS_APPS_MIN_ID, freq_min_limit);

	return n;
}

static ssize_t cpufreq_max_limit_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int freq;

	freq = get_max_lock();
	if (!freq)
		freq = -1;

	return sprintf(buf, "%d\n", freq);
}

static ssize_t cpufreq_max_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int freq_max_limit, ret = 0;

	ret = sscanf(buf, "%d", &freq_max_limit);

	if (ret != 1)
		return -EINVAL;

	set_freq_limit(DVFS_APPS_MAX_ID, freq_max_limit);

	return n;
}
static ssize_t cpufreq_table_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i, count = 0;
	unsigned int freq;

	struct cpufreq_frequency_table *table;

	table = cpufreq_frequency_get_table(BOOT_CPU);
	if (table == NULL)
		return 0;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++)
		count = i;

	for (i = count; i >= 0; i--) {
		freq = table[i].frequency;

		if (freq < MIN_FREQ_LIMIT || freq > MAX_FREQ_LIMIT)
			continue;

		len += sprintf(buf + len, "%u ", freq);
	}

	len--;
	len += sprintf(buf + len, "\n");

	return len;
}

static ssize_t cpufreq_table_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	pr_info("%s: Not supported\n", __func__);
	return n;
}

power_attr(cpufreq_max_limit);
power_attr(cpufreq_min_limit);
power_attr(cpufreq_table);
#endif

#ifdef CONFIG_FREEZER
static ssize_t pm_freeze_timeout_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", freeze_timeout_msecs);
}

static ssize_t pm_freeze_timeout_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	freeze_timeout_msecs = val;
	return n;
}

power_attr(pm_freeze_timeout);

#endif	/* CONFIG_FREEZER*/

static struct attribute * g[] = {
	&state_attr.attr,
#ifdef CONFIG_PM_TRACE
	&pm_trace_attr.attr,
	&pm_trace_dev_match_attr.attr,
#endif
#ifdef CONFIG_PM_SLEEP
	&pm_async_attr.attr,
	&wakeup_count_attr.attr,
#ifdef CONFIG_PM_AUTOSLEEP
	&autosleep_attr.attr,
#endif
#ifdef CONFIG_PM_WAKELOCKS
	&wake_lock_attr.attr,
	&wake_unlock_attr.attr,
#endif
#ifdef CONFIG_PM_DEBUG
	&pm_test_attr.attr,
#endif
#endif
#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
	&cpufreq_table_attr.attr,
	&cpufreq_max_limit_attr.attr,
	&cpufreq_min_limit_attr.attr,
#endif
#ifdef CONFIG_SEC_DVFS
	&cpufreq_min_limit_attr.attr,
	&cpufreq_max_limit_attr.attr,
	&cpufreq_table_attr.attr,
#endif
#ifdef CONFIG_FREEZER
	&pm_freeze_timeout_attr.attr,
#endif
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = g,
};

#ifdef CONFIG_PM_RUNTIME
struct workqueue_struct *pm_wq;
EXPORT_SYMBOL_GPL(pm_wq);

static int __init pm_start_workqueue(void)
{
	pm_wq = alloc_workqueue("pm", WQ_FREEZABLE, 0);

	return pm_wq ? 0 : -ENOMEM;
}
#else
static inline int pm_start_workqueue(void) { return 0; }
#endif

static int __init pm_init(void)
{
	int error = pm_start_workqueue();
	if (error)
		return error;
	hibernate_image_size_init();
	hibernate_reserved_size_init();

	touch_evt_timer_val = ktime_set(2, 0);
	hrtimer_init(&tc_ev_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	tc_ev_timer.function = &tc_ev_stop;
	tc_ev_processed = 1;

	power_kobj = kobject_create_and_add("power", NULL);
	if (!power_kobj)
		return -ENOMEM;
	error = sysfs_create_group(power_kobj, &attr_group);
	if (error)
		return error;
#ifdef CONFIG_SUSPEND_HELPER
	suspend_helper_init();
#endif
#ifdef CONFIG_SEC_DVFS
	apps_min_freq = MIN_FREQ_LIMIT;
	apps_max_freq = MAX_FREQ_LIMIT;
	thermald_max_freq = MAX_FREQ_LIMIT;
#endif
	return pm_autosleep_init();
}

core_initcall(pm_init);
