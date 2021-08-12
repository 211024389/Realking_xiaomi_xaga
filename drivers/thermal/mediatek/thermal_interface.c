// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/thermal.h>

#include <linux/io.h>
#include "thermal_interface.h"

#define MAX_HEADROOM		(100)

struct therm_intf_info {
	int sw_ready;
	unsigned int cpu_cluster_num;
	struct device *dev;
	struct mutex lock;
	struct dentry *debug_dir;
};

static struct therm_intf_info tm_data;
void __iomem *thermal_csram_base;
EXPORT_SYMBOL(thermal_csram_base);
void __iomem *thermal_apu_mbox_base;
EXPORT_SYMBOL(thermal_apu_mbox_base);
struct fps_cooler_info fps_cooler_data;
EXPORT_SYMBOL(fps_cooler_data);

static struct md_info md_info_data;

static int therm_intf_read_csram_s32(int offset)
{
	void __iomem *addr = thermal_csram_base + offset;

	return sign_extend32(readl(addr), 31);
}

static int therm_intf_read_apu_mbox_s32(int offset)
{
	void __iomem *addr = thermal_apu_mbox_base + offset;

	return (!(thermal_apu_mbox_base) ? -1 : sign_extend32(readl(addr), 31));
}

int get_thermal_headroom(enum headroom_id id)
{
	int headroom = 0;

	if (!tm_data.sw_ready)
		return MAX_HEADROOM;

	if (id >= SOC_CPU0 && id < SOC_CPU0 + num_possible_cpus()) {
		headroom = therm_intf_read_csram_s32(CPU_HEADROOM_OFFSET + 4 * id);
	} else if (id == PCB_AP) {
		mutex_lock(&tm_data.lock);
		headroom = therm_intf_read_csram_s32(AP_NTC_HEADROOM_OFFSET);
		mutex_unlock(&tm_data.lock);
	}

	return headroom;
}
EXPORT_SYMBOL(get_thermal_headroom);

int set_cpu_min_opp(int gear, int opp)
{
	if (!tm_data.sw_ready)
		return -ENODEV;

	if (gear >= tm_data.cpu_cluster_num)
		return -EINVAL;

	writel(opp, thermal_csram_base + CPU_MIN_OPP_HINT_OFFSET + 4 * gear);

	return 0;
}
EXPORT_SYMBOL(set_cpu_min_opp);

int set_cpu_active_bitmask(int mask)
{
	if (!tm_data.sw_ready)
		return -ENODEV;

	writel(mask, thermal_csram_base + CPU_ACTIVE_BITMASK_OFFSET);

	return 0;
}
EXPORT_SYMBOL(set_cpu_active_bitmask);

int get_cpu_temp(int cpu_id)
{
	int temp = 25000;

	if (!tm_data.sw_ready || cpu_id >= num_possible_cpus())
		return temp;

	temp = therm_intf_read_csram_s32(CPU_TEMP_OFFSET + 4 * cpu_id);

	return temp;
}
EXPORT_SYMBOL(get_cpu_temp);

static ssize_t headroom_info_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int i;
	int len = 0;

	for (i = 0; i < NR_HEADROOM_ID; i++) {
		if (i == NR_HEADROOM_ID - 1)
			len += snprintf(buf + len, PAGE_SIZE - len, "%d\n",
				get_thermal_headroom((enum headroom_id)i));
		else
			len += snprintf(buf + len, PAGE_SIZE - len, "%d,",
				get_thermal_headroom((enum headroom_id)i));
	}

	return len;
}

static void write_ttj(unsigned int cpu_ttj, unsigned int gpu_ttj,
	unsigned int apu_ttj)
{
	void __iomem *addr = thermal_csram_base + TTJ_OFFSET;

	writel(cpu_ttj, addr);
	writel(gpu_ttj, addr + 4);
	writel(apu_ttj, addr + 8);

	if (thermal_apu_mbox_base)
		writel(apu_ttj, (void __iomem *)(thermal_apu_mbox_base + APU_MBOX_TTJ_OFFSET));
}

