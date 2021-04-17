// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 MediaTek Inc.
 */

/**
 * @file    mtk_gpufreq_core.c
 * @brief   GPU-DVFS Driver Platform Implementation
 */

/**
 * ===============================================
 * Include
 * ===============================================
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/pm_runtime.h>
#if IS_ENABLED(CONFIG_MTK_DEVINFO)
#include <linux/nvmem-consumer.h>
#endif

#include <gpufreq_v2.h>
#include <gpufreq_debug.h>
#include <gpuppm.h>
#include <gpufreq_common.h>
#include <gpufreq_mt6893.h>

#if IS_ENABLED(CONFIG_MTK_BATTERY_OC_POWER_THROTTLING)
#include <mtk_battery_oc_throttling.h>
#endif
#if IS_ENABLED(CONFIG_MTK_BATTERY_PERCENT_THROTTLING)
#include <mtk_bp_thl.h>
#endif
#if IS_ENABLED(CONFIG_MTK_LOW_BATTERY_POWER_THROTTLING)
#include <mtk_low_battery_throttling.h>
#endif
#if IS_ENABLED(CONFIG_MTK_STATIC_POWER)
#include <leakage_table_v2/mtk_static_power.h>
#endif
#if IS_ENABLED(CONFIG_MTK_PBM)
#include <mtk_pbm.h>
#endif
#if IS_ENABLED(CONFIG_MTK_FREQ_HOPPING)
#include <mtk_freqhopping_drv.h>
#endif

/**
 * ===============================================
 * Local Function Declaration
 * ===============================================
 */
/* misc function */
static unsigned int __gpufreq_custom_init_enable(void);
static unsigned int __gpufreq_dvfs_enable(void);
static void __gpufreq_dump_bringup_status(void);
static void __gpufreq_measure_power(void);
static void __gpufreq_set_springboard(void);
static void __gpufreq_kick_pbm(enum gpufreq_buck_state buck);
static void __iomem *__gpufreq_of_ioremap(const char *node_name, int idx);
static void __gpufreq_set_vgpu_mode(unsigned int mode);
static void __gpufreq_interpolate_volt(void);
static void __gpufreq_apply_aging(bool apply_aging);
/* dvfs function */
static int __gpufreq_custom_commit_gpu(
	unsigned int target_freq, unsigned int target_volt,
	enum gpufreq_dvfs_state key);
static int __gpufreq_freq_scale_gpu(
	unsigned int freq_old, unsigned int freq_new);
static int __gpufreq_volt_scale_gpu(
	unsigned int vgpu_old, unsigned int vgpu_new,
	unsigned int vsram_old, unsigned int vsram_new);
static int __gpufreq_switch_clksrc(enum gpufreq_clk_src clksrc);
static unsigned int __gpufreq_calculate_pcw(
	unsigned int freq, enum gpufreq_postdiv posdiv_power);
static unsigned int __gpufreq_settle_time_vgpu(bool mode, int deltaV);
static unsigned int __gpufreq_settle_time_vsram(bool mode, int deltaV);
/* get function */
static unsigned int __gpufreq_get_fmeter_fgpu(void);
// static unsigned int __gpufreq_get_fmeter_fgstack(void);
static unsigned int __gpufreq_get_real_fgpu(void);
static unsigned int __gpufreq_get_real_vgpu(void);
static unsigned int __gpufreq_get_real_vsram(void);
// static unsigned int __gpufreq_get_real_fgstack(void);
// static unsigned int __gpufreq_get_real_vgstack(void);
static enum gpufreq_postdiv __gpufreq_get_real_posdiv(void);
static enum gpufreq_postdiv __gpufreq_get_posdiv_by_freq(unsigned int freq);
/* power control function */
static void __gpufreq_external_cg_control(void);
static int __gpufreq_cg_control(enum gpufreq_cg_state cg);
static int __gpufreq_mtcmos_control(enum gpufreq_mtcmos_state mtcmos);
static int __gpufreq_buck_control(enum gpufreq_buck_state buck);
/* init function */
static void __gpufreq_init_opp_idx(void);
static void __gpufreq_init_shader_present(void);
static void __gpufreq_segment_adjustment(struct platform_device *pdev);
static int __gpufreq_init_opp_table(struct platform_device *pdev);
static int __gpufreq_init_segment_id(struct platform_device *pdev);
static int __gpufreq_init_clk(struct platform_device *pdev);
static int __gpufreq_init_pmic(struct platform_device *pdev);
static int __gpufreq_pdrv_probe(struct platform_device *pdev);
static int __gpufreq_mtcmos_pdrv_probe(struct platform_device *pdev);
static int __gpufreq_mtcmos_pdrv_remove(struct platform_device *pdev);
static int __gpufreq_mfg2_probe(struct platform_device *pdev);
static int __gpufreq_mfg3_probe(struct platform_device *pdev);
static int __gpufreq_mfg4_probe(struct platform_device *pdev);
static int __gpufreq_mfg5_probe(struct platform_device *pdev);
static int __gpufreq_mfg6_probe(struct platform_device *pdev);
static int __gpufreq_mfg_remove(struct platform_device *pdev);

/**
 * ===============================================
 * Local Variable Definition
 * ===============================================
 */
static const struct of_device_id g_gpufreq_of_match[] = {
	{ .compatible = "mediatek,gpufreq" },
	{ /* sentinel */ }
};
static struct platform_driver g_gpufreq_pdrv = {
	.probe = __gpufreq_pdrv_probe,
	.remove = NULL,
	.driver = {
		.name = "gpufreq",
		.owner = THIS_MODULE,
		.of_match_table = g_gpufreq_of_match,
	},
};

static const struct gpufreq_mfg_fp mfg2_fp = {
	.probe = __gpufreq_mfg2_probe,
	.remove = __gpufreq_mfg_remove,
};

static const struct gpufreq_mfg_fp mfg3_fp = {
	.probe = __gpufreq_mfg3_probe,
	.remove = __gpufreq_mfg_remove,
};

static const struct gpufreq_mfg_fp mfg4_fp = {
	.probe = __gpufreq_mfg4_probe,
	.remove = __gpufreq_mfg_remove,
};

static const struct gpufreq_mfg_fp mfg5_fp = {
	.probe = __gpufreq_mfg5_probe,
	.remove = __gpufreq_mfg_remove,
};

static const struct gpufreq_mfg_fp mfg6_fp = {
	.probe = __gpufreq_mfg6_probe,
	.remove = __gpufreq_mfg_remove,
};

static const struct of_device_id g_gpufreq_mtcmos_of_match[] = {
	{
		.compatible = "mediatek,mt6893-mfg2",
		.data = &mfg2_fp,
	}, {
		.compatible = "mediatek,mt6893-mfg3",
		.data = &mfg3_fp,
	}, {
		.compatible = "mediatek,mt6893-mfg4",
		.data = &mfg4_fp,
	}, {
		.compatible = "mediatek,mt6893-mfg5",
		.data = &mfg5_fp,
	}, {
		.compatible = "mediatek,mt6893-mfg6",
		.data = &mfg6_fp,
	}, {
		/* sentinel */
	}
};

static struct platform_driver g_gpufreq_mtcmos_pdrv = {
	.probe = __gpufreq_mtcmos_pdrv_probe,
	.remove = __gpufreq_mtcmos_pdrv_remove,
	.driver = {
		.name = "gpufreq-mtcmos",
		.of_match_table = g_gpufreq_mtcmos_of_match,
	},
};

static void __iomem *g_apmixed_base;
static void __iomem *g_mfg_base;
static void __iomem *g_sleep;
static void __iomem *g_infracfg_base;
static void __iomem *g_infra_bpi_bsi_slv0;
static void __iomem *g_infra_peri_debug1;
static void __iomem *g_infra_peri_debug2;
static void __iomem *g_infra_peri_debug3;
static void __iomem *g_infra_peri_debug4;
static struct gpufreq_pmic_info *g_pmic;
static struct gpufreq_clk_info *g_clk;
static struct gpufreq_mtcmos_info *g_mtcmos;
static struct gpufreq_status g_gpu;
// static struct gpufreq_status g_gstact;
static unsigned int g_shader_present;
static bool g_probe_done;
static bool g_stress_test_enable;
static bool g_aging_enable;
static enum gpufreq_dvfs_state g_dvfs_state;
static DEFINE_MUTEX(gpufreq_lock_gpu);
// static DEFINE_MUTEX(gpufreq_lock_gstack);

/**
 * ===============================================
 * External Function Definition
 * ===============================================
 */
unsigned int __gpufreq_bringup(void)
{
	return GPUFREQ_BRINGUP;
}
EXPORT_SYMBOL(__gpufreq_bringup);

unsigned int __gpufreq_power_ctrl_enable(void)
{
	return GPUFREQ_POWER_CTRL_ENABLE;
}
EXPORT_SYMBOL(__gpufreq_power_ctrl_enable);

unsigned int __gpufreq_get_dvfs_state(void)
{
	return g_dvfs_state;
}
EXPORT_SYMBOL(__gpufreq_get_dvfs_state);

unsigned int __gpufreq_get_shader_present(void)
{
	return g_shader_present;
}
EXPORT_SYMBOL(__gpufreq_get_shader_present);

unsigned int __gpufreq_get_cur_fgpu(void)
{
	return g_gpu.cur_freq;
}
EXPORT_SYMBOL(__gpufreq_get_cur_fgpu);

unsigned int __gpufreq_get_cur_vgpu(void)
{
	return g_gpu.buck_count ? g_gpu.cur_volt : 0;
}
EXPORT_SYMBOL(__gpufreq_get_cur_vgpu);

unsigned int __gpufreq_get_cur_vsram(void)
{
	return g_gpu.cur_vsram;
}
EXPORT_SYMBOL(__gpufreq_get_cur_vsram);

int __gpufreq_get_cur_idx_gpu(void)
{
	return g_gpu.cur_oppidx;
}
EXPORT_SYMBOL(__gpufreq_get_cur_idx_gpu);

/* API: get the segment OPP index with the highest performance */
int __gpufreq_get_max_idx_gpu(void)
{
	return g_gpu.max_oppidx;
}
EXPORT_SYMBOL(__gpufreq_get_max_idx_gpu);

/* API: get the segment OPP index with the lowest performance */
int __gpufreq_get_min_idx_gpu(void)
{
	return g_gpu.min_oppidx;
}
EXPORT_SYMBOL(__gpufreq_get_min_idx_gpu);

unsigned int __gpufreq_get_opp_num_gpu(void)
{
	return g_gpu.opp_num;
}
EXPORT_SYMBOL(__gpufreq_get_opp_num_gpu);

unsigned int __gpufreq_get_signed_opp_num_gpu(void)
{
	return g_gpu.signed_opp_num;
}
EXPORT_SYMBOL(__gpufreq_get_signed_opp_num_gpu);

const struct gpufreq_opp_info *__gpufreq_get_working_table_gpu(void)
{
	return g_gpu.working_table;
}
EXPORT_SYMBOL(__gpufreq_get_working_table_gpu);

const struct gpufreq_opp_info *__gpufreq_get_signed_table_gpu(void)
{
	return g_gpu.signed_table;
}
EXPORT_SYMBOL(__gpufreq_get_signed_table_gpu);

const struct gpufreq_sb_info *__gpufreq_get_sb_table_gpu(void)
{
	return g_gpu.sb_table;
}
EXPORT_SYMBOL(__gpufreq_get_sb_table_gpu);

struct gpufreq_debug_opp_info __gpufreq_get_debug_opp_info_gpu(void)
{
	struct gpufreq_debug_opp_info opp_info = {};
	int ret = GPUFREQ_SUCCESS;

	mutex_lock(&gpufreq_lock_gpu);
	opp_info.cur_oppidx = g_gpu.cur_oppidx;
	opp_info.cur_freq = g_gpu.cur_freq;
	opp_info.cur_volt = g_gpu.cur_volt;
	opp_info.cur_vsram = g_gpu.cur_vsram;
	opp_info.power_count = g_gpu.power_count;
	opp_info.cg_count = g_gpu.cg_count;
	opp_info.mtcmos_count = g_gpu.mtcmos_count;
	opp_info.buck_count = g_gpu.buck_count;
	opp_info.segment_id = g_gpu.segment_id;
	opp_info.segment_upbound = g_gpu.segment_upbound;
	opp_info.segment_lowbound = g_gpu.segment_lowbound;
	opp_info.dvfs_state = g_dvfs_state;
	opp_info.shader_present = g_shader_present;
	opp_info.aging_enable = g_aging_enable;
	opp_info.stress_test_enable = g_stress_test_enable;
	mutex_unlock(&gpufreq_lock_gpu);

