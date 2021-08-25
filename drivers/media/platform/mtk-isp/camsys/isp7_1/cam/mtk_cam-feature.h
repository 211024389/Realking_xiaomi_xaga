/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_CAM_FEATURE_H
#define __MTK_CAM_FEATURE_H

#include "mtk_cam.h"
#include "mtk_cam-raw.h"

static inline bool mtk_cam_feature_is_mstream(int feature)
{
	int raw_feature = feature & MTK_CAM_FEATURE_HDR_MASK;

	if (raw_feature == MSTREAM_NE_SE || raw_feature == MSTREAM_SE_NE)
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_time_shared(int feature)
{
	if (feature & MTK_CAM_FEATURE_TIMESHARE_MASK)
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_stagger_m2m(int feature)
{
	if (feature & MTK_CAM_FEATURE_STAGGER_M2M_MASK)
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_stagger(int feature)
{
	int is_hdr;

	is_hdr = feature & MTK_CAM_FEATURE_HDR_MASK;
	if (is_hdr && is_hdr >= STAGGER_2_EXPOSURE_LE_SE &&
	    is_hdr <= STAGGER_3_EXPOSURE_SE_NE_LE)
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_subsample(int feature)
{
	if (feature & MTK_CAM_FEATURE_SUBSAMPLE_MASK)
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_2_exposure(int feature)
{
	if (feature == STAGGER_2_EXPOSURE_LE_SE ||
	    feature == STAGGER_2_EXPOSURE_SE_LE ||
	    feature == STAGGER_M2M_2_EXPOSURE_LE_SE ||
	    feature == STAGGER_M2M_2_EXPOSURE_SE_LE ||
	    mtk_cam_feature_is_mstream(feature))
		return true;

	return false;
}

static inline bool mtk_cam_feature_is_3_exposure(int feature)
{
	if (feature == STAGGER_3_EXPOSURE_LE_NE_SE ||
	    feature == STAGGER_3_EXPOSURE_SE_NE_LE ||
	    feature == STAGGER_M2M_3_EXPOSURE_LE_NE_SE ||
	    feature == STAGGER_M2M_3_EXPOSURE_SE_NE_LE)
		return true;

	return false;
}

static inline bool mtk_cam_feature_change_is_mstream(int feature_change)
{
	if (feature_change & MSTREAM_EXPOSURE_CHANGE)
		return true;

	return false;
}

bool mtk_cam_is_time_shared(struct mtk_cam_ctx *ctx);
bool mtk_cam_is_stagger(struct mtk_cam_ctx *ctx);
bool mtk_cam_is_stagger_m2m(struct mtk_cam_ctx *ctx);
bool mtk_cam_is_mstream(struct mtk_cam_ctx *ctx);
bool mtk_cam_feature_is_mstream(int feature);
bool mtk_cam_feature_change_is_mstream(int feature_change);
bool mtk_cam_is_subsample(struct mtk_cam_ctx *ctx);
bool mtk_cam_is_2_exposure(struct mtk_cam_ctx *ctx);
bool mtk_cam_is_3_exposure(struct mtk_cam_ctx *ctx);
int mtk_cam_get_sensor_exposure_num(u32 raw_feature);
int mtk_cam_get_feature_switch(struct mtk_raw_pipeline *raw_pipe,
			       int prev);
#endif /*__MTK_CAM_FEATURE_H */