static ssize_t ttj_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%u, %u, %u\n",
		therm_intf_read_csram_s32(TTJ_OFFSET),
		therm_intf_read_csram_s32(TTJ_OFFSET + 4),
		therm_intf_read_csram_s32(TTJ_OFFSET + 8));

	return len;
}

static ssize_t ttj_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	char cmd[10];
	unsigned int cpu_ttj, gpu_ttj, apu_ttj;

	if (sscanf(buf, "%4s %u %u %u", cmd, &cpu_ttj, &gpu_ttj, &apu_ttj)
		== 4) {
		if (strncmp(cmd, "TTJ", 3) == 0) {
			write_ttj(cpu_ttj, gpu_ttj, apu_ttj);

			return count;
		}
	}

	pr_info("[thermal_ttj] invalid input\n");

	return -EINVAL;
}

static void write_power_budget(unsigned int cpu_pb, unsigned int gpu_pb,
	unsigned int apu_pb)
{
	void __iomem *addr = thermal_csram_base + POWER_BUDGET_OFFSET;

	writel(cpu_pb, addr);
	writel(gpu_pb, addr + 4);
	writel(apu_pb, addr + 8);

	if (thermal_apu_mbox_base)
		writel(apu_pb, (void __iomem *)(thermal_apu_mbox_base + APU_MBOX_PB_OFFSET));
}

static ssize_t power_budget_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%u, %u, %u\n",
		therm_intf_read_csram_s32(POWER_BUDGET_OFFSET),
		therm_intf_read_csram_s32(POWER_BUDGET_OFFSET + 4),
		therm_intf_read_csram_s32(POWER_BUDGET_OFFSET + 8));

	return len;
}

static ssize_t power_budget_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	char cmd[10];
	unsigned int cpu_pb, gpu_pb, apu_pb;

	if (sscanf(buf, "%3s %u %u %u", cmd, &cpu_pb, &gpu_pb, &apu_pb) == 4) {
		if (strncmp(cmd, "pb", 2) == 0) {
			write_power_budget(cpu_pb, gpu_pb, apu_pb);
			return count;
		}
	}

	pr_info("[thermal_power_budget] invalid input\n");

	return -EINVAL;
}

static ssize_t cpu_info_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		therm_intf_read_csram_s32(CPU_MIN_OPP_HINT_OFFSET),
		therm_intf_read_csram_s32(CPU_MIN_OPP_HINT_OFFSET + 4),
		therm_intf_read_csram_s32(CPU_MIN_OPP_HINT_OFFSET + 8),
		therm_intf_read_csram_s32(CPU_LIMIT_OPP_OFFSET),
		therm_intf_read_csram_s32(CPU_LIMIT_OPP_OFFSET + 4),
		therm_intf_read_csram_s32(CPU_LIMIT_OPP_OFFSET + 8),
		therm_intf_read_csram_s32(CPU_CUR_OPP_OFFSET),
		therm_intf_read_csram_s32(CPU_CUR_OPP_OFFSET + 4),
		therm_intf_read_csram_s32(CPU_CUR_OPP_OFFSET + 8),
		therm_intf_read_csram_s32(CPU_MAX_TEMP_OFFSET),
		therm_intf_read_csram_s32(CPU_MAX_TEMP_OFFSET + 4),
		therm_intf_read_csram_s32(CPU_MAX_TEMP_OFFSET + 8));

	return len;
}

static ssize_t cpu_temp_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;
	int cpu_id = 0;

	for (cpu_id = 0; cpu_id < num_possible_cpus(); cpu_id++) {
		if (cpu_id == num_possible_cpus() - 1)
			len += snprintf(buf + len, PAGE_SIZE - len, "%d\n",
				get_cpu_temp(cpu_id));
		else
			len += snprintf(buf + len, PAGE_SIZE - len, "%d,",
				get_cpu_temp(cpu_id));
	}

	return len;
}

