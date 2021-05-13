// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/energy_model.h>
#include <linux/topology.h>
#include <trace/hooks/topology.h>
#include <trace/events/sched.h>
#include <trace/hooks/sched.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "../../../../kernel/sched/sched.h"
#include "sched_sys_common.h"

DEFINE_MUTEX(sched_core_pause_mutex);

/*
 * Pause a cpu
 * Success or no need to change: return 0
 */
int sched_pause_cpu(int cpu)
{
	int err = 0;
	struct cpumask cpu_pause_mask;
	struct cpumask cpu_inactive_mask;

	cpumask_complement(&cpu_inactive_mask, cpu_active_mask);
	if (cpumask_test_cpu(cpu, &cpu_inactive_mask)) {
		pr_info("[Core Pause]Already Pause: cpu=%d, pause_mask=0x%lx, active_mask=0x%lx\n",
			cpu, cpu_pause_mask.bits[0], cpu_active_mask->bits[0]);
		return err;
	}

	mutex_lock(&sched_core_pause_mutex);
	cpumask_clear(&cpu_pause_mask);
	cpumask_set_cpu(cpu, &cpu_pause_mask);

	err = pause_cpus(&cpu_pause_mask);
	if (err) {
		pr_info("[Core Pause]Pause fail: cpu=%d, pause_mask=0x%lx, active_mask=0x%lx, err=%d\n",
			cpu, cpu_pause_mask.bits[0], cpu_active_mask->bits[0], err);
	} else {
		pr_info("[Core Pause]Pause success: cpu=%d, pause_mask=0x%lx, active_mask=0x%lx\n",
			cpu, cpu_pause_mask.bits[0], cpu_active_mask->bits[0]);
	}
	mutex_unlock(&sched_core_pause_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(sched_pause_cpu);

/*
 * Resume a cpu
 * Success or no need to change: return 0
 */
int sched_resume_cpu(int cpu)
{
	int err = 0;
	struct cpumask cpu_resume_mask;

	if (cpumask_test_cpu(cpu, cpu_active_mask)) {
		pr_info("[Core Pause]Already Resume: cpu=%d, resume_mask=0x%lx, active_mask=0x%lx\n",
				cpu, cpu_resume_mask.bits[0], cpu_active_mask->bits[0]);
		return err;
	}

	mutex_lock(&sched_core_pause_mutex);
	cpumask_clear(&cpu_resume_mask);
	cpumask_set_cpu(cpu, &cpu_resume_mask);
	err = resume_cpus(&cpu_resume_mask);
	if (err) {
		pr_info("[Core Pause]Resume fail: cpu=%d, resume_mask=0x%lx, active_mask=0x%lx, err=%d\n",
				cpu, cpu_resume_mask.bits[0], cpu_active_mask->bits[0], err);
	} else {
		pr_info("[Core Pause]Resume success: cpu=%d, resume_mask=0x%lx, active_mask=0x%lx\n",
				cpu, cpu_resume_mask.bits[0], cpu_active_mask->bits[0]);
	}
	mutex_unlock(&sched_core_pause_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(sched_resume_cpu);

static ssize_t show_sched_core_pause_info(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	unsigned int len = 0;
	unsigned int max_len = 4096;

	len += snprintf(buf+len, max_len-len,
			"cpu_active_mask=0x%lx\n",
			__cpu_active_mask.bits[0]);

	return len;
}

static ssize_t set_sched_pause_cpu(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *ubuf,
				size_t cnt)
{
	unsigned int cpu_id = -1;

	if (sscanf(ubuf, "%iu", &cpu_id) != 0) {
		if (cpu_id < nr_cpu_ids)
			sched_pause_cpu(cpu_id);
	}

	return cnt;
}

static ssize_t set_sched_resume_cpu(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *ubuf,
				size_t cnt)
{
	unsigned int cpu_id = -1;

	if (sscanf(ubuf, "%iu", &cpu_id) != 0) {
		if (cpu_id < nr_cpu_ids)
			sched_resume_cpu(cpu_id);
	}

	return cnt;
}

struct kobj_attribute sched_core_pause_info_attr =
__ATTR(sched_core_pause_info, 0400, show_sched_core_pause_info, NULL);

struct kobj_attribute set_sched_pause_cpu_attr =
__ATTR(sched_pause_cpu, 0200, NULL, set_sched_pause_cpu);

struct kobj_attribute set_sched_resume_cpu_attr =
__ATTR(sched_resume_cpu, 0200, NULL, set_sched_resume_cpu);