	ret = __gpufreq_power_control(POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to control power state (p=%d, c=%d, m=%d, b=%d) (%d)",
			POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON, ret);
		goto done;
	}

	mutex_lock(&gpufreq_lock_gpu);
	opp_info.fmeter_freq = __gpufreq_get_fmeter_fgpu();
	opp_info.con1_freq = __gpufreq_get_real_fgpu();
	opp_info.regulator_volt = __gpufreq_get_real_vgpu();
	opp_info.regulator_vsram = __gpufreq_get_real_vsram();
	mutex_unlock(&gpufreq_lock_gpu);

	/* we don't care the result of power off */
	__gpufreq_power_control(POWER_OFF, CG_OFF, MTCMOS_OFF, BUCK_OFF);

done:
	return opp_info;
}
EXPORT_SYMBOL(__gpufreq_get_debug_opp_info_gpu);

/* API: get freq of GPU via OPP index */
unsigned int __gpufreq_get_fgpu_by_idx(int oppidx)
{
	if (oppidx >= 0 && oppidx < g_gpu.opp_num)
		return g_gpu.working_table[oppidx].freq;
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_fgpu_by_idx);

/* API: get volt of GPU via OPP index */
unsigned int __gpufreq_get_vgpu_by_idx(int oppidx)
{
	if (oppidx >= 0 && oppidx < g_gpu.opp_num)
		return g_gpu.working_table[oppidx].volt;
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_vgpu_by_idx);

/* API: get volt of SRAM via OPP index */
unsigned int __gpufreq_get_vsram_by_idx(int oppidx)
{
	if (oppidx >= 0 && oppidx < g_gpu.opp_num)
		return g_gpu.working_table[oppidx].vsram;
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_vsram_by_idx);

/* API: get power of GPU via OPP index */
unsigned int __gpufreq_get_pgpu_by_idx(int oppidx)
{
	if (oppidx >= 0 && oppidx < g_gpu.opp_num)
		return g_gpu.working_table[oppidx].power;
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_pgpu_by_idx);

/* API: get OPP index of GPU via frequency */
int __gpufreq_get_idx_by_fgpu(unsigned int freq)
{
	int idx = 0;

	/* find the smallest index that satisfy given freq */
	for (idx = g_gpu.min_oppidx; idx >= 0; idx--) {
		if (g_gpu.working_table[idx].freq >= freq)
			break;
	}

	/* found */
	if (idx >= 0)
		return idx;
	/* not found */
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_idx_by_fgpu);

/* API: get OPP index of GPU via power */
int __gpufreq_get_idx_by_pgpu(unsigned int power)
{
	int idx = 0;

	/* find the smallest index that satisfy given power */
	for (idx = g_gpu.min_oppidx; idx >= 0; idx--) {
		if (g_gpu.working_table[idx].power >= power)
			break;
	}

	/* found */
	if (idx >= 0)
		return idx;
	/* not found */
	else
		return 0;
}
EXPORT_SYMBOL(__gpufreq_get_idx_by_pgpu);

/* API: get volt of SRAM via volt of GPU */
unsigned int __gpufreq_get_vsram_by_vgpu(unsigned int vgpu)
{
	unsigned int vsram;

	if (vgpu > VSRAM_FIXED_THRESHOLD)
		vsram = vgpu + VSRAM_FIXED_DIFF;
	else
		vsram = VSRAM_FIXED_VOLT;

	return vsram;
}
EXPORT_SYMBOL(__gpufreq_get_vsram_by_vgpu);

/* API: get leakage power of GPU */
unsigned int __gpufreq_get_lkg_pgpu(unsigned int volt)
{
	GPUFREQ_UNREFERENCED(volt);

	return GPU_LEAKAGE_POWER;
}
EXPORT_SYMBOL(__gpufreq_get_lkg_pgpu);

/* API: get dynamic power of GPU */
unsigned int __gpufreq_get_dyn_pgpu(
	unsigned int freq, unsigned int volt)
{
	unsigned int p_dynamic = GPU_ACT_REF_POWER;
	unsigned int ref_freq = GPU_ACT_REF_FREQ;
	unsigned int ref_volt = GPU_ACT_REF_VOLT;

	p_dynamic = p_dynamic *
			((freq * 100) / ref_freq) *
			((volt * 100) / ref_volt) *
			((volt * 100) / ref_volt) /
			(100 * 100 * 100);

	return p_dynamic;
}
EXPORT_SYMBOL(__gpufreq_get_dyn_pgpu);

int __gpufreq_power_control(
	enum gpufreq_power_state power, enum gpufreq_cg_state cg,
	enum gpufreq_mtcmos_state mtcmos, enum gpufreq_buck_state buck)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("power=%d, cg=%d, mtcmos=%d, buck=%d",
		power, cg, mtcmos, buck);

	mutex_lock(&gpufreq_lock_gpu);

	GPUFREQ_LOGD("switch power: %s(%d), cg: %s(%d), mtcmos: %s(%d), buck: %s(%d)",
		power == POWER_OFF ? "off" : "on", g_gpu.power_count,
		cg == CG_OFF ? "off" : "on", g_gpu.cg_count,
		mtcmos == MTCMOS_OFF ? "off" : "on", g_gpu.mtcmos_count,
		buck == BUCK_OFF ? "off" : "on", g_gpu.buck_count);

	if (power == POWER_ON)
		g_gpu.power_count++;
	else {
		g_gpu.power_count--;
		/* todo */
		// check_pending_info();
		// gpu_assert(g_gpu.power_count >= 0, GPU_FREQ_EXCEPTION,
		// "switch power: %d (count: %d), cg: %d, mtcmos: %d, buck: %d",
		// power, g_gpu.power_count, cg, mtcmos, buck);
	}
	__gpufreq_footprint_power_count(g_gpu.power_count);

	if (power == POWER_ON) {
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_1);

		if (buck == BUCK_ON) {
			ret = __gpufreq_buck_control(buck);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to control buck: BUCK_ON (%d)",
					ret);
				g_gpu.power_count--;
				goto done_unlock;
			}
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_2);

		if (mtcmos == MTCMOS_ON) {
			ret = __gpufreq_mtcmos_control(mtcmos);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to control mtcmos: MTCMOS_ON (%d)",
					ret);
				g_gpu.power_count--;
				g_gpu.buck_count--;
				goto done_unlock;
			}
			ret = GPUFREQ_SUCCESS;
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_3);

		if (cg == CG_ON) {
			ret = __gpufreq_cg_control(cg);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to control cg: CG_ON (%d)",
					ret);
				g_gpu.power_count--;
				g_gpu.buck_count--;
				g_gpu.mtcmos_count--;
				goto done_unlock;
			}
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_4);

		if (g_gpu.power_count == 1)
			g_dvfs_state &= ~DVFS_POWEROFF;
	} else {
		if (g_gpu.power_count == 0)
			g_dvfs_state |= DVFS_POWEROFF;

		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_5);

		if (cg == CG_OFF) {
			ret = __gpufreq_cg_control(cg);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to control cg: CG_OFF (%d)",
					ret);
				g_gpu.power_count++;
				goto done_unlock;
			}
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_6);

		if (mtcmos == MTCMOS_OFF) {
			ret = __gpufreq_mtcmos_control(mtcmos);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to control mtcmos: MTCMOS_OFF (%d)",
					ret);
				g_gpu.power_count++;
				g_gpu.cg_count++;
				goto done_unlock;
			}
			ret = GPUFREQ_SUCCESS;
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_7);

		if (buck == BUCK_OFF) {
			ret = __gpufreq_buck_control(buck);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to control buck: BUCK_OFF (%d)",
					ret);
				g_gpu.power_count++;
				g_gpu.cg_count++;
				g_gpu.mtcmos_count++;
				goto done_unlock;
			}
		}
		__gpufreq_footprint_vgpu(GPUFREQ_VGPU_STEP_8);
	}

done_unlock:
	mutex_unlock(&gpufreq_lock_gpu);

	GPUFREQ_TRACE_END();

	return ret;
}
EXPORT_SYMBOL(__gpufreq_power_control);

int __gpufreq_commit_gpu(
	int target_oppidx, enum gpufreq_dvfs_state key)
{
	struct gpufreq_opp_info *opp_table = g_gpu.working_table;
	struct gpufreq_sb_info *sb_table = g_gpu.sb_table;
	unsigned int opp_num = g_gpu.opp_num;
	int cur_oppidx = 0;
	unsigned int cur_freq = 0, target_freq = 0;
	unsigned int cur_volt = 0, target_volt = 0;
	unsigned int cur_vsram = 0, target_vsram = 0;
	int sb_idx = 0;
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("target_oppidx=%d, key=%d",
		target_oppidx, key);

	/* validate 0 <= target_oppidx < opp_num */
	if (target_oppidx < 0 || target_oppidx >= opp_num) {
		GPUFREQ_LOGE("invalid target GPU OPP index: %d (OPP_NUM: %d)",
			target_oppidx, opp_num);
		ret = GPUFREQ_EINVAL;
		goto done;
	}

	mutex_lock(&gpufreq_lock_gpu);

	/* check dvfs state */
	if (g_dvfs_state & ~key) {
		GPUFREQ_LOGI("unavailable dvfs state (0x%x)", g_dvfs_state);
		ret = GPUFREQ_SUCCESS;
		goto done_unlock;
	}

	/* randomly replace target index */
	if (g_stress_test_enable) {
		get_random_bytes(&target_oppidx, sizeof(target_oppidx));
		target_oppidx = target_oppidx < 0 ?
			(target_oppidx*-1) % opp_num : target_oppidx % opp_num;
	}

	cur_oppidx = g_gpu.cur_oppidx;
	cur_freq = g_gpu.cur_freq;
	cur_volt = g_gpu.cur_volt;
	cur_vsram = g_gpu.cur_vsram;

	target_freq = opp_table[target_oppidx].freq;
	target_volt = opp_table[target_oppidx].volt;
	target_vsram = opp_table[target_oppidx].vsram;

	GPUFREQ_LOGD("begin to commit GPU OPP index: (%d->%d)",
		cur_oppidx, target_oppidx);

	/* todo: GED log buffer (gpufreq_pr_logbuf) */

	if (target_freq == cur_freq) {
		/* voltage scaling */
		ret = __gpufreq_volt_scale_gpu(
			cur_volt, target_volt, cur_vsram, target_vsram);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
				cur_volt, target_volt, cur_vsram, target_vsram);
			goto done_unlock;
		}
	} else if (target_freq > cur_freq) {
		/* voltage scaling */
		while (target_volt != cur_volt) {
			sb_idx = target_oppidx > sb_table[cur_oppidx].up ?
				target_oppidx : sb_table[cur_oppidx].up;

			ret = __gpufreq_volt_scale_gpu(
				cur_volt, opp_table[sb_idx].volt,
				cur_vsram, opp_table[sb_idx].vsram);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
					cur_volt, opp_table[sb_idx].volt,
					cur_vsram, opp_table[sb_idx].vsram);
				goto done_unlock;
			}

			cur_oppidx = sb_idx;
			cur_volt = opp_table[sb_idx].volt;
			cur_vsram = opp_table[sb_idx].vsram;
		}
		/* frequency scaling */
		ret = __gpufreq_freq_scale_gpu(cur_freq, target_freq);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Fgpu: (%d->%d)",
				cur_freq, target_freq);
			goto done_unlock;
		}
	} else {
		/* frequency scaling */
		ret = __gpufreq_freq_scale_gpu(cur_freq, target_freq);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Fgpu: (%d->%d)",
				cur_freq, target_freq);
			goto done_unlock;
		}
		/* voltage scaling */
		while (target_volt != cur_volt) {
			sb_idx = target_oppidx < sb_table[cur_oppidx].down ?
				target_oppidx : sb_table[cur_oppidx].down;

			ret = __gpufreq_volt_scale_gpu(
				cur_volt, opp_table[sb_idx].volt,
				cur_vsram, opp_table[sb_idx].vsram);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
					cur_volt, opp_table[sb_idx].volt,
					cur_vsram, opp_table[sb_idx].vsram);
				goto done_unlock;
			}

			cur_oppidx = sb_idx;
			cur_volt = opp_table[sb_idx].volt;
			cur_vsram = opp_table[sb_idx].vsram;
		}
	}

	g_gpu.cur_oppidx = target_oppidx;

	__gpufreq_kick_pbm(BUCK_ON);

done_unlock:
	mutex_unlock(&gpufreq_lock_gpu);

done:
	GPUFREQ_TRACE_END();

	return ret;
}
EXPORT_SYMBOL(__gpufreq_commit_gpu);

