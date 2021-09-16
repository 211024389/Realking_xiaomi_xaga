/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_REVISER_EXPORT_H__
#define __APUSYS_REVISER_EXPORT_H__
#include <linux/types.h>

extern struct reviser_dev_info *g_rdv;

int reviser_get_vlm(uint32_t request_size, bool force,
		unsigned long *ctx, uint32_t *tcm_size);
int reviser_free_vlm(uint32_t ctx);
int reviser_set_context(int type,
		int index, uint8_t ctx);
int reviser_get_resource_vlm(uint32_t *addr, uint32_t *size);
int reviser_set_manual_vlm(uint32_t session, uint32_t size);
int reviser_clear_manual_vlm(uint32_t session);
int reviser_alloc_pool(uint32_t type, uint64_t session, uint32_t size, uint32_t *sid);
int reviser_free_pool(uint64_t session, uint32_t sid, uint32_t type);
int reviser_get_pool_size(uint32_t type, uint32_t *size);
int reviser_alloc_external(uint32_t addr, uint32_t size, uint64_t session, uint32_t *sid);
int reviser_free_external(uint64_t session, uint32_t sid);
int reviser_import_external(uint64_t session, uint32_t sid);
int reviser_unimport_external(uint64_t session, uint32_t sid);
#endif
