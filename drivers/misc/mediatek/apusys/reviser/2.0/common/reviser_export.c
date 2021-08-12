// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include "apusys_device.h"
#include "reviser_cmn.h"
#include "reviser_export.h"
#include "reviser_table_mgt.h"
#include "reviser_hw_mgt.h"
#include "reviser_drv.h"
#include "reviser_remote_cmd.h"


/**
 * reviser_get_vlm - get continuous memory which is consists of TCM/DRAM/System-Memory
 * @request_size: the request size of the memory
 * @force: use tcm and block function until get it
 * @ctx: the id of continuous memory
 * @tcm_size: the real TCM size of continuous memory
 *
 * This function creates contiguous memory from TCM/DRAM/System-Memory.
 */
int reviser_get_vlm(uint32_t request_size, bool force,
		unsigned long *ctx, uint32_t *tcm_size)
{
	int ret = 0;

	DEBUG_TAG;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	if (!reviser_table_get_vlm(g_rdv,
			request_size, force,
			ctx, tcm_size)) {
		LOG_DBG_RVR_VLM("request(0x%x) force(%d) ctx(%lu) tcm_size(0x%x)\n",
				request_size, force, *ctx, *tcm_size);
	} else {
		ret = -EINVAL;
	}

	return ret;
}
/**
 * reviser_free_vlm - free continuous memory which is consists of TCM/DRAM/System-Memory
 * @ctx: the id of continuous memory
 *
 * This function free contiguous memory by id.
 */
int reviser_free_vlm(uint32_t ctx)
{
	int ret = 0;

	DEBUG_TAG;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	if (reviser_table_free_vlm(g_rdv, ctx)) {
		LOG_ERR("Free VLM Fail: ctx: %d\n", ctx);
		ret = -EINVAL;
		return ret;
	}

	LOG_DBG_RVR_VLM("ctx(%d)\n", ctx);
	return ret;
}

/**
 * reviser_set_context - set context id for specific hardware
 * @type: the hardware type
 * @index: the index of specific hardware
 * @ctx: the id of continuous memory
 *
 * This function set context id for specific hardware for using continuous memory.
 */
int reviser_set_context(int type,
		int index, uint8_t ctx)
{
	int ret = 0;

	DEBUG_TAG;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}


	if (reviser_mgt_set_ctx(g_rdv,
			type, index, ctx)) {
		LOG_ERR("Set reviser Ctx Fail\n");
		ret = -EINVAL;
		return ret;
	}



	LOG_DBG_RVR_VLM("type/index/ctx(%d/%d/%d)\n", type, index, ctx);

	return ret;
}

/**
 * reviser_get_resource_vlm - get vlm address and available TCM size
 * @addr: the address of specific hardware (VLM)
 * @size: the size of specific hardware (TCM)
 *
 * This function get vlm address and size from dts.
 */

int reviser_get_resource_vlm(uint32_t *addr, uint32_t *size)
{
	int ret = 0;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	*addr = (uint32_t) g_rdv->plat.vlm_addr;
	*size = (uint32_t) g_rdv->plat.vlm_size;

	LOG_DBG_RVR_VLM("VLM addr(0x%x) size(0x%x)core\n", *addr, *size);

	return 0;
}

int reviser_set_manual_vlm(uint32_t session, uint32_t size)
{
	int ret = 0;

	return ret;
}

int reviser_clear_manual_vlm(uint32_t session)
{
	int ret = 0;

	return ret;
}

int reviser_alloc_pool(uint32_t type, uint64_t session, uint32_t size, uint32_t *sid)
{
	int ret = 0;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	ret = reviser_remote_alloc_mem(g_rdv, type, size, session, sid);
	if (ret)
		LOG_ERR("Remote Handshake fail %d\n", ret);

	return ret;
}

int reviser_free_pool(uint64_t session, uint32_t sid, uint32_t type)
{
	int ret = 0;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	ret = reviser_remote_free_mem(g_rdv, session, sid, type);
	if (ret)
		LOG_ERR("Remote Handshake fail %d\n", ret);

	return 0;
}

int reviser_get_pool_size(uint32_t type, uint32_t *size)
{
	int ret = 0;
	uint32_t ret_size = 0;

	if (g_rdv == NULL) {
		LOG_ERR("Invalid reviser_device\n");
		ret = -EINVAL;
		return ret;
	}

	ret = reviser_remote_get_mem_info(g_rdv, type);
	if (ret)
		LOG_ERR("Remote Handshake fail %d\n", ret);

	switch (type) {
	case REVISER_MEM_TYPE_TCM:
		ret_size = g_rdv->plat.pool_size[REVSIER_POOL_TCM];
		break;
	case REVISER_MEM_TYPE_SLBS:
		ret_size = g_rdv->plat.pool_size[REVSIER_POOL_SLBS];
		break;
	default:
		LOG_ERR("Invalid type\n", type);
		ret = -EINVAL;
		break;
	}

	*size = ret_size;


	return ret;
}