int __gpufreq_fix_target_oppidx_gpu(int oppidx)
{
	int ret = GPUFREQ_SUCCESS;

	ret = __gpufreq_power_control(POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to control power state (p=%d, c=%d, m=%d, b=%d) (%d)",
			POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON, ret);
		goto done;
	}

	if (oppidx == GPUFREQ_DBG_DEFAULT_IDX) {
		mutex_lock(&gpufreq_lock_gpu);
		g_dvfs_state &= ~DVFS_DEBUG_KEEP;
		mutex_unlock(&gpufreq_lock_gpu);

		/* we don't care the result of this commit */
		__gpufreq_commit_gpu(g_gpu.min_oppidx, DVFS_DEBUG_KEEP);
	} else if (oppidx >= 0 && oppidx < g_gpu.opp_num) {
		mutex_lock(&gpufreq_lock_gpu);
		g_dvfs_state |= DVFS_DEBUG_KEEP;
		mutex_unlock(&gpufreq_lock_gpu);

		ret = __gpufreq_commit_gpu(oppidx, DVFS_DEBUG_KEEP);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to commit GPU OPP index: %d (%d)",
				oppidx, ret);
			mutex_lock(&gpufreq_lock_gpu);
			g_dvfs_state &= ~DVFS_DEBUG_KEEP;
			mutex_unlock(&gpufreq_lock_gpu);
		}
	} else {
		GPUFREQ_LOGE("invalid fixed OPP index: %d", oppidx);

		ret = GPUFREQ_EINVAL;
	}

	/* we don't care the result of power off */
	__gpufreq_power_control(POWER_OFF, CG_OFF, MTCMOS_OFF, BUCK_OFF);

done:
	return ret;
}
EXPORT_SYMBOL(__gpufreq_fix_target_oppidx_gpu);

int __gpufreq_fix_custom_freq_volt_gpu(
	unsigned int freq, unsigned int volt)
{
	int ret = GPUFREQ_SUCCESS;

	ret = __gpufreq_power_control(POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to control power state (p=%d, c=%d, m=%d, b=%d) (%d)",
			POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON, ret);
		goto done;
	}

	if (freq == GPUFREQ_DBG_DEFAULT_FREQ &&
		volt == GPUFREQ_DBG_DEFAULT_VOLT) {
		mutex_lock(&gpufreq_lock_gpu);
		g_dvfs_state &= ~DVFS_DEBUG_KEEP;
		mutex_unlock(&gpufreq_lock_gpu);

		/* we don't care the result of this commit */
		__gpufreq_commit_gpu(g_gpu.min_oppidx, DVFS_DEBUG_KEEP);
	} else if (freq > POSDIV_2_MAX_FREQ || freq < POSDIV_8_MIN_FREQ) {
		GPUFREQ_LOGE("invalid fixed freq: %d\n", freq);
		ret = GPUFREQ_EINVAL;
	} else if (volt > VGPU_MAX_VOLT || volt < VGPU_MIN_VOLT) {
		GPUFREQ_LOGE("invalid fixed volt: %d\n", volt);
		ret = GPUFREQ_EINVAL;
	} else {
		mutex_lock(&gpufreq_lock_gpu);
		g_dvfs_state |= DVFS_DEBUG_KEEP;
		mutex_unlock(&gpufreq_lock_gpu);

		ret = __gpufreq_custom_commit_gpu(freq, volt, DVFS_DEBUG_KEEP);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to custom commit GPU freq: %d, volt: %d (%d)",
				freq, volt, ret);
			mutex_lock(&gpufreq_lock_gpu);
			g_dvfs_state &= ~DVFS_DEBUG_KEEP;
			mutex_unlock(&gpufreq_lock_gpu);
		}
	}

	/* we don't care the result of power off */
	__gpufreq_power_control(POWER_OFF, CG_OFF, MTCMOS_OFF, BUCK_OFF);

done:
	return ret;
}
EXPORT_SYMBOL(__gpufreq_fix_custom_freq_volt_gpu);

unsigned int __gpufreq_get_cur_fgstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_cur_fgstack);

unsigned int __gpufreq_get_cur_vgstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_cur_vgstack);

int __gpufreq_get_cur_idx_gstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_cur_idx_gstack);

int __gpufreq_get_max_idx_gstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_max_idx_gstack);

int __gpufreq_get_min_idx_gstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_min_idx_gstack);

unsigned int __gpufreq_get_opp_num_gstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_opp_num_gstack);

unsigned int __gpufreq_get_signed_opp_num_gstack(void)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_signed_opp_num_gstack);

const struct gpufreq_opp_info *__gpufreq_get_working_table_gstack(void)
{
	return NULL;
}
EXPORT_SYMBOL(__gpufreq_get_working_table_gstack);

const struct gpufreq_opp_info *__gpufreq_get_signed_table_gstack(void)
{
	return NULL;
}
EXPORT_SYMBOL(__gpufreq_get_signed_table_gstack);

const struct gpufreq_sb_info *__gpufreq_get_sb_table_gstack(void)
{
	return NULL;
}
EXPORT_SYMBOL(__gpufreq_get_sb_table_gstack);

struct gpufreq_debug_opp_info __gpufreq_get_debug_opp_info_gstack(void)
{
	struct gpufreq_debug_opp_info opp_info = {};

	return opp_info;
}
EXPORT_SYMBOL(__gpufreq_get_debug_opp_info_gstack);

/* API: get freq of GPUSTACK via OPP index */
unsigned int __gpufreq_get_fgstack_by_idx(int oppidx)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_fgstack_by_idx);

/* API: get volt of GPUSTACK via OPP index */
unsigned int __gpufreq_get_vgstack_by_idx(int oppidx)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_vgstack_by_idx);

/* API: get power of GPUSTACK via OPP index */
unsigned int __gpufreq_get_pgstack_by_idx(int oppidx)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_pgstack_by_idx);

/* API: get OPP index of GPUSTACK via frequency */
int __gpufreq_get_idx_by_fgstack(unsigned int freq)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_idx_by_fgstack);

/* API: get OPP index of GPUSTACK via power */
int __gpufreq_get_idx_by_pgstack(unsigned int power)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_idx_by_pgstack);

/* API: get leakage power of GPUSTACK */
unsigned int __gpufreq_get_lkg_pgstack(unsigned int volt)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_lkg_pgstack);

/* API: get dynamic power of GPUSTACK */
unsigned int __gpufreq_get_dyn_pgstack(
	unsigned int freq, unsigned int volt)
{
	return 0;
}
EXPORT_SYMBOL(__gpufreq_get_dyn_pgstack);

int __gpufreq_commit_gstack(
	int target_oppidx, enum gpufreq_dvfs_state key)
{
	return GPUFREQ_EINVAL;
}
EXPORT_SYMBOL(__gpufreq_commit_gstack);

int __gpufreq_fix_target_oppidx_gstack(int oppidx)
{
	return GPUFREQ_EINVAL;
}
EXPORT_SYMBOL(__gpufreq_fix_target_oppidx_gstack);

int __gpufreq_fix_custom_freq_volt_gstack(
	unsigned int freq, unsigned int volt)
{
	return GPUFREQ_EINVAL;
}
EXPORT_SYMBOL(__gpufreq_fix_custom_freq_volt_gstack);

void __gpufreq_set_timestamp(void)
{
	/* write 1 into 0x13fb_f130 bit 0 to enable timestamp register */
	/* timestamp will be used by clGetEventProfilingInfo*/
	writel(0x00000001, g_mfg_base + 0x130);
}
EXPORT_SYMBOL(__gpufreq_set_timestamp);

void __gpufreq_check_bus_idle(void)
{
	u32 val;

	/* MFG_QCHANNEL_CON (0x13fb_f0b4) bit [1:0] = 0x1 */
	writel(0x00000001, g_mfg_base + 0xb4);

	/* set register MFG_DEBUG_SEL (0x13fb_f170) bit [7:0] = 0x03 */
	writel(0x00000003, g_mfg_base + 0x170);

	/* polling register MFG_DEBUG_TOP (0x13fb_f178) bit 2 = 0x1 */
	/* => 1 for bus idle, 0 for bus non-idle */
	do {
		val = readl(g_mfg_base + 0x178);
	} while ((val & 0x4) != 0x4);
}
EXPORT_SYMBOL(__gpufreq_check_bus_idle);

void __gpufreq_dump_infra_status(void)
{
	GPUFREQ_LOGI("== [GPUFREQ INFRA STATUS] ==");
	GPUFREQ_LOGI("[%d] freq: %d, vgpu: %d, vsram: %d",
		g_gpu.cur_oppidx, g_gpu.cur_freq,
		g_gpu.cur_volt, g_gpu.cur_vsram);

	// 0x1020E
	if (g_infracfg_base) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1020E810, readl(g_infracfg_base + 0x810));

		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1020E814, readl(g_infracfg_base + 0x814));
	}

	// 0x1021E
	if (g_infra_bpi_bsi_slv0) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1021E230, readl(g_infra_bpi_bsi_slv0 + 0x230));

		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1021E234, readl(g_infra_bpi_bsi_slv0 + 0x234));
	}

	// 0x10023000
	if (g_infra_peri_debug1) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x10023000, readl(g_infra_peri_debug1 + 0x000));

		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x10023440, readl(g_infra_peri_debug1 + 0x440));

		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x10023444, readl(g_infra_peri_debug1 + 0x444));
	}

	// 0x10025000
	if (g_infra_peri_debug2) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x10025000, readl(g_infra_peri_debug2 + 0x000));

		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1002542C, readl(g_infra_peri_debug2 + 0x42C));
	}

	// 0x1002B000
	if (g_infra_peri_debug3) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1002B000, readl(g_infra_peri_debug3 + 0x000));
	}

	// 0x1002E000
	if (g_infra_peri_debug4) {
		GPUFREQ_LOGI("infra status (0x%x): 0x%08x",
			0x1002E000, readl(g_infra_peri_debug4 + 0x000));
	}

	// 0x10006000
	if (g_sleep) {
		GPUFREQ_LOGI("pwr status (0x%x): 0x%08x 0x%08x 0x%08x 0x%08x",
			0x10006000 + 0x308, readl(g_sleep + 0x308),
			readl(g_sleep + 0x30C), readl(g_sleep + 0x310),
			readl(g_sleep + 0x314));

		GPUFREQ_LOGI("pwr status (0x%x) :0x%08x 0x%08x 0x%08x",
			0x10006000 + 0x318, readl(g_sleep + 0x318),
			readl(g_sleep + 0x31C), readl(g_sleep + 0x320));

		GPUFREQ_LOGI("pwr status (0x%x): 0x%08x",
			0x10006000 + 0x16C, readl(g_sleep + 0x16C));

		GPUFREQ_LOGI("pwr status (0x%x): 0x%08x",
			0x10006000 + 0x170, readl(g_sleep + 0x170));
	}
}
EXPORT_SYMBOL(__gpufreq_dump_infra_status);

void __gpufreq_resume_dvfs(void)
{
	GPUFREQ_TRACE_START();

	__gpufreq_set_vgpu_mode(REGULATOR_MODE_NORMAL); /* NORMAL */

	/* we don't care the result of power off */
	__gpufreq_power_control(POWER_OFF, CG_OFF, MTCMOS_OFF, BUCK_OFF);

	mutex_lock(&gpufreq_lock_gpu);
	g_dvfs_state &= ~DVFS_AVS_KEEP;
	mutex_unlock(&gpufreq_lock_gpu);

	GPUFREQ_LOGD("dvfs state: 0x%x", g_dvfs_state);

	GPUFREQ_TRACE_END();
}
EXPORT_SYMBOL(__gpufreq_resume_dvfs);

int __gpufreq_pause_dvfs(void)
{
	unsigned int keep_freq = GPUFREQ_AVS_KEEP_FREQ;
	unsigned int keep_volt = GPUFREQ_AVS_KEEP_VOLT;
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START();

	mutex_lock(&gpufreq_lock_gpu);
	g_dvfs_state |= DVFS_AVS_KEEP;
	mutex_unlock(&gpufreq_lock_gpu);

	ret = __gpufreq_power_control(POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to control power state (p=%d, c=%d, m=%d, b=%d) (%d)",
			POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON, ret);
		goto done;
	}

	ret = __gpufreq_custom_commit_gpu(keep_freq, keep_volt, DVFS_AVS_KEEP);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to custom commit GPU freq: %d, volt: %d (%d)",
			keep_freq, keep_volt, ret);
		mutex_lock(&gpufreq_lock_gpu);
		g_dvfs_state &= ~DVFS_AVS_KEEP;
		mutex_unlock(&gpufreq_lock_gpu);
		goto done;
	}

	__gpufreq_set_vgpu_mode(REGULATOR_MODE_FAST); /* PWM */

	GPUFREQ_LOGD("dvfs state: 0x%x, keep freq: %d, keep volt: %d",
		g_dvfs_state, keep_freq, keep_volt);

done:
	GPUFREQ_TRACE_END();

	return ret;
}
EXPORT_SYMBOL(__gpufreq_pause_dvfs);