static ssize_t gpu_info_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d,%d,%d\n",
		therm_intf_read_csram_s32(GPU_TEMP_OFFSET),
		therm_intf_read_csram_s32(GPU_TEMP_OFFSET + 4),
		therm_intf_read_csram_s32(GPU_TEMP_OFFSET + 8));

	return len;
}

static ssize_t apu_info_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	if (thermal_apu_mbox_base) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%d,%d,%d\n",
			therm_intf_read_apu_mbox_s32(APU_MBOX_TEMP_OFFSET),
			therm_intf_read_apu_mbox_s32(APU_MBOX_LIMIT_OPP_OFFSET),
			therm_intf_read_apu_mbox_s32(APU_MBOX_CUR_OPP_OFFSET));
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "%d,%d,%d\n",
			therm_intf_read_csram_s32(APU_TEMP_OFFSET),
			therm_intf_read_csram_s32(APU_TEMP_OFFSET + 4),
			therm_intf_read_csram_s32(APU_TEMP_OFFSET + 8));
	}

	return len;
}

static ssize_t fps_cooler_info_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d,%d,%d,%d,%d\n",
		fps_cooler_data.target_fps, fps_cooler_data.tpcb,
		fps_cooler_data.tpcb_slope, fps_cooler_data.ap_headroom,
		fps_cooler_data.n_sec_to_ttpcb);

	return len;
}

static ssize_t fps_cooler_info_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int target_fps, tpcb, tpcb_slope, ap_headroom, n_sec_to_ttpcb;
	void __iomem *addr = thermal_csram_base + AP_NTC_HEADROOM_OFFSET;
	void __iomem *tpcb_addr = thermal_csram_base + TPCB_OFFSET;

	if(sscanf(buf, "%d,%d,%d,%d,%d", &target_fps, &tpcb, &tpcb_slope,
				&ap_headroom, &n_sec_to_ttpcb) == 5)
	{
		if ((ap_headroom >= -100) && (ap_headroom <= 100))
		{
			writel(ap_headroom, addr);
			fps_cooler_data.ap_headroom = ap_headroom;
		} else {
			pr_info("[fps_cooler_info_store] invalid ap head room input\n");
			return -EINVAL;
		}

		writel(tpcb, tpcb_addr);
		fps_cooler_data.tpcb = tpcb;
		fps_cooler_data.target_fps = target_fps;
		fps_cooler_data.tpcb_slope = tpcb_slope;
		fps_cooler_data.n_sec_to_ttpcb = n_sec_to_ttpcb;
	} else {
		pr_info("[fps_cooler_info_store] invalid input\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t atc_show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d, %d, %d\n",
		therm_intf_read_csram_s32(ATC_OFFSET),
		therm_intf_read_csram_s32(ATC_OFFSET + 4),
		therm_intf_read_csram_s32(ATC_OFFSET + 8));

	return len;
}

static ssize_t target_tpcb_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;
	void __iomem *target_tpcb_addr = thermal_csram_base + TARGET_TPCB_OFFSET;
	int target_tpcb = readl(target_tpcb_addr);

	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n", target_tpcb);

	return len;
}

