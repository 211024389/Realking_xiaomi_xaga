// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/slab.h>

#include "mdw_cmn.h"

int mdw_dev_init(struct mdw_device *mdev)
{
	int ret = -ENODEV;

	mdw_drv_info("mdw dev init type(%d-%u)\n",
		mdev->driver_type, mdev->mdw_ver);

	switch (mdev->driver_type) {
	case MDW_DRIVER_TYPE_PLATFORM:
		mdw_ap_set_func(mdev);
		break;
	case MDW_DRIVER_TYPE_RPMSG:
		mdw_rv_set_func(mdev);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (mdev->dev_funcs)
		ret = mdev->dev_funcs->late_init(mdev);

	return ret;
}

void mdw_dev_deinit(struct mdw_device *mdev)
{
	if (mdev->dev_funcs) {
		mdev->dev_funcs->late_deinit(mdev);
		mdev->dev_funcs = NULL;
	}
}
