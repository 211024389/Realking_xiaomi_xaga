// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Tiffany Lin <tiffany.lin@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <soc/mediatek/smi.h>

#include "mtk_vcodec_enc_pm.h"
#include "mtk_vcodec_enc_pm_plat.h"
#include "mtk_vcodec_util.h"
#include "mtk_vcu.h"
//#include "smi_public.h"

#include <mtk_iommu.h>

#ifdef CONFIG_MTK_PSEUDO_M4U
#include <mach/mt_iommu.h>
#include "mach/pseudo_m4u.h"
#include "smi_port.h"
#endif

void mtk_venc_init_ctx_pm(struct mtk_vcodec_ctx *ctx)
{
	ctx->sram_data.uid = UID_MM_VENC;
	ctx->sram_data.type = TP_BUFFER;
	ctx->sram_data.size = 0;
	ctx->sram_data.flag = FG_POWER;

	if (slbc_request(&ctx->sram_data) >= 0)
		ctx->use_slbc = 1;
	else
		ctx->use_slbc = 0;

	pr_debug("slbc_request %d, %p\n", &ctx->sram_data, ctx->use_slbc);
}

int mtk_vcodec_init_enc_pm(struct mtk_vcodec_dev *mtkdev)
{
	int ret = 0;
#ifndef FPGA_PWRCLK_API_DISABLE
	struct device_node *node;
	struct platform_device *pdev;
	struct device *dev;
	struct mtk_vcodec_pm *pm;
	int larb_index;
	int clk_id = 0;
	const char *clk_name;
	struct mtk_venc_clks_data *clks_data;

	pdev = mtkdev->plat_dev;
	pm = &mtkdev->pm;
	memset(pm, 0, sizeof(struct mtk_vcodec_pm));
	pm->mtkdev = mtkdev;
	pm->dev = &pdev->dev;
	clks_data = &pm->venc_clks_data;
	dev = &pdev->dev;

	node = of_parse_phandle(dev->of_node, "mediatek,larbs", 0);
	if (!node) {
		mtk_v4l2_err("no mediatek,larb found");
		return -1;
	}
	for (larb_index = 0; larb_index < MTK_VENC_MAX_LARB_COUNT; larb_index++) {
		node = of_parse_phandle(pdev->dev.of_node, "mediatek,larbs", larb_index);
		if (!node)
			break;

		pdev = of_find_device_by_node(node);
		if (WARN_ON(!pdev)) {
			of_node_put(node);
			return -1;
		}
		pm->larbvencs[larb_index] = &pdev->dev;
		mtk_v4l2_debug(8, "larbvencs[%d] = %p", larb_index, pm->larbvencs[larb_index]);
		pdev = mtkdev->plat_dev;
	}

	memset(clks_data, 0x00, sizeof(struct mtk_venc_clks_data));
	while (!of_property_read_string_index(
			pdev->dev.of_node, "clock-names", clk_id, &clk_name)) {
		mtk_v4l2_debug(8, "init clock, id: %d, name: %s", clk_id, clk_name);
		pm->venc_clks[clk_id] = devm_clk_get(&pdev->dev, clk_name);
		if (IS_ERR(pm->venc_clks[clk_id])) {
			mtk_v4l2_err(
				"[VCODEC][ERROR] Unable to devm_clk_get id: %d, name: %s\n",
				clk_id, clk_name);
			return PTR_ERR(pm->venc_clks[clk_id]);
		}
		clks_data->core_clks[clks_data->core_clks_len].clk_id = clk_id;
		clks_data->core_clks[clks_data->core_clks_len].clk_name = clk_name;
		clks_data->core_clks_len++;
		clk_id++;
	}

	pm_runtime_enable(&pdev->dev);
#endif
	return ret;
}

void mtk_vcodec_release_enc_pm(struct mtk_vcodec_dev *mtkdev)
{
#if ENC_EMI_BW
	/* do nothing */
#endif
	pm_runtime_disable(mtkdev->pm.dev);
}

void mtk_venc_deinit_ctx_pm(struct mtk_vcodec_ctx *ctx)
{
	if (ctx->use_slbc == 1) {
		pr_debug("slbc_release, %p\n", &ctx->sram_data);
		slbc_release(&ctx->sram_data);
	}
}