static ssize_t target_tpcb_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int target_tpcb = 0;
	void __iomem *target_tpcb_addr = thermal_csram_base + TARGET_TPCB_OFFSET;

	if(sscanf(buf, "%d", &target_tpcb) == 1)
		writel(target_tpcb, target_tpcb_addr);
	else {
		pr_info("[target_tpcb_store] invalid input\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t md_sensor_info_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	char info_type_s[MAX_MD_NAME_LENGTH];
	int len = 0, num = 0, val = 0, i = 0;
	struct md_thermal_sensor_t *ts_info;

	if (sscanf(buf, "%20s %d%n", info_type_s, &num, &len) != 2) {
		pr_info("%s: wrong scan info_type and num %s\n", __func__, buf);
		return -EINVAL;
	}

	if (strncmp(info_type_s, "s_tmp", 5) != 0) {
		pr_info("%s: wrong info type=%s\n", __func__, info_type_s);
		return -EINVAL;
	}

	if (num <= 0) {
		pr_info("%s: wrong input num=%d\n", __func__, num);
		return -EINVAL;
	}

	buf += len;

	if (!md_info_data.sensor_info) {
		ts_info = devm_kcalloc(tm_data.dev, num,
			sizeof(struct md_thermal_sensor_t), GFP_KERNEL);
		if (!ts_info)
			return -ENOMEM;

		md_info_data.sensor_info = ts_info;
		md_info_data.sensor_num = num;
	} else if (md_info_data.sensor_num != num) {
		pr_info("%s: wrong sensor num=%d %d\n", __func__, md_info_data.sensor_num, num);
		return -EINVAL;
	}

	ts_info = md_info_data.sensor_info;
	for (i = 0; i < md_info_data.sensor_num; i++) {
		if (sscanf(buf, " %d%n", &val, &len) == 1) {
			buf += len;
			ts_info[i].cur_temp = val;
		}
	}

	return count;
}

static ssize_t md_sensor_info_show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	int len = 0, i;
	struct md_thermal_sensor_t *ts_info;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d", md_info_data.sensor_num);

	if (!md_info_data.sensor_info) {
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
		return len;
	}

	ts_info = md_info_data.sensor_info;
	for (i = 0; i < md_info_data.sensor_num; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, ",%d", ts_info[i].cur_temp);

	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}

static ssize_t md_actuator_info_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	char info_type_s[MAX_MD_NAME_LENGTH];
	int len = 0, num = 0, val = 0, i = 0;
	struct md_thermal_actuator_t *ta_info;

	if (sscanf(buf, "%20s %d%n", info_type_s, &num, &len) != 2) {
		pr_info("%s: wrong scan info_type and num %s\n", __func__, buf);
		return -EINVAL;
	}

	if (strncmp(info_type_s, "a_ctl", 5) == 0) {
		md_info_data.md_autonomous_ctrl = num;
		return count;
	}

	if (strncmp(info_type_s, "a_cst", 5) != 0 &&
		strncmp(info_type_s, "a_mst", 5) != 0) {
		pr_info("%s: wrong info type=%s\n", __func__, info_type_s);
		return -EINVAL;
	}

	if (num <= 0) {
		pr_info("%s: wrong input num=%d\n", __func__, num);
		return -EINVAL;
	}

	buf += len;

	if (!md_info_data.actuator_info) {
		ta_info = devm_kcalloc(tm_data.dev, num,
			sizeof(struct md_thermal_actuator_t), GFP_KERNEL);
		if (!ta_info)
			return -ENOMEM;

		md_info_data.actuator_info = ta_info;
		md_info_data.actuator_num = num;
	} else if (md_info_data.actuator_num != num) {
		pr_info("%s: wrong actuator num=%d %d\n", __func__,
			md_info_data.actuator_num, num);
		return -EINVAL;
	}

	ta_info = md_info_data.actuator_info;
	if (strncmp(info_type_s, "a_cst", 5) == 0) {
		for (i = 0; i < md_info_data.actuator_num; i++) {
			if (sscanf(buf, " %d%n", &val, &len) == 1) {
				buf += len;
				ta_info[i].cur_status = val;
			}
		}
	} else {
		for (i = 0; i < md_info_data.actuator_num; i++) {
			if (sscanf(buf, " %d%n", &val, &len) == 1) {
				buf += len;
				ta_info[i].max_status = val;
			}
		}
	}

	return count;
}