/* API: map given AVS idx to real OPP idx */
int __gpufreq_map_avs_idx(int avsidx)
{
	unsigned int avs_num = 0;
	int signed_oppidx = 0;

	avs_num = AVS_NUM;

	if (avsidx >= 0 && avsidx < avs_num) {
		GPUFREQ_LOGD("map AVS to OPP index: (%d->%d)",
			avsidx, g_avs_to_opp[avsidx]);

		signed_oppidx = g_avs_to_opp[avsidx];
	} else {
		GPUFREQ_LOGE("invalid AVS index: %d (AVS_NUM: %d)",
			avsidx, avs_num);
	}

	return signed_oppidx;
}
EXPORT_SYMBOL(__gpufreq_map_avs_idx);

/* API: restore original segment OPP table */
void __gpufreq_restore_opp_gpu(void)
{
	GPUFREQ_TRACE_START();

	/* not support */

	GPUFREQ_TRACE_END();
}
EXPORT_SYMBOL(__gpufreq_restore_opp_gpu);

void __gpufreq_restore_opp_gstack(void)
{
	/* not support */
}
EXPORT_SYMBOL(__gpufreq_restore_opp_gstack);

/*
 * API: adjust volt value of OPP table by given volt array
 * AVS: Adaptive Voltage Scaling
 */
void __gpufreq_adjust_volt_by_avs(
	unsigned int avs_volt[], unsigned int array_size)
{
	GPUFREQ_TRACE_START("avs_volt=0x%x, array_size=%d",
		avs_volt, array_size);

	return;

	GPUFREQ_TRACE_END();
}
EXPORT_SYMBOL(__gpufreq_adjust_volt_by_avs);

int __gpufreq_get_batt_oc_idx(int batt_oc_level)
{
#if (GPUFREQ_BATT_OC_ENABLE && \
	IS_ENABLED(CONFIG_MTK_BATTERY_OC_POWER_THROTTLING))
	if (batt_oc_level == BATTERY_OC_LEVEL_1)
		return GPUFREQ_BATT_OC_IDX - g_gpu.segment_upbound;
	else
		return GPUPPM_RESET_IDX;
#else
	GPUFREQ_UNREFERENCED(batt_oc_level);

	return GPUPPM_KEEP_IDX;
#endif /* GPUFREQ_BATT_OC_ENABLE */
}
EXPORT_SYMBOL(__gpufreq_get_batt_oc_idx);

int __gpufreq_get_batt_percent_idx(int batt_percent_level)
{
#if (GPUFREQ_BATT_PERCENT_ENABLE && \
	IS_ENABLED(CONFIG_MTK_BATTERY_PERCENT_THROTTLING))
	if (batt_percent_level == BATTERY_PERCENT_LEVEL_1)
		return GPUFREQ_BATT_PERCENT_IDX - g_gpu.segment_upbound;
	else
		return GPUPPM_RESET_IDX;
#else
	GPUFREQ_UNREFERENCED(batt_percent_level);

	return GPUPPM_KEEP_IDX;
#endif /* GPUFREQ_BATT_PERCENT_ENABLE */
}
EXPORT_SYMBOL(__gpufreq_get_batt_percent_idx);

int __gpufreq_get_low_batt_idx(int low_batt_level)
{
#if (GPUFREQ_LOW_BATT_ENABLE && \
	IS_ENABLED(CONFIG_MTK_LOW_BATTERY_POWER_THROTTLING))
	if (low_batt_level == LOW_BATTERY_LEVEL_2)
		return GPUFREQ_LOW_BATT_IDX - g_gpu.segment_upbound;
	else
		return GPUPPM_RESET_IDX;
#else
	GPUFREQ_UNREFERENCED(low_batt_level);

	return GPUPPM_KEEP_IDX;
#endif /* GPUFREQ_LOW_BATT_ENABLE */
}
EXPORT_SYMBOL(__gpufreq_get_low_batt_idx);

void __gpufreq_set_stress_test(bool mode)
{
	mutex_lock(&gpufreq_lock_gpu);

	g_stress_test_enable = mode;

	mutex_unlock(&gpufreq_lock_gpu);
}
EXPORT_SYMBOL(__gpufreq_set_stress_test);

int __gpufreq_set_enforced_aging(bool mode)
{
	/* prevent from double aging */
	if (g_aging_enable ^ mode) {
		mutex_lock(&gpufreq_lock_gpu);

		__gpufreq_apply_aging(mode);
		g_aging_enable = mode;

		mutex_unlock(&gpufreq_lock_gpu);

		return GPUFREQ_SUCCESS;
	} else
		return GPUFREQ_EINVAL;
}
EXPORT_SYMBOL(__gpufreq_set_enforced_aging);

/**
 * ===============================================
 * Internal Function Definition
 * ===============================================
 */
static unsigned int __gpufreq_custom_init_enable(void)
{
	return GPUFREQ_CUST_INIT_ENABLE;
}

static unsigned int __gpufreq_dvfs_enable(void)
{
	return GPUFREQ_DVFS_ENABLE;
}

static void __gpufreq_apply_aging(bool apply_aging)
{
	int i = 0;

	GPUFREQ_TRACE_START("apply_aging=%d", apply_aging);

	for (i = 0; i < g_gpu.opp_num; i++) {
		if (apply_aging) {
			g_gpu.working_table[i].volt -=
				g_gpu.working_table[i].vaging;
		} else {
			g_gpu.working_table[i].volt +=
				g_gpu.working_table[i].vaging;
		}

		g_gpu.working_table[i].vsram =
			__gpufreq_get_vsram_by_vgpu(
				g_gpu.working_table[i].volt);

		GPUFREQ_LOGD("apply aging: %d, [%02d] vgpu: %d, vsram: %d",
			apply_aging, i,
			g_gpu.working_table[i].volt,
			g_gpu.working_table[i].vsram);
	}

	__gpufreq_set_springboard();

	GPUFREQ_TRACE_END();
}

static int __gpufreq_custom_commit_gpu(
	unsigned int target_freq, unsigned int target_volt,
	enum gpufreq_dvfs_state key)
{
	unsigned int cur_freq = 0, cur_volt = 0;
	unsigned int cur_vsram = 0, target_vsram = 0;
	unsigned int sb_volt = 0, sb_vsram = 0;
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("target_freq=%d, target_volt=%d, key=%d",
		target_freq, target_volt, key);

	mutex_lock(&gpufreq_lock_gpu);

	/* check dvfs state */
	if (g_dvfs_state & ~key) {
		GPUFREQ_LOGI("unavailable dvfs state (0x%x)", g_dvfs_state);
		ret = GPUFREQ_SUCCESS;
		goto done_unlock;
	}

	cur_freq = g_gpu.cur_freq;
	cur_volt = g_gpu.cur_volt;
	cur_vsram = g_gpu.cur_vsram;

	target_vsram = __gpufreq_get_vsram_by_vgpu(target_volt);

	GPUFREQ_LOGD("begin to custom commit freq: (%d->%d), volt: (%d->%d)",
		cur_freq, target_freq, cur_volt, target_volt);

	if (target_freq == cur_freq) {
		/* voltage scaling */
		ret = __gpufreq_volt_scale_gpu(
			cur_volt, target_volt, cur_vsram, target_vsram);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
				cur_volt, target_volt, cur_vsram, target_vsram);
			goto done_unlock;
		}
	} else if (target_freq > cur_freq) {
		/* voltage scaling */
		while (target_volt != cur_volt) {
			if ((target_vsram - cur_volt) > MAX_BUCK_DIFF) {
				sb_volt = cur_volt + MAX_BUCK_DIFF;
				sb_vsram = __gpufreq_get_vsram_by_vgpu(sb_volt);
			} else {
				sb_volt = target_volt;
				sb_vsram = target_vsram;
			}

			ret = __gpufreq_volt_scale_gpu(
				cur_volt, sb_volt, cur_vsram, sb_vsram);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
					cur_volt, sb_volt, cur_vsram, sb_vsram);
				goto done_unlock;
			}

			cur_volt = sb_volt;
			cur_vsram = sb_vsram;
		}
		/* frequency scaling */
		ret = __gpufreq_freq_scale_gpu(cur_freq, target_freq);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Fgpu: (%d->%d)",
				cur_freq, target_freq);
			goto done_unlock;
		}
	} else {
		/* frequency scaling */
		ret = __gpufreq_freq_scale_gpu(cur_freq, target_freq);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to scale Fgpu: (%d->%d)",
				cur_freq, target_freq);
			goto done_unlock;
		}
		/* voltage scaling */
		while (target_volt != cur_volt) {
			if ((cur_vsram - target_volt) > MAX_BUCK_DIFF) {
				sb_volt = cur_volt - MAX_BUCK_DIFF;
				sb_vsram = __gpufreq_get_vsram_by_vgpu(sb_volt);
			} else {
				sb_volt = target_volt;
				sb_vsram = target_vsram;
			}

			ret = __gpufreq_volt_scale_gpu(
				cur_volt, sb_volt, cur_vsram, sb_vsram);
			if (unlikely(ret)) {
				GPUFREQ_LOGE("fail to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
					cur_volt, sb_volt, cur_vsram, sb_vsram);
				goto done_unlock;
			}

			cur_volt = sb_volt;
			cur_vsram = sb_vsram;
		}
	}

	g_gpu.cur_oppidx = __gpufreq_get_idx_by_fgpu(target_freq);

	__gpufreq_kick_pbm(BUCK_ON);

done_unlock:
	mutex_unlock(&gpufreq_lock_gpu);

	GPUFREQ_TRACE_END();

	return ret;
}

/*
 * API: interpolate OPP of none AVS idx.
 * step = (large - small) / range
 * vnew = large - step * j
 */
static void __gpufreq_interpolate_volt(void)
{
	unsigned int avs_num = 0;
	int front_idx = 0, rear_idx = 0, inner_idx = 0;
	unsigned int large_volt = 0, small_volt = 0;
	unsigned int large_freq = 0, small_freq = 0;
	unsigned int inner_volt = 0, inner_freq = 0;
	unsigned int range = 0;
	int slope = 0;
	int i = 0, j = 0;

	GPUFREQ_TRACE_START();

	avs_num = AVS_NUM;

	for (i = 1; i < avs_num; i++) {
		front_idx = __gpufreq_map_avs_idx(i - 1);
		rear_idx = __gpufreq_map_avs_idx(i);
		range = rear_idx - front_idx;

		/* freq division to amplify slope */
		large_volt = g_gpu.signed_table[front_idx].volt;
		large_freq = g_gpu.signed_table[front_idx].freq / 1000;

		small_volt = g_gpu.signed_table[rear_idx].volt;
		small_freq = g_gpu.signed_table[rear_idx].freq / 1000;

		/* slope = volt / freq */
		slope = (large_volt - small_volt) / (large_freq - small_freq);

		if (unlikely(slope < 0)) {
			dump_stack();
			/* todo: gpu_assert */
		}

		GPUFREQ_LOGD("[%02d*] freq: %d, volt: %d, slope: %d",
			rear_idx, small_freq*1000, small_volt, slope);

		/* start from small v and f, and use (+) instead of (-) */
		for (j = 1; j < range; j++) {
			inner_idx = rear_idx - j;
			inner_freq = g_gpu.signed_table[inner_idx].freq / 1000;
			inner_volt = small_volt +
				slope * (inner_freq - small_freq);
			inner_volt = VOLT_NORMALIZATION(inner_volt);

			/* todo: gpu_assert */

			g_gpu.signed_table[inner_idx].volt = inner_volt;
			g_gpu.signed_table[inner_idx].vsram =
				__gpufreq_get_vsram_by_vgpu(inner_volt);

			GPUFREQ_LOGD("[%02d*] freq: %d, volt: %d, vsram: %d,",
				inner_idx, inner_freq*1000, inner_volt,
				g_gpu.signed_table[inner_idx].vsram);
		}
		GPUFREQ_LOGD("[%02d*] freq: %d, volt: %d",
			front_idx, large_freq*1000, large_volt);
	}

	GPUFREQ_TRACE_END();
}

/*
 * API: set AUTO_MODE or PWM_MODE to VGPU
 * REGULATOR_MODE_FAST: PWM mode
 * REGULATOR_MODE_NORMAL: Auto mode
 */
static void __gpufreq_set_vgpu_mode(unsigned int mode)
{
	int ret = GPUFREQ_SUCCESS;

	if (regulator_is_enabled(g_pmic->reg_vgpu)) {
		ret = regulator_set_mode(g_pmic->reg_vgpu, mode);

		if (unlikely(ret))
			GPUFREQ_LOGE("fail to set regulator mode: %d (%d)",
				mode, ret);
		else
			GPUFREQ_LOGD("set regulator mode: %d, (%d: PWM, %d: AUTO)",
				mode, REGULATOR_MODE_NORMAL,
				REGULATOR_MODE_FAST);
	}
}

