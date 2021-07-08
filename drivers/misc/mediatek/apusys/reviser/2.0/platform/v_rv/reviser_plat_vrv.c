// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/platform_device.h>

#include "reviser_drv.h"
#include "reviser_cmn.h"
#include "reviser_plat.h"

#include "reviser_hw_vrv.h"
#include "reviser_hw_mgt.h"
#include "reviser_hw_cmn.h"

int reviser_vrv_init(struct platform_device *pdev)
{
	struct reviser_dev_info *rdv = platform_get_drvdata(pdev);
	struct reviser_hw_ops *hw_cb;

	hw_cb = (struct reviser_hw_ops *) reviser_hw_mgt_get_cb();

	hw_cb->dmp_boundary = reviser_print_rvr_boundary;
	hw_cb->dmp_ctx = reviser_print_rvr_context_ID;
	hw_cb->dmp_rmp = reviser_print_rvr_remap_table;
	hw_cb->dmp_default = reviser_print_rvr_default_iova;
	hw_cb->dmp_exception = reviser_print_rvr_exception;


	//Set TCM Info
	rdv->plat.pool_type[0] = 2;
	rdv->plat.pool_base[0] = 0;
	rdv->plat.pool_step[0] = 1;
	//Set DRAM fallback
	rdv->plat.dram[0] = 0x4000000;

	//Set remap max
	rdv->plat.rmp_max = 0xD;
	//Set ctx max
	rdv->plat.ctx_max = 32;

	rdv->plat.hw_ver = 0x100;

	return 0;
}
int reviser_vrv_uninit(struct platform_device *pdev)
{
	return 0;
}