static ssize_t md_actuator_info_show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	int len = 0, i;
	struct md_thermal_actuator_t *ta_info;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d", md_info_data.md_autonomous_ctrl);
	len += snprintf(buf + len, PAGE_SIZE - len, ",%d", md_info_data.actuator_num);

	if (!md_info_data.actuator_info) {
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
		return len;
	}

	ta_info = md_info_data.actuator_info;
	for (i = 0; i < md_info_data.actuator_num; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, ",%d,%d",
			ta_info[i].cur_status, ta_info[i].max_status);

	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}

static struct kobj_attribute ttj_attr = __ATTR_RW(ttj);
static struct kobj_attribute power_budget_attr = __ATTR_RW(power_budget);
static struct kobj_attribute cpu_info_attr = __ATTR_RO(cpu_info);
static struct kobj_attribute gpu_info_attr = __ATTR_RO(gpu_info);
static struct kobj_attribute apu_info_attr = __ATTR_RO(apu_info);
static struct kobj_attribute fps_cooler_info_attr = __ATTR_RW(fps_cooler_info);
static struct kobj_attribute cpu_temp_attr = __ATTR_RO(cpu_temp);
static struct kobj_attribute headroom_info_attr = __ATTR_RO(headroom_info);
static struct kobj_attribute atc_attr = __ATTR_RO(atc);
static struct kobj_attribute target_tpcb_attr = __ATTR_RW(target_tpcb);
static struct kobj_attribute md_sensor_info_attr = __ATTR_RW(md_sensor_info);
static struct kobj_attribute md_actuator_info_attr = __ATTR_RW(md_actuator_info);

static struct attribute *thermal_attrs[] = {
	&ttj_attr.attr,
	&power_budget_attr.attr,
	&cpu_info_attr.attr,
	&gpu_info_attr.attr,
	&apu_info_attr.attr,
	&fps_cooler_info_attr.attr,
	&cpu_temp_attr.attr,
	&headroom_info_attr.attr,
	&atc_attr.attr,
	&target_tpcb_attr.attr,
	&md_sensor_info_attr.attr,
	&md_actuator_info_attr.attr,
	NULL
};
static struct attribute_group thermal_attr_group = {
	.name	= "thermal",
	.attrs	= thermal_attrs,
};

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int emul_temp_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "%d,%d,%d,%d\n",
		therm_intf_read_csram_s32(EMUL_TEMP_OFFSET),
		therm_intf_read_csram_s32(EMUL_TEMP_OFFSET + 4),
		therm_intf_read_csram_s32(EMUL_TEMP_OFFSET + 8),
		therm_intf_read_csram_s32(EMUL_TEMP_OFFSET + 12));

	return 0;
}

static ssize_t emul_temp_write(struct file *flip,
			const char *ubuf, size_t cnt, loff_t *data)
{
	int ret, temp;
	char *buf;
	char target[10];
	void __iomem *addr = thermal_csram_base + EMUL_TEMP_OFFSET;

	buf = kzalloc(cnt + 1, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, cnt)) {
		ret = -EFAULT;
		goto err;
	}
	buf[cnt] = '\0';

	if (sscanf(buf, "%10s %d", target, &temp) != 2) {
		dev_info(tm_data.dev, "invalid input for emul temp\n");
		ret = -EINVAL;
		goto err;
	}

	if (strncmp(target, "cpu", 3) == 0) {
		writel(temp, addr);
	} else if (strncmp(target, "gpu", 3) == 0) {
		writel(temp, addr + 4);
	} else if (strncmp(target, "apu", 3) == 0) {
		writel(temp, addr + 8);
		if (thermal_apu_mbox_base)
			writel(temp, thermal_apu_mbox_base + APU_MBOX_EMUL_TEMP_OFFSET);
	} else if (strncmp(target, "vcore", 5) == 0) {
		writel(temp, addr + 12);
	}

	ret = cnt;

err:
	kfree(buf);

	return ret;
}

static int emul_temp_open(struct inode *i, struct file *file)
{
	return single_open(file, emul_temp_show, i->i_private);
}