static int __gpufreq_switch_clksrc(enum gpufreq_clk_src clksrc)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("clksrc=%d", clksrc);

	ret = clk_prepare_enable(g_clk->clk_mux);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to enable clk_mux(TOP_MUX_MFG) (%d)", ret);
		goto done;
	}

	if (clksrc == CLOCK_MAIN) {
		ret = clk_set_parent(g_clk->clk_mux, g_clk->clk_main_parent);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to switch to CLOCK_MAIN (%d)", ret);
			goto done;
		}
	} else if (clksrc == CLOCK_SUB) {
		ret = clk_set_parent(g_clk->clk_mux, g_clk->clk_sub_parent);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to switch to CLOCK_SUB (%d)", ret);
			goto done;
		}
	} else {
		GPUFREQ_LOGE("invalid clock source: %d (EINVAL)", clksrc);
		goto done;
	}

	clk_disable_unprepare(g_clk->clk_mux);

done:
	GPUFREQ_TRACE_END();

	return ret;
}

/*
 * API: calculate pcw for setting CON1
 * Fin is 26 MHz
 * VCO Frequency = Fin * N_INFO
 * MFGPLL output Frequency = VCO Frequency / POSDIV
 * N_INFO = MFGPLL output Frequency * POSDIV / FIN
 * N_INFO[21:14] = FLOOR(N_INFO, 8)
 */
static unsigned int __gpufreq_calculate_pcw(
	unsigned int freq, enum gpufreq_postdiv postdiv)
{
	/*
	 * MFGPLL VCO range: 1.5GHz - 3.8GHz by divider 1/2/4/8/16,
	 * MFGPLL range: 125MHz - 3.8GHz,
	 * | VCO MAX | VCO MIN | POSDIV | PLL OUT MAX | PLL OUT MIN |
	 * |  3800   |  1500   |    1   |   3800MHz   |   1500MHz   |
	 * |  3800   |  1500   |    2   |   1900MHz   |    750MHz   |
	 * |  3800   |  1500   |    4   |    950MHz   |    375MHz   |
	 * |  3800   |  1500   |    8   |    475MHz   |  187.5MHz   |
	 * |  3800   |  2000   |   16   |  237.5MHz   |    125MHz   |
	 */
	unsigned int pcw = 0;

	if ((freq >= POSDIV_8_MIN_FREQ) && (freq <= POSDIV_4_MAX_FREQ)) {
		pcw = (((freq / TO_MHZ_HEAD * (1 << postdiv)) << DDS_SHIFT)
			/ MFGPLL_FIN + ROUNDING_VALUE) / TO_MHZ_TAIL;
	} else {
		GPUFREQ_LOGE("out of range freq: %d (EINVAL)", freq);
	}

	return pcw;
}

static enum gpufreq_postdiv __gpufreq_get_real_posdiv(void)
{
	unsigned long mfgpll = 0;
	enum gpufreq_postdiv postdiv = POSDIV_POWER_1;

	mfgpll = DRV_Reg32(MFGPLL_CON1);

	postdiv = (mfgpll & (0x7 << POSDIV_SHIFT)) >> POSDIV_SHIFT;

	return postdiv;
}

static enum gpufreq_postdiv __gpufreq_get_posdiv_by_freq(unsigned int freq)
{
	struct gpufreq_opp_info *opp_table = g_gpu.signed_table;
	int i = 0;

	for (i = 0; i < g_gpu.signed_opp_num; i++) {
		if (opp_table[i].freq <= freq)
			return opp_table[i].postdiv;
	}

	GPUFREQ_LOGE("fail to find post divder of freq: %d", freq);

	if (freq > POSDIV_2_MAX_FREQ)
		return POSDIV_POWER_1;
	else if (freq > POSDIV_4_MAX_FREQ)
		return POSDIV_POWER_2;
	else if (freq > POSDIV_8_MAX_FREQ)
		return POSDIV_POWER_4;
	else if (freq > POSDIV_16_MAX_FREQ)
		return POSDIV_POWER_8;
	else
		return POSDIV_POWER_16;
}

static int __gpufreq_freq_scale_gpu(
	unsigned int freq_old, unsigned int freq_new)
{
	enum gpufreq_postdiv cur_posdiv = POSDIV_POWER_1;
	enum gpufreq_postdiv target_posdiv = POSDIV_POWER_1;
	unsigned int pcw = 0;
	unsigned int pll = 0;
	bool parking = false;
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("freq_old=%d, freq_new=%d",
		freq_old, freq_new);

	GPUFREQ_LOGD("begin to scale Fgpu: (%d->%d)", freq_old, freq_new);

	cur_posdiv = __gpufreq_get_real_posdiv();
	target_posdiv = __gpufreq_get_posdiv_by_freq(freq_new);
	pcw = __gpufreq_calculate_pcw(freq_new, target_posdiv);
	if (!pcw) {
		GPUFREQ_LOGE("invalid pcw: %d", pcw);
		goto done;
	}
	pll = (0x80000000) | (target_posdiv << POSDIV_SHIFT) | pcw;

#if IS_ENABLED(CONFIG_MTK_FREQ_HOPPING)
	if (target_posdiv != cur_posdiv)
		parking = true;
	else
		parking = false;
#else
	/* force parking if FHCTL isn't ready */
	parking = true;
#endif /* CONFIG_MTK_FREQ_HOPPING */

	if (parking) {
		ret = __gpufreq_switch_clksrc(CLOCK_SUB);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to switch clock source (%d)", ret);
			goto done;
		}
		/*
		 * MFGPLL_CON1[31:31] = MFGPLL_SDM_PCW_CHG
		 * MFGPLL_CON1[26:24] = MFGPLL_POSDIV
		 * MFGPLL_CON1[21:0]  = MFGPLL_SDM_PCW (pcw)
		 */
		DRV_WriteReg32(MFGPLL_CON1, pll);
		/* PLL spec */
		udelay(20);

		ret = __gpufreq_switch_clksrc(CLOCK_MAIN);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to switch clock source (%d)", ret);
			goto done;
		}
	} else {
#if IS_ENABLED(CONFIG_MTK_FREQ_HOPPING)
		ret = mt_dfs_general_pll(MFGPLL_FH_PLL, pcw);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to hopping pcw: 0x%x (%d)",
				pcw, ret);
			goto done;
		}
#endif
	}

	g_gpu.cur_freq = __gpufreq_get_real_fgpu();

	GPUFREQ_LOGD("Fgpu: %d, pcw: 0x%x, pll: 0x%08x, parking: %d",
		g_gpu.cur_freq, pcw, pll, parking);

	/* todo: GED log buffer (gpufreq_pr_logbuf) */

	/* todo: hopping failed assert */

done:
	GPUFREQ_TRACE_END();

	return ret;
}

static unsigned int __gpufreq_settle_time_vgpu(bool mode, int deltaV)
{
	/* [MT6315][VGPU]
	 * DVFS Rising : delta(V) / 12.5mV + 4us + 5us
	 * DVFS Falling: delta(V) / 5mV + 4us + 5us
	 */
	unsigned int t_settle = 0;

	if (mode) {
		/* rising 12.5mv/us*/
		t_settle = deltaV / (125 * 10) + 9;
	} else {
		/* falling 5mv/us*/
		t_settle = deltaV / (5 * 100) + 9;
	}

	return t_settle; /* us */
}

static unsigned int __gpufreq_settle_time_vsram(bool mode, int deltaV)
{
	/* [MT6359][VSRAM_GPU]
	 * DVFS Rising : delta(V) / 12.5mV + 3us + 5us
	 * DVFS Falling: delta(V) / 5mV + 3us + 5us
	 */
	unsigned int t_settle = 0;

	if (mode) {
		/* rising 12.5mv/us*/
		t_settle = deltaV / (125 * 10) + 8;
	} else {
		/* falling 5mv/us*/
		t_settle = deltaV / (5 * 100) + 8;
	}

	return t_settle; /* us */
}

/*
 * API: scale vgpu and vsram via PMIC
 */
static int __gpufreq_volt_scale_gpu(
	unsigned int vgpu_old, unsigned int vgpu_new,
	unsigned int vsram_old, unsigned int vsram_new)
{
	unsigned int t_settle_vgpu = 0;
	unsigned int t_settle_vsram = 0;
	unsigned int t_settle = 0;
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("vgpu_old=%d, vgpu_new=%d, vsram_old=%d, vsram_new=%d",
		vgpu_old, vgpu_new, vsram_old, vsram_new);

	GPUFREQ_LOGD("begin to scale Vgpu: (%d->%d), Vsram_gpu: (%d->%d)",
		vgpu_old, vgpu_new, vsram_old, vsram_new);

	if (vgpu_new > vgpu_old) {
		/* scale-up volt */
		t_settle_vgpu =
			__gpufreq_settle_time_vgpu(
				true, (vgpu_new - vgpu_old));
		t_settle_vsram =
			__gpufreq_settle_time_vsram(
				true, (vsram_new - vsram_old));

		ret = regulator_set_voltage(
				g_pmic->reg_vsram,
				vsram_new * 10,
				VSRAM_MAX_VOLT * 10 + 125);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to set VSRAM_GPU (%d)", ret);
			goto done;
		}

		ret = regulator_set_voltage(
				g_pmic->reg_vgpu,
				vgpu_new * 10,
				VGPU_MAX_VOLT * 10 + 125);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to set VGPU (%d)", ret);
			goto done;
		}
	} else if (vgpu_new < vgpu_old) {
		/* scale-down volt */
		t_settle_vgpu =
			__gpufreq_settle_time_vgpu(
				false, (vgpu_old - vgpu_new));
		t_settle_vsram =
			__gpufreq_settle_time_vsram(
				false, (vsram_old - vsram_new));

		ret = regulator_set_voltage(
				g_pmic->reg_vgpu,
				vgpu_new * 10,
				VGPU_MAX_VOLT * 10 + 125);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to set VGPU (%d)", ret);
			goto done;
		}

		ret = regulator_set_voltage(
				g_pmic->reg_vsram,
				vsram_new * 10,
				VSRAM_MAX_VOLT * 10 + 125);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to set VSRAM_GPU (%d)", ret);
			goto done;
		}
	} else {
		/* keep volt */
		ret = GPUFREQ_SUCCESS;
	}

	t_settle = (t_settle_vgpu > t_settle_vsram) ?
		t_settle_vgpu : t_settle_vsram;
	udelay(t_settle);

	g_gpu.cur_volt = __gpufreq_get_real_vgpu();
	g_gpu.cur_vsram = __gpufreq_get_real_vsram();

	/* todo: GED log buffer (gpufreq_pr_logbuf) */

	GPUFREQ_LOGD("Vgpu: %d, Vsram_gpu: %d, udelay: %d",
		g_gpu.cur_volt, g_gpu.cur_vsram, t_settle);

done:
	GPUFREQ_TRACE_END();

	return ret;
}

/*
 * API: dump power/clk status when bring-up
 */
static void __gpufreq_dump_bringup_status(void)
{
	/* 0x1000C000 */
	g_apmixed_base =
		__gpufreq_of_ioremap("mediatek,mt6853-apmixedsys", 0);
	if (!g_apmixed_base) {
		GPUFREQ_LOGE("fail to ioremap APMIXED (ENOENT)");
		goto done;
	}

	/* 0x10006000 */
	g_sleep =
		__gpufreq_of_ioremap("mediatek,sleep", 0);
	if (!g_sleep) {
		GPUFREQ_LOGE("fail to ioremap SLEEP (ENOENT)");
		goto done;
	}

	/*
	 * [SPM] pwr_status: pwr_ack (@0x1000_616C)
	 * [SPM] pwr_status_2nd: pwr_ack_2nd (@x1000_6170)
	 * [2]: MFG0, [3]: MFG1, [4]: MFG2, [5]: MFG3, [7]: MFG5
	 */
	GPUFREQ_LOGI("[MFG0-5] PWR_STATUS=0x%08X, PWR_STATUS_2ND=0x%08X",
		readl(g_sleep + 0x16C) & 0x000000BC,
		readl(g_sleep + 0x170) & 0x000000BC);
	GPUFREQ_LOGI("[MFGPLL] FMETER=%d CON1=%d",
		__gpufreq_get_fmeter_fgpu(), __gpufreq_get_real_fgpu());

done:
	return;
}

/*
 * API: set first OPP idx by init config
 */
