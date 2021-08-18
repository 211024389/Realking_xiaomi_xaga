// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include "mdw_cmn.h"
#include "mdw_rv.h"
#include "mdw_rv_tag.h"

static int mdw_rv_sw_init(struct mdw_device *mdev)
{
	int ret = 0, i = 0;
	struct mdw_rv_dev *rdev = (struct mdw_rv_dev *)mdev->dev_specific;
	struct mdw_dinfo *d = NULL;

	/* update device info */
	for (i = 0; i < MDW_DEV_MAX; i++) {
		if (!test_bit(i, rdev->dev_mask) || mdev->dinfos[i])
			continue;

		/* setup mdev's info */
		d = kvzalloc(sizeof(*d), GFP_KERNEL);
		if (!d)
			goto free_dinfo;

		d->num = rdev->dev_num[i];
		d->type = i;

		/* meta data */
		memcpy(d->meta, &rdev->meta_data[i][0], sizeof(d->meta));
		mdw_drv_debug("dev(%u) support (%u)core\n", d->type, d->num);

		mdev->dinfos[i] = d;
		bitmap_set(mdev->dev_mask, i, 1);
	}

	goto out;

free_dinfo:
	for (i = 0; i < MDW_DEV_MAX; i++) {
		if (mdev->dinfos[i] != NULL) {
			kvfree(mdev->dinfos[i]);
			mdev->dinfos[i] = NULL;
		}
	}
	ret = -ENOMEM;
out:
	return 0;
}

static void mdw_rv_sw_deinit(struct mdw_device *mdev)
{
	unsigned int i = 0;

	for (i = 0; i < MDW_DEV_MAX; i++) {
		if (mdev->dinfos[i] != NULL) {
			kvfree(mdev->dinfos[i]);
			mdev->dinfos[i] = NULL;
		}
	}
}

static int mdw_rv_late_init(struct mdw_device *mdev)
{
	int ret = 0;

	mdw_rv_tag_init();

	/* get vlm property */
	ret = mdw_rvs_get_vlm_property(&mdev->vlm_start, &mdev->vlm_size);
	if (ret)
		mdw_drv_warn("vlm wrong\n");

	/* init rv device */
	ret = mdw_rv_dev_init(mdev);
	if (ret || !mdev->dev_specific) {
		mdw_drv_err("init mdw rvdev fail(%d)\n", ret);
		goto dev_deinit;
	}

	goto out;

dev_deinit:
	mdw_rv_dev_deinit(mdev);
out:
	return ret;
}

static void mdw_rv_late_deinit(struct mdw_device *mdev)
{
	mdw_rv_dev_deinit(mdev);
	mdw_rv_tag_deinit();
}

static int mdw_rv_run_cmd(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	return mdw_rv_dev_run_cmd(mpriv, c);
}

static int mdw_rv_set_power(struct mdw_device *mdev,
	uint32_t type, uint32_t idx, uint32_t boost)
{
	return -EINVAL;
}

static int mdw_rv_ucmd(struct mdw_device *mdev,
	uint32_t type, void *vaddr, uint32_t size)
{
	return -EINVAL;
}

static int mdw_rv_lock(struct mdw_device *mdev)
{
	return 0;
}

static int mdw_rv_unlock(struct mdw_device *mdev)
{
	return 0;
}

static int mdw_rv_set_param(struct mdw_device *mdev,
	enum mdw_info_type type, uint32_t val)
{
	struct mdw_rv_dev *mrdev = (struct mdw_rv_dev *)mdev->dev_specific;

	return mdw_rv_dev_set_param(mrdev, type, val);
}

static uint32_t mdw_rv_get_info(struct mdw_device *mdev,
	enum mdw_info_type type)
{
	struct mdw_rv_dev *mrdev = (struct mdw_rv_dev *)mdev->dev_specific;

	return mdw_rv_dev_get_param(mrdev, type);
}

static const struct mdw_dev_func mdw_rv_func = {
	.sw_init = mdw_rv_sw_init,
	.sw_deinit = mdw_rv_sw_deinit,
	.late_init = mdw_rv_late_init,
	.late_deinit = mdw_rv_late_deinit,
	.run_cmd = mdw_rv_run_cmd,
	.set_power = mdw_rv_set_power,
	.ucmd = mdw_rv_ucmd,
	.lock = mdw_rv_lock,
	.unlock = mdw_rv_unlock,
	.set_param = mdw_rv_set_param,
	.get_info = mdw_rv_get_info,
};

void mdw_rv_set_func(struct mdw_device *mdev)
{
	mdev->dev_funcs = &mdw_rv_func;
	mdev->uapi_ver = 2;
}