static const struct file_operations emul_temp_fops = {
	.owner = THIS_MODULE,
	.open = emul_temp_open,
	.read = seq_read,
	.write = emul_temp_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void therm_intf_debugfs_init(void)
{
	void __iomem *addr = thermal_csram_base + EMUL_TEMP_OFFSET;

	tm_data.debug_dir = debugfs_create_dir("thermal", NULL);
	if (!tm_data.debug_dir) {
		dev_info(tm_data.dev, "failed to create thermal debugfs!\n");
		return;
	}

	debugfs_create_file("emul_temp", 0640, tm_data.debug_dir, NULL, &emul_temp_fops);

	writel(THERMAL_TEMP_INVALID, addr);
	writel(THERMAL_TEMP_INVALID, addr + 4);
	writel(THERMAL_TEMP_INVALID, addr + 8);
	writel(THERMAL_TEMP_INVALID, addr + 12);

	if (thermal_apu_mbox_base)
		writel(THERMAL_TEMP_INVALID, thermal_apu_mbox_base + APU_MBOX_EMUL_TEMP_OFFSET);
}

static void therm_intf_debugfs_exit(void)
{
	debugfs_remove_recursive(tm_data.debug_dir);
}
#else
static void therm_intf_debugfs_init(void) {}
static void therm_intf_debugfs_exit(void) {}
#endif


static const struct of_device_id therm_intf_of_match[] = {
	{ .compatible = "mediatek,therm_intf", },
	{},
};
MODULE_DEVICE_TABLE(of, therm_intf_of_match);

static int therm_intf_probe(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *addr;
	struct device_node *cpu_np;
	struct of_phandle_args args;
	unsigned int cpu, max_perf_domain = 0;
	int ret;

	if (!pdev->dev.of_node) {
		dev_info(&pdev->dev, "Only DT based supported\n");
		return -ENODEV;
	}

	tm_data.dev = &pdev->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "therm_sram");
	addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(addr))
		return PTR_ERR(addr);

	thermal_csram_base = addr;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apu_mbox");
	addr = ioremap(res->start, res->end - res->start + 1);
	if (!IS_ERR(addr))
		thermal_apu_mbox_base = addr;

	/* get CPU cluster num */
	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_info(&pdev->dev, "Failed to get cpu %d device\n", cpu);
			return -ENODEV;
		}

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);

		if (ret < 0)
			return ret;

		max_perf_domain = max(max_perf_domain, args.args[0]);
	}

	tm_data.cpu_cluster_num = max_perf_domain + 1;
	dev_info(&pdev->dev, "cpu_cluster_num = %d\n", tm_data.cpu_cluster_num);

	ret = sysfs_create_group(kernel_kobj, &thermal_attr_group);
	if (ret) {
		dev_info(&pdev->dev, "failed to create thermal sysfs, ret=%d!\n", ret);
		return ret;
	}

	therm_intf_debugfs_init();

	writel(100, thermal_csram_base + AP_NTC_HEADROOM_OFFSET);
	write_ttj(0, 0, 0);
	write_power_budget(0, 0, 0);

	mutex_init(&tm_data.lock);

	tm_data.sw_ready = 1;

	return 0;
}

static int therm_intf_remove(struct platform_device *pdev)
{
	therm_intf_debugfs_exit();
	sysfs_remove_group(kernel_kobj, &thermal_attr_group);

	return 0;
}

static struct platform_driver therm_intf_driver = {
	.probe = therm_intf_probe,
	.remove = therm_intf_remove,
	.driver = {
		.name = "mtk-thermal-interface",
		.of_match_table = therm_intf_of_match,
	},
};

module_platform_driver(therm_intf_driver);

MODULE_AUTHOR("Henry Huang <henry.huang@mediatek.com>");
MODULE_DESCRIPTION("Mediatek thermal interface driver");
MODULE_LICENSE("GPL v2");