static void __gpufreq_init_opp_idx(void)
{
	struct gpufreq_opp_info *opp_table = g_gpu.working_table;
	unsigned int cur_freq = 0;
	int oppidx = 0;

	GPUFREQ_TRACE_START();

	cur_freq = __gpufreq_get_real_fgpu();
	GPUFREQ_LOGI("preloader init freq: %d", cur_freq);

	if (__gpufreq_custom_init_enable()) {
		oppidx = GPUFREQ_CUST_INIT_OPPIDX;
		GPUFREQ_LOGI("custom init opp idx: %d, freq: %d",
			oppidx, opp_table[oppidx].freq);
	} else {
		/* Restrict freq to legal opp idx */
		if (cur_freq >= opp_table[0].freq) {
			oppidx = 0;
		} else if (cur_freq <= opp_table[g_gpu.min_oppidx].freq) {
			oppidx = g_gpu.min_oppidx;
		/* Mapping freq to the first smaller opp idx */
		} else {
			for (oppidx = 1; oppidx < g_gpu.opp_num; oppidx++) {
				if (cur_freq >= opp_table[oppidx].freq)
					break;
			}
		}
	}

	g_gpu.cur_oppidx = oppidx;
	g_gpu.cur_freq = cur_freq;
	g_gpu.cur_volt = __gpufreq_get_real_vgpu();
	g_gpu.cur_vsram = __gpufreq_get_real_vsram();

	if (!__gpufreq_dvfs_enable()) {
		g_dvfs_state = DVFS_DISABLE;

		if (__gpufreq_custom_init_enable()) {
			GPUFREQ_LOGI("dvfs state: 0x%x, custom init OPP index: %d",
				g_dvfs_state, oppidx);

			__gpufreq_commit_gpu(oppidx, DVFS_DISABLE);
		} else {
			GPUFREQ_LOGI("dvfs state: 0x%x, disable dvfs",
				g_dvfs_state);
		}
	} else {
		g_dvfs_state = DVFS_FREE;

		GPUFREQ_LOGI("dvfs state: 0x%x, init OPP index: %d",
			g_dvfs_state, oppidx);

		__gpufreq_commit_gpu(oppidx, DVFS_FREE);
	}

	GPUFREQ_TRACE_END();
}

static unsigned int __gpufreq_get_fmeter_fgpu(void)
{
	/* todo: fmeter not ready */
	// return mt_get_abist_freq(FM_MGPLL_CK);
	return 0;
}

/*
 * API: get real current frequency from CON1 (khz)
 * Freq = ((PLL_CON1[21:0] * 26M) / 2^14) / 2^PLL_CON1[26:24]
 */
static unsigned int __gpufreq_get_real_fgpu(void)
{
	unsigned long mfgpll = 0;
	unsigned int posdiv_power = 0;
	unsigned int freq = 0;
	unsigned long pcw = 0;

	mfgpll = DRV_Reg32(MFGPLL_CON1);

	pcw = mfgpll & (0x3FFFFF);

	posdiv_power = (mfgpll & (0x7 << POSDIV_SHIFT)) >> POSDIV_SHIFT;

	freq = (((pcw * TO_MHZ_TAIL + ROUNDING_VALUE) * MFGPLL_FIN) >>
			DDS_SHIFT) / (1 << posdiv_power) * TO_MHZ_HEAD;

	return freq;
}

/*
 * API: get real current vsram from regulator (mV * 100)
 */
static unsigned int __gpufreq_get_real_vsram(void)
{
	unsigned int volt = 0;

	if (regulator_is_enabled(g_pmic->reg_vsram)) {
		/* regulator_get_voltage prints volt with uV */
		volt = regulator_get_voltage(g_pmic->reg_vsram) / 10;
	}

	return volt;
}

/*
 * API: get real current vgpu from regulator (mV * 100)
 */
static unsigned int __gpufreq_get_real_vgpu(void)
{
	unsigned int volt = 0;

	if (regulator_is_enabled(g_pmic->reg_vgpu)) {
		/* regulator_get_voltage prints volt with uV */
		volt = regulator_get_voltage(g_pmic->reg_vgpu) / 10;
	}

	return volt;
}

/*
 * API: kick Power Budget Manager(PBM) when OPP changed
 */
static void __gpufreq_kick_pbm(enum gpufreq_buck_state buck)
{
#if IS_ENABLED(CONFIG_MTK_PBM)
	struct gpufreq_opp_info *opp_table = g_gpu.working_table;
	unsigned int cur_power = 0;
	unsigned int cur_volt = 0;

	cur_volt = __gpufreq_get_real_vgpu();

	if (buck == BUCK_ON) {
		cur_power = opp_table[g_gpu.cur_oppidx].power;

		kicker_pbm_by_gpu(true, cur_power, cur_volt / 100);
	} else
		kicker_pbm_by_gpu(false, 0, cur_volt / 100);
#else
	GPUFREQ_UNREFERENCED(buck);
#endif /* CONFIG_MTK_PBM */
}

static void __gpufreq_external_cg_control(void)
{
	u32 val;

	/* [F] MFG_ASYNC_CON 0x13FB_F020 [22] MEM0_MST_CG_ENABLE = 0x1 */
	/* [J] MFG_ASYNC_CON 0x13FB_F020 [23] MEM0_SLV_CG_ENABLE = 0x1 */
	/* [G] MFG_ASYNC_CON 0x13FB_F020 [24] MEM1_MST_CG_ENABLE = 0x1 */
	/* [K] MFG_ASYNC_CON 0x13FB_F020 [25] MEM1_SLV_CG_ENABLE = 0x1 */
	val = readl(g_mfg_base + 0x20);
	val |= (1UL << 22);
	val |= (1UL << 23);
	val |= (1UL << 24);
	val |= (1UL << 25);
	writel(val, g_mfg_base + 0x20);

	/* [H] MFG_ASYNC_CON_3 0x13FB_F02C [12] MEM0_1_MST_CG_ENABLE = 0x1 */
	/* [L] MFG_ASYNC_CON_3 0x13FB_F02C [13] MEM0_1_SLV_CG_ENABLE = 0x1 */
	/* [I] MFG_ASYNC_CON_3 0x13FB_F02C [14] MEM1_1_MST_CG_ENABLE = 0x1 */
	/* [M] MFG_ASYNC_CON_3 0x13FB_F02C [15] MEM1_1_SLV_CG_ENABLE = 0x1 */
	val = readl(g_mfg_base + 0x2C);
	val |= (1UL << 12);
	val |= (1UL << 13);
	val |= (1UL << 14);
	val |= (1UL << 15);
	writel(val, g_mfg_base + 0x2C);

	/* [D] MFG_GLOBAL_CON 0x13FB_F0B0 [10] GPU_CLK_FREE_RUN = 0x0 */
	/* [D] MFG_GLOBAL_CON 0x13FB_F0B0 [9] MFG_SOC_OUT_AXI_FREE_RUN = 0x0 */
	val = readl(g_mfg_base + 0xB0);
	val &= ~(1UL << 10);
	val &= ~(1UL << 9);
	writel(val, g_mfg_base + 0xB0);

	/* [D] MFG_QCHANNEL_CON 0x13FB_F0B4 [4] QCHANNEL_ENABLE = 0x1 */
	val = readl(g_mfg_base + 0xB4);
	val |= (1UL << 4);
	writel(val, g_mfg_base + 0xB4);

	/* [E] MFG_GLOBAL_CON 0x13FB_F0B0 [19] PWR_CG_FREE_RUN = 0x0 */
	/* [P] MFG_GLOBAL_CON 0x13FB_F0B0 [8] MFG_SOC_IN_AXI_FREE_RUN = 0x0 */
	val = readl(g_mfg_base + 0xB0);
	val &= ~(1UL << 19);
	val &= ~(1UL << 8);
	writel(val, g_mfg_base + 0xB0);

	/*[O] MFG_ASYNC_CON_1 0x13FB_F024 [0] FAXI_CK_SOC_IN_EN_ENABLE = 0x1*/
	val = readl(g_mfg_base + 0x24);
	val |= (1UL << 0);
	writel(val, g_mfg_base + 0x24);
}

static int __gpufreq_cg_control(enum gpufreq_cg_state cg)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("cg=%d", cg);

	if (cg == CG_ON) {
		ret = clk_prepare_enable(g_clk->subsys_mfg_cg);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to enable subsys_mfg_cg (%d)", ret);
			goto done;
		}
		__gpufreq_external_cg_control();

		g_gpu.cg_count++;
	} else {
		clk_disable_unprepare(g_clk->subsys_mfg_cg);

		g_gpu.cg_count--;
	}

done:
	GPUFREQ_TRACE_END();

	return ret;
}

static int __gpufreq_mtcmos_control(enum gpufreq_mtcmos_state mtcmos)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("mtcmos=%d", mtcmos);

	if (mtcmos == MTCMOS_ON) {
		if (g_shader_present & MFG2_SHADER_STACK0) {
			ret = pm_runtime_get_sync(&g_mtcmos->mfg2_pdev->dev);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to enable mtcmos_mfg2 (%d)",
					ret);
				goto done;
			}
		}
		if (g_shader_present & MFG3_SHADER_STACK1) {
			ret = pm_runtime_get_sync(&g_mtcmos->mfg3_pdev->dev);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to enable mtcmos_mfg3 (%d)",
					ret);
				goto done;
			}
		}
		if (g_shader_present & MFG4_SHADER_STACK2) {
			ret = pm_runtime_get_sync(&g_mtcmos->mfg4_pdev->dev);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to enable mtcmos_mfg4 (%d)",
					ret);
				goto done;
			}
		}
		if (g_shader_present & MFG5_SHADER_STACK5) {
			ret = pm_runtime_get_sync(&g_mtcmos->mfg5_pdev->dev);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to enable mtcmos_mfg5 (%d)",
					ret);
				goto done;
			}
		}
		if (g_shader_present & MFG6_SHADER_STACK6) {
			ret = pm_runtime_get_sync(&g_mtcmos->mfg6_pdev->dev);
			if (unlikely(ret < 0)) {
				GPUFREQ_LOGE("fail to enable mtcmos_mfg6 (%d)",
					ret);
				goto done;
			}
		}

		g_gpu.mtcmos_count++;
	} else {
		if (g_shader_present & MFG6_SHADER_STACK6)
			pm_runtime_put_sync(&g_mtcmos->mfg6_pdev->dev);
		if (g_shader_present & MFG5_SHADER_STACK5)
			pm_runtime_put_sync(&g_mtcmos->mfg5_pdev->dev);
		if (g_shader_present & MFG4_SHADER_STACK2)
			pm_runtime_put_sync(&g_mtcmos->mfg4_pdev->dev);
		if (g_shader_present & MFG3_SHADER_STACK1)
			pm_runtime_put_sync(&g_mtcmos->mfg3_pdev->dev);
		if (g_shader_present & MFG2_SHADER_STACK0)
			pm_runtime_put_sync(&g_mtcmos->mfg2_pdev->dev);

		g_gpu.mtcmos_count--;
	}

done:
	GPUFREQ_TRACE_END();

	return ret;
}

static int __gpufreq_buck_control(enum gpufreq_buck_state buck)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("buck=%d", buck);

	if (buck == BUCK_ON) {
		ret = regulator_enable(g_pmic->reg_vsram);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to enable VSRAM_GPU (%d)", ret);
			goto done;
		}
		ret = regulator_enable(g_pmic->reg_vgpu);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to enable VGPU (%d)", ret);
			goto done;
		}

		g_gpu.buck_count++;
	} else {
		ret = regulator_disable(g_pmic->reg_vgpu);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to disable VGPU (%d)", ret);
			goto done;
		}
		ret = regulator_disable(g_pmic->reg_vsram);
		if (unlikely(ret)) {
			GPUFREQ_LOGE("fail to disable VSRAM_GPU (%d)", ret);
			goto done;
		}

		g_gpu.buck_count--;
	}

	__gpufreq_kick_pbm(buck);

done:
	GPUFREQ_TRACE_END();

	return ret;
}

/*
 * API: find the longest and valid opp idx can be reached
 * use springboard opp index to avoid buck variation,
 * as the diff between Vgpu and Vsram must be in valid range
 * that is, MIN_BUCK_DIFF <= Vsram - Vgpu <= MAX_BUCK_DIFF
 * and Vsram always >= Vgpu, so we only focus on "MAX_BUCK_DIFF"
 */