void mtk_vcodec_enc_clock_on(struct mtk_vcodec_ctx *ctx, int core_id)
{
	struct mtk_vcodec_pm *pm = &ctx->dev->pm;
	int ret;
	int i, larb_port_num, larb_id;
#ifdef CONFIG_MTK_PSEUDO_M4U
	struct M4U_PORT_STRUCT port;
#endif
	int larb_index;
	int j, clk_id;
	struct mtk_venc_clks_data *clks_data;
	struct mtk_vcodec_dev *dev = NULL;

	dev = ctx->dev;

#ifndef FPGA_PWRCLK_API_DISABLE
	time_check_start(MTK_FMT_ENC, core_id);

	clks_data = &pm->venc_clks_data;

	for (larb_index = 0; larb_index < MTK_VENC_MAX_LARB_COUNT; larb_index++) {
		if (pm->larbvencs[larb_index]) {
			ret = mtk_smi_larb_get(pm->larbvencs[larb_index]);
			if (ret)
				mtk_v4l2_err("Failed to get venc larb. index: %d, core_id: %d",
					larb_index, core_id);
		}
	}


	if (core_id == MTK_VENC_CORE_0 ||
		core_id == MTK_VENC_CORE_1) {
			// enable core clocks
		for (j = 0; j < clks_data->core_clks_len; j++) {
			clk_id = clks_data->core_clks[j].clk_id;
			ret = clk_prepare_enable(pm->venc_clks[clk_id]);
			if (ret) {
				mtk_v4l2_err("clk_prepare_enable id: %d, name: %s fail %d",
					clk_id, clks_data->core_clks[j].clk_name, ret);
			}
		}
	} else {
		mtk_v4l2_err("invalid core_id %d", core_id);
		time_check_end(MTK_FMT_ENC, core_id, 50);
		return;
	}
	time_check_end(MTK_FMT_ENC, core_id, 50);
#endif
	if (ctx->use_slbc == 1) {
		time_check_start(MTK_FMT_ENC, core_id);
		ret = slbc_power_on(&ctx->sram_data);
		time_check_end(MTK_FMT_ENC, core_id, 50);
	}

	time_check_start(MTK_FMT_ENC, core_id);
	if (core_id == MTK_VENC_CORE_0) {
		larb_port_num = dev->venc_ports[0].total_port_num;
		larb_id = 7;
	} else {
		larb_port_num = dev->venc_ports[1].total_port_num;
		larb_id = 8;
	}

	//enable slbc port configs
	if (pm->larbvencs[core_id]) {
		for (i = 0; i < larb_port_num; i++) {
			if (dev->venc_ports[core_id].ram_type[i] == 1) {
				ret = smi_sysram_enable(pm->larbvencs[core_id],
					MTK_M4U_ID(larb_id, i), true, "LARB_VENC");

				if (ret)
					mtk_v4l2_err("%#x smi_sysram_enable err: %#x\n",
						i, ret);

			}
		}
	}

	time_check_end(MTK_FMT_ENC, core_id, 50);

#ifdef CONFIG_MTK_PSEUDO_M4U
	time_check_start(MTK_FMT_ENC, core_id);
	if (core_id == MTK_VENC_CORE_0) {
		larb_port_num = SMI_LARB7_PORT_NUM;
		larb_id = 7;
	}

	//enable 34bits port configs
	for (i = 0; i < larb_port_num; i++) {
		port.ePortID = MTK_M4U_ID(larb_id, i);
		port.Direction = 0;
		port.Distance = 1;
		port.domain = 0;
		port.Security = 0;
		port.Virtuality = 1;
		m4u_config_port(&port);
	}
	time_check_end(MTK_FMT_ENC, core_id, 50);
#endif
}

void mtk_vcodec_enc_clock_off(struct mtk_vcodec_ctx *ctx, int core_id)
{
	struct mtk_vcodec_pm *pm = &ctx->dev->pm;
	int i, clk_id;
	int larb_index;
	struct mtk_venc_clks_data *clks_data;

	if (ctx->use_slbc == 1)
		slbc_power_off(&ctx->sram_data);

#ifndef FPGA_PWRCLK_API_DISABLE
	clks_data = &pm->venc_clks_data;

	if (core_id == MTK_VENC_CORE_0 ||
		core_id == MTK_VENC_CORE_1) {
		if (clks_data->core_clks_len > 0) {
			for (i = clks_data->core_clks_len - 1; i >= 0; i--) {
				clk_id = clks_data->core_clks[i].clk_id;
				clk_disable_unprepare(pm->venc_clks[clk_id]);
			}
		}
	} else
		mtk_v4l2_err("invalid core_id %d", core_id);
	for (larb_index = 0; larb_index < MTK_VENC_MAX_LARB_COUNT; larb_index++) {
		if (pm->larbvencs[larb_index])
			mtk_smi_larb_put(pm->larbvencs[larb_index]);
	}
#endif
}