static void __gpufreq_set_springboard(void)
{
	struct gpufreq_opp_info *opp_table = g_gpu.working_table;
	int src_idx = 0, dst_idx = 0;
	unsigned int src_vgpu = 0, src_vsram = 0;
	unsigned int dst_vgpu = 0, dst_vsram = 0;

	/* build volt scale-up springboad table */
	/* when volt scale-up: Vsram -> Vgpu */
	for (src_idx = 0; src_idx < g_gpu.opp_num; src_idx++) {
		src_vgpu = opp_table[src_idx].volt;
		/* search from the beginning of opp table */
		for (dst_idx = 0; dst_idx < g_gpu.opp_num; dst_idx++) {
			dst_vsram = opp_table[dst_idx].vsram;
			/* the smallest valid opp idx can be reached */
			if (dst_vsram - src_vgpu <= MAX_BUCK_DIFF) {
				g_gpu.sb_table[src_idx].up = dst_idx;
				break;
			}
		}
		GPUFREQ_LOGD("springboard_up[%02d]: %d",
			src_idx, g_gpu.sb_table[src_idx].up);
	}

	/* build volt scale-down springboad table */
	/* when volt scale-down: Vgpu -> Vsram */
	for (src_idx = 0; src_idx < g_gpu.opp_num; src_idx++) {
		src_vsram = opp_table[src_idx].vsram;
		/* search from the end of opp table */
		for (dst_idx = g_gpu.min_oppidx; dst_idx >= 0 ; dst_idx--) {
			dst_vgpu = opp_table[dst_idx].volt;
			/* the largest valid opp idx can be reached */
			if (src_vsram - dst_vgpu <= MAX_BUCK_DIFF) {
				g_gpu.sb_table[src_idx].down = dst_idx;
				break;
			}
		}
		GPUFREQ_LOGD("springboard_down[%02d]: %d",
			src_idx, g_gpu.sb_table[src_idx].down);
	}
}

static void __gpufreq_measure_power(void)
{
	unsigned int freq = 0, volt = 0;
	unsigned int p_total = 0, p_dynamic = 0, p_leakage = 0;
	int i = 0;

	GPUFREQ_TRACE_START();

	for (i = 0; i < g_gpu.opp_num; i++) {
		freq = g_gpu.working_table[i].freq;
		volt = g_gpu.working_table[i].volt;

		p_leakage = __gpufreq_get_lkg_pgpu(volt);
		p_dynamic = __gpufreq_get_dyn_pgpu(freq, volt);
		p_total = p_dynamic + p_leakage;

		g_gpu.working_table[i].power = p_total;

		GPUFREQ_LOGD("[%02d] power: %d (dynamic: %d, leakage: %d)",
			i, p_total, p_dynamic, p_leakage);
	}

	GPUFREQ_TRACE_END();
}

static void __gpufreq_adjust_opp_gpu(
	struct gpufreq_adj_info *adj_table, unsigned int adj_num)
{
	int i = 0;
	int oppidx = 0;

	GPUFREQ_TRACE_START("adj_table=0x%x, adj_num=%d",
		adj_table, adj_num);

	if (!adj_table) {
		GPUFREQ_LOGE("null adjustment table (EINVAL)");
		goto done;
	}

	mutex_lock(&gpufreq_lock_gpu);

	for (i = 0; i < adj_num; i++) {
		oppidx = adj_table[i].oppidx;
		if (oppidx >= 0 && oppidx < g_gpu.signed_opp_num) {
			g_gpu.signed_table[oppidx].freq = adj_table[i].freq ?
				adj_table[i].freq :
				g_gpu.signed_table[oppidx].freq;
			g_gpu.signed_table[oppidx].volt = adj_table[i].volt ?
				adj_table[i].volt :
				g_gpu.signed_table[oppidx].volt;
			g_gpu.signed_table[oppidx].vsram = adj_table[i].vsram ?
				adj_table[i].vsram :
				g_gpu.signed_table[oppidx].vsram;
		} else {
			GPUFREQ_LOGE("invalid adj_table[%d].oppidx: %d",
				i, adj_table[i].oppidx);
		}

		GPUFREQ_LOGD("[%02d*] fgpu: %d, vgpu: %d, vsram: %d",
			oppidx, g_gpu.signed_table[oppidx].freq,
			g_gpu.signed_table[oppidx].volt,
			g_gpu.signed_table[oppidx].vsram);
	}

	mutex_unlock(&gpufreq_lock_gpu);

done:
	GPUFREQ_TRACE_END();
}

static void __gpufreq_segment_adjustment(struct platform_device *pdev)
{
	struct gpufreq_adj_info *adj_table;
	unsigned int adj_num = 0;
	unsigned int efuse_id = 0x0;

#if IS_ENABLED(CONFIG_MTK_DEVINFO)
	struct nvmem_cell *efuse_cell;
	unsigned int *efuse_buf;
	size_t efuse_len;

	efuse_cell = nvmem_cell_get(&pdev->dev, "efuse_pod19");
	if (IS_ERR(efuse_cell)) {
		GPUFREQ_LOGE("fail to get efuse_pod19 (%d)",
			PTR_ERR(efuse_cell));
		return;
	}

	efuse_buf = (unsigned int *)nvmem_cell_read(efuse_cell, &efuse_len);
	nvmem_cell_put(efuse_cell);
	if (IS_ERR(efuse_buf)) {
		GPUFREQ_LOGE("fail to get efuse_buf (%d)",
			PTR_ERR(efuse_buf));
		return;
	}

	efuse_id = ((*efuse_buf >> 4) & 0x3);
	kfree(efuse_buf);
#else
	efuse_id = 0x0;
#endif /* CONFIG_MTK_DEVINFO */

	switch (efuse_id) {
	case 0x2:
		adj_table = g_adj_gpu_segment_1;
		adj_num = ADJ_GPU_SEGMENT_1_NUM;
		__gpufreq_adjust_opp_gpu(adj_table, adj_num);
		break;
	default:
		GPUFREQ_LOGW("unknown efuse id: 0x%x", efuse_id);
		break;
	}

	GPUFREQ_LOGI("efuse_id: 0x%x, adj_num: %d", efuse_id, adj_num);
}

static void __gpufreq_init_shader_present(void)
{
	unsigned int segment_id = 0;

	segment_id = g_gpu.segment_id;

	switch (segment_id) {
	case MT6891_SEGMENT:
	case MT6893_SEGMENT:
		g_shader_present = GPU_SHADER_PRESENT_9;
		break;
	default:
		g_shader_present = GPU_SHADER_PRESENT_9;
		GPUFREQ_LOGI("invalid segment id: %d", segment_id);
	}
	GPUFREQ_LOGD("segment_id: %d, shader_present: %d",
		segment_id, g_shader_present);
}

/*
 * 1. init working OPP range
 * 2. init working OPP table = default + adjustment
 * 3. init springboard table
 */
static int __gpufreq_init_opp_table(struct platform_device *pdev)
{
	unsigned int segment_id = 0;
	int i = 0, j = 0;
	int ret = GPUFREQ_SUCCESS;

	/* init working OPP range */
	segment_id = g_gpu.segment_id;
	if (segment_id == MT6891_SEGMENT)
		g_gpu.segment_upbound = 8;
	else if (segment_id == MT6893_SEGMENT)
		g_gpu.segment_upbound = 0;
	else
		g_gpu.segment_upbound = 0;
	g_gpu.segment_lowbound = SIGNED_OPP_NUM_GPU - 1;
	g_gpu.signed_opp_num = SIGNED_OPP_NUM_GPU;

	g_gpu.max_oppidx = 0;
	g_gpu.min_oppidx = g_gpu.segment_lowbound - g_gpu.segment_upbound;
	g_gpu.opp_num = g_gpu.min_oppidx + 1;

	GPUFREQ_LOGD("number of signed OPP: %d, upper and lower bound: [%d, %d]",
		g_gpu.signed_opp_num,
		g_gpu.segment_upbound, g_gpu.segment_lowbound);
	GPUFREQ_LOGI("number of working OPP: %d, max and min OPP index: [%d, %d]",
		g_gpu.opp_num, g_gpu.max_oppidx, g_gpu.min_oppidx);

	/* init working OPP table = default + adjustment */
	g_gpu.signed_table = g_default_gpu_segment;
	/* apply OPP segment adjustment */
	__gpufreq_segment_adjustment(pdev);

	g_gpu.working_table = kcalloc(g_gpu.opp_num,
		sizeof(struct gpufreq_opp_info), GFP_KERNEL);
	if (!g_gpu.working_table) {
		GPUFREQ_LOGE("fail to alloc gpufreq_opp_info (ENOMEM)");
		ret = GPUFREQ_ENOMEM;
		goto done;
	}

	for (i = 0; i < g_gpu.opp_num; i++) {
		j = i + g_gpu.segment_upbound;
		g_gpu.working_table[i].freq = g_gpu.signed_table[j].freq;
		g_gpu.working_table[i].volt = g_gpu.signed_table[j].volt;
		g_gpu.working_table[i].vsram = g_gpu.signed_table[j].vsram;
		g_gpu.working_table[i].postdiv = g_gpu.signed_table[j].postdiv;
		g_gpu.working_table[i].vaging = g_gpu.signed_table[j].vaging;
		g_gpu.working_table[i].power = g_gpu.signed_table[j].power;
		GPUFREQ_LOGD("[%02d] freq: %d, volt: %d, vsram: %d, aging: %d",
			i, g_gpu.working_table[i].freq,
			g_gpu.working_table[i].volt,
			g_gpu.working_table[i].vsram,
			g_gpu.working_table[i].vaging);
	}

	/* set power info to OPP table */
	__gpufreq_measure_power();

	/* init springboard table */
	g_gpu.sb_table = kcalloc(g_gpu.opp_num,
		sizeof(struct gpufreq_sb_info), GFP_KERNEL);
	if (!g_gpu.sb_table) {
		GPUFREQ_LOGE("fail to alloc springboard table (ENOMEM)");
		ret = GPUFREQ_ENOMEM;
		goto done;
	}

	__gpufreq_set_springboard();

done:
	return ret;
}

static void __iomem *__gpufreq_of_ioremap(const char *node_name, int idx)
{
	struct device_node *node;
	void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, node_name);

	if (node)
		base = of_iomap(node, idx);
	else
		base = NULL;

	return base;
}

static int __gpufreq_init_segment_id(struct platform_device *pdev)
{
	unsigned int efuse_id = 0x0;
	unsigned int segment_id = MT6893_SEGMENT;
	int ret = GPUFREQ_SUCCESS;

#if IS_ENABLED(CONFIG_MTK_DEVINFO)
	struct nvmem_cell *efuse_cell;
	unsigned int *efuse_buf;
	size_t efuse_len;

	efuse_cell = nvmem_cell_get(&pdev->dev, "efuse_segment_cell");
	if (IS_ERR(efuse_cell)) {
		GPUFREQ_LOGE("fail to get efuse_segment_cell (%d)",
			PTR_ERR(efuse_cell));
		ret = PTR_ERR(efuse_cell);
		goto done;
	}

	efuse_buf = (unsigned int *)nvmem_cell_read(efuse_cell, &efuse_len);
	nvmem_cell_put(efuse_cell);
	if (IS_ERR(efuse_buf)) {
		GPUFREQ_LOGE("fail to get efuse_buf (%d)",
			PTR_ERR(efuse_buf));
		ret = PTR_ERR(efuse_buf);
		goto done;
	}

	efuse_id = (*efuse_buf & 0xFF);
	kfree(efuse_buf);
#else
	efuse_id = 0x0;
#endif /* CONFIG_MTK_DEVINFO */

	switch (efuse_id) {
	case 0x10:
		segment_id = MT6891_SEGMENT;
		break;
	case 0x40:
		segment_id = MT6893_SEGMENT;
		break;
	default:
		segment_id = MT6893_SEGMENT;
		GPUFREQ_LOGW("unknown efuse id: 0x%x", efuse_id);
		break;
	}

	GPUFREQ_LOGI("efuse_id: 0x%x, segment_id: %d", efuse_id, segment_id);

done:
	g_gpu.segment_id = segment_id;

	return ret;
}

static int __gpufreq_init_clk(struct platform_device *pdev)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("pdev=0x%x", pdev);

	/* MFGPLL is from APMIXED and its parent clock is from XTAL(26MHz); */
	g_apmixed_base =
		__gpufreq_of_ioremap("mediatek,mt6893-apmixedsys", 0);
	if (!g_apmixed_base) {
		GPUFREQ_LOGE("fail to ioremap APMIXED (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_mfg_base =
		__gpufreq_of_ioremap("mediatek,g3d_config", 0);
	if (!g_mfg_base) {
		GPUFREQ_LOGE("fail to ioremap g3d_config (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_clk->clk_mux = devm_clk_get(&pdev->dev, "clk_mux");
	if (IS_ERR(g_clk->clk_mux)) {
		GPUFREQ_LOGE("fail to get clk_mux (%ld)",
			PTR_ERR(g_clk->clk_mux));
		ret = PTR_ERR(g_clk->clk_mux);
		goto done;
	}

	g_clk->clk_main_parent = devm_clk_get(&pdev->dev, "clk_main_parent");
	if (IS_ERR(g_clk->clk_main_parent)) {
		GPUFREQ_LOGE("fail to get clk_main_parent (%ld)",
			PTR_ERR(g_clk->clk_main_parent));
		ret = PTR_ERR(g_clk->clk_main_parent);
		goto done;
	}

	g_clk->clk_sub_parent = devm_clk_get(&pdev->dev, "clk_sub_parent");
	if (IS_ERR(g_clk->clk_sub_parent)) {
		GPUFREQ_LOGE("fail to get clk_sub_parent (%ld)",
			PTR_ERR(g_clk->clk_sub_parent));
		ret = PTR_ERR(g_clk->clk_sub_parent);
		goto done;
	}

	g_clk->subsys_mfg_cg = devm_clk_get(&pdev->dev, "subsys_mfg_cg");
	if (IS_ERR(g_clk->subsys_mfg_cg)) {
		GPUFREQ_LOGE("fail to get subsys_mfg_cg (%ld)",
			PTR_ERR(g_clk->subsys_mfg_cg));
		ret = PTR_ERR(g_clk->subsys_mfg_cg);
		goto done;
	}

	g_sleep =
		__gpufreq_of_ioremap("mediatek,sleep", 0);
	if (!g_sleep) {
		GPUFREQ_LOGE("fail to ioremap sleep (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

#if defined(GPUFREQ_TODO)
	g_infracfg_base =
		__gpufreq_of_ioremap("mediatek,infracfg", 0);
	if (!g_infracfg_base) {
		GPUFREQ_LOGE("fail to ioremap infracfg (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_infra_bpi_bsi_slv0 =
		__gpufreq_of_ioremap("mediatek,bpi_bsi_slv0", 0);
	if (!g_infra_bpi_bsi_slv0) {
		GPUFREQ_LOGE("fail to ioremap bpi_bsi_slv0 (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_infra_peri_debug1 = __gpufreq_of_ioremap(
		"mediatek,devapc_ao_infra_peri_debug1", 0);
	if (!g_infra_peri_debug1) {
		GPUFREQ_LOGE("fail to ioremap devapc_ao_infra_peri_debug1 (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_infra_peri_debug2 = __gpufreq_of_ioremap(
		"mediatek,devapc_ao_infra_peri_debug2", 0);
	if (!g_infra_peri_debug2) {
		GPUFREQ_LOGE("fail to ioremap devapc_ao_infra_peri_debug2 (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_infra_peri_debug3 = __gpufreq_of_ioremap(
		"mediatek,devapc_ao_infra_peri_debug3", 0);
	if (!g_infra_peri_debug3) {
		GPUFREQ_LOGE("fail to ioremap devapc_ao_infra_peri_debug3 (ENOENT)",);
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	g_infra_peri_debug4 = __gpufreq_of_ioremap(
		"mediatek,devapc_ao_infra_peri_debug4", 0);
	if (!g_infra_peri_debug4) {
		GPUFREQ_LOGE("fail to ioremap devapc_ao_infra_peri_debug4 (ENOENT)");
		ret = GPUFREQ_ENOENT;
		goto done;
	}
#endif /* GPUFREQ_TODO */

done:
	GPUFREQ_TRACE_END();

	return ret;
}

static int __gpufreq_init_pmic(struct platform_device *pdev)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_TRACE_START("pdev=0x%x", pdev);

	g_pmic->reg_vgpu =
		regulator_get_optional(&pdev->dev, "_vgpu");
	if (IS_ERR(g_pmic->reg_vgpu)) {
		GPUFREQ_LOGE("fail to get VGPU (%ld)",
			PTR_ERR(g_pmic->reg_vgpu));
		ret = PTR_ERR(g_pmic->reg_vgpu);
		goto done;
	}

	g_pmic->reg_vsram =
		regulator_get_optional(&pdev->dev, "_vsram_gpu");
	if (IS_ERR(g_pmic->reg_vsram)) {
		GPUFREQ_LOGE("fail to get VSRAM_GPU (%ld)",
			PTR_ERR(g_pmic->reg_vsram));
		ret = PTR_ERR(g_pmic->reg_vsram);
		goto done;
	}

done:
	GPUFREQ_TRACE_END();

	return ret;
}

/*
 * API: gpufreq driver probe
 */
static int __gpufreq_pdrv_probe(struct platform_device *pdev)
{
	struct device_node *node;
	int ret = GPUFREQ_SUCCESS;

	if (__gpufreq_bringup()) {
		GPUFREQ_LOGI("skip gpufreq driver probe when bringup");
		goto done;
	}

	GPUFREQ_LOGI("start to probe gpufreq driver");

	node = of_find_matching_node(NULL, g_gpufreq_of_match);
	if (!node) {
		GPUFREQ_LOGE("fail to find gpufreq node");
		ret = GPUFREQ_ENOENT;
		goto done;
	}

	/* init pmic regulator */
	ret = __gpufreq_init_pmic(pdev);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init pmic (%d)", ret);
		goto done;
	}

	/* init clock source */
	ret = __gpufreq_init_clk(pdev);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init clk (%d)", ret);
		goto done;
	}

	/* init segment id */
	ret = __gpufreq_init_segment_id(pdev);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init segment id (%d)", ret);
		goto done;
	}

	/* init opp table */
	ret = __gpufreq_init_opp_table(pdev);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init opp table (%d)", ret);
		goto done;
	}

	/* init shader present */
	__gpufreq_init_shader_present();

	if (!__gpufreq_power_ctrl_enable()) {
		GPUFREQ_LOGI("power control always on");
		ret = __gpufreq_power_control(
			POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON);
		if (unlikely(ret))
			GPUFREQ_LOGE("fail to control power state (p=%d, c=%d, m=%d, b=%d) (%d)",
				POWER_ON, CG_ON, MTCMOS_ON, BUCK_ON, ret);
	}

#if defined(GPUFREQ_AGING_LOAD)
	GPUFREQ_LOGI("aging load");
	g_aging_enable = true;
	__gpufreq_apply_aging(true);
#endif /* GPUFREQ_AGING_LOAD */

	/* init opp index by bootup freq */
	__gpufreq_init_opp_idx();

#if IS_ENABLED(CONFIG_MTK_STATIC_POWER)
	/* Initial leackage power usage */
	mt_spower_init();
#endif /* CONFIG_MTK_STATIC_POWER */

	g_probe_done = true;
	GPUFREQ_LOGI("gpufreq driver probe done");

done:
	return ret;
}

static int __gpufreq_mfg2_probe(struct platform_device *pdev)
{
	if (!pdev->dev.pm_domain) {
		GPUFREQ_LOGE("fail to get mfg2 pm_domain");
		return -EPROBE_DEFER;
	}

	g_mtcmos->mfg2_pdev = pdev;
	pm_runtime_enable(&pdev->dev);
	dev_pm_syscore_device(&pdev->dev, true);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mfg3_probe(struct platform_device *pdev)
{
	if (!pdev->dev.pm_domain) {
		GPUFREQ_LOGE("fail to get mfg3 pm_domain");
		return -EPROBE_DEFER;
	}

	g_mtcmos->mfg3_pdev = pdev;
	pm_runtime_enable(&pdev->dev);
	dev_pm_syscore_device(&pdev->dev, true);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mfg4_probe(struct platform_device *pdev)
{
	if (!pdev->dev.pm_domain) {
		GPUFREQ_LOGE("fail to get mfg4 pm_domain");
		return -EPROBE_DEFER;
	}

	g_mtcmos->mfg4_pdev = pdev;
	pm_runtime_enable(&pdev->dev);
	dev_pm_syscore_device(&pdev->dev, true);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mfg5_probe(struct platform_device *pdev)
{
	if (!pdev->dev.pm_domain) {
		GPUFREQ_LOGE("fail to get mfg5 pm_domain");
		return -EPROBE_DEFER;
	}

	g_mtcmos->mfg5_pdev = pdev;
	pm_runtime_enable(&pdev->dev);
	dev_pm_syscore_device(&pdev->dev, true);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mfg6_probe(struct platform_device *pdev)
{
	if (!pdev->dev.pm_domain) {
		GPUFREQ_LOGE("fail to get mfg6 pm_domain");
		return -EPROBE_DEFER;
	}

	g_mtcmos->mfg6_pdev = pdev;
	pm_runtime_enable(&pdev->dev);
	dev_pm_syscore_device(&pdev->dev, true);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mfg_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);

	return GPUFREQ_SUCCESS;
}

static int __gpufreq_mtcmos_pdrv_probe(struct platform_device *pdev)
{
	const struct gpufreq_mfg_fp *mfg_fp;
	int ret = GPUFREQ_SUCCESS;

	if (__gpufreq_bringup()) {
		GPUFREQ_LOGI("skip gpufreq mtcmos probe when bringup");
		goto done;
	}

	mfg_fp = of_device_get_match_data(&pdev->dev);
	if (!mfg_fp) {
		GPUFREQ_LOGE("fail to get mtcmos match data");
		ret = GPUFREQ_EINVAL;
		goto done;
	}

	ret = mfg_fp->probe(pdev);
	if (unlikely(ret))
		GPUFREQ_LOGE("fail to probe mtcmos device");

done:
	return ret;
}

static int __gpufreq_mtcmos_pdrv_remove(struct platform_device *pdev)
{
	const struct gpufreq_mfg_fp *mfg_fp;
	int ret = GPUFREQ_SUCCESS;

	if (__gpufreq_bringup()) {
		GPUFREQ_LOGI("skip gpufreq mtcmos remove when bringup");
		goto done;
	}

	mfg_fp = of_device_get_match_data(&pdev->dev);
	if (!mfg_fp) {
		GPUFREQ_LOGE("fail to get mtcmos match data");
		ret = GPUFREQ_EINVAL;
		goto done;
	}

	ret = mfg_fp->remove(pdev);
	if (unlikely(ret))
		GPUFREQ_LOGE("fail to remove mtcmos device");

done:
	return ret;
}

/*
 * API: register gpufreq platform driver
 */
static int __init __gpufreq_init(void)
{
	int ret = GPUFREQ_SUCCESS;

	GPUFREQ_LOGI("start to init gpufreq platform driver");

	g_mtcmos = kzalloc(sizeof(struct gpufreq_mtcmos_info), GFP_KERNEL);
	if (!g_mtcmos) {
		GPUFREQ_LOGE("fail to alloc gpufreq_mtcmos_info (ENOMEM)");
		ret = GPUFREQ_ENOMEM;
		goto done;
	}

	g_pmic = kzalloc(sizeof(struct gpufreq_pmic_info), GFP_KERNEL);
	if (!g_pmic) {
		GPUFREQ_LOGE("fail to alloc gpufreq_pmic_info (ENOMEM)");
		ret = GPUFREQ_ENOMEM;
		goto done;
	}

	g_clk = kzalloc(sizeof(struct gpufreq_clk_info), GFP_KERNEL);
	if (!g_clk) {
		GPUFREQ_LOGE("fail to alloc gpufreq_clk_info (ENOMEM)");
		ret = GPUFREQ_ENOMEM;
		goto done;
	}

	/* register gpufreq mtcmos driver */
	ret = platform_driver_register(&g_gpufreq_mtcmos_pdrv);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to register gpufreq mtcmos driver\n");
		goto done;
	}

	/* register gpufreq platform driver */
	ret = platform_driver_register(&g_gpufreq_pdrv);
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to register gpufreq platform driver (%d)",
			ret);
		goto done;
	}

	if (__gpufreq_bringup()) {
		__gpufreq_dump_bringup_status();
		GPUFREQ_LOGI("skip the rest of gpufreq init when bringup");
		goto done;
	}

	/* init gpu ppm */
	ret = gpuppm_init();
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init gpuppm (%d)", ret);
		goto done;
	}

	/* init gpufreq wrapper */
	ret = gpufreq_wrapper_init();
	if (unlikely(ret)) {
		GPUFREQ_LOGE("fail to init gpufreq wrapper driver (%d)", ret);
		goto done;
	}

	__gpufreq_footprint_vgpu_reset();
	__gpufreq_footprint_oppidx_reset();
	__gpufreq_footprint_power_count_reset();

	GPUFREQ_LOGI("gpufreq platform driver init done");

done:
	return ret;
}

/*
 * API: unregister gpufreq driver
 */
static void __exit __gpufreq_exit(void)
{
	kfree(g_gpu.working_table);
	kfree(g_gpu.sb_table);
	kfree(g_clk);
	kfree(g_pmic);
	kfree(g_mtcmos);

	platform_driver_unregister(&g_gpufreq_pdrv);
	platform_driver_unregister(&g_gpufreq_mtcmos_pdrv);
}

module_init(__gpufreq_init);
module_exit(__gpufreq_exit);

MODULE_DEVICE_TABLE(of, g_gpufreq_of_match);
MODULE_DEVICE_TABLE(of, g_gpufreq_mtcmos_of_match);
MODULE_DESCRIPTION("MediaTek GPU-DVFS platform driver");
MODULE_LICENSE("GPL");
