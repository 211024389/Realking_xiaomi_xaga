// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/list.h>
#include <linux/of.h>

#include <media/v4l2-event.h>
#include <media/v4l2-subdev.h>

#include "mtk_cam.h"
#include "mtk_cam-feature.h"
#include "mtk_cam-ctrl.h"
#include "mtk_cam-debug.h"
#include "mtk_cam-dvfs_qos.h"
#include "mtk_cam-pool.h"
#include "mtk_cam-raw.h"
#include "mtk_cam-regs.h"
#include "mtk_cam-sv-regs.h"
#include "mtk_cam-tg-flash.h"
#include "mtk_camera-v4l2-controls.h"
#include "mtk_camera-videodev2.h"
#include "imgsys/mtk_imgsys-cmdq-ext.h"

#include "frame_sync_camsys.h"

#define SENSOR_SET_DEADLINE_MS  18
#define SENSOR_SET_RESERVED_MS  7
#define STATE_NUM_AT_SOF 3
#define INITIAL_DROP_FRAME_CNT 1

enum MTK_CAMSYS_STATE_RESULT {
	STATE_RESULT_TRIGGER_CQ = 0,
	STATE_RESULT_PASS_CQ_INIT,
	STATE_RESULT_PASS_CQ_SW_DELAY,
	STATE_RESULT_PASS_CQ_SCQ_DELAY,
	STATE_RESULT_PASS_CQ_HW_DELAY,
};

#define v4l2_set_frame_interval_which(x, y) (x.reserved[0] = y)

static void state_transition(struct mtk_camsys_ctrl_state *state_entry,
			     enum MTK_CAMSYS_STATE_IDX from,
			     enum MTK_CAMSYS_STATE_IDX to)
{
	if (state_entry->estate == from)
		state_entry->estate = to;
}

static void mtk_cam_event_eos(struct mtk_raw_pipeline *pipeline)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_EOS,
	};
	v4l2_event_queue(pipeline->subdev.devnode, &event);
}

static void mtk_cam_event_frame_sync(struct mtk_raw_pipeline *pipeline,
				     unsigned int frame_seq_no)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
		.u.frame_sync.frame_sequence = frame_seq_no,
	};
	v4l2_event_queue(pipeline->subdev.devnode, &event);
}

static void mtk_cam_sv_event_frame_sync(struct mtk_camsv_device *camsv_dev,
				unsigned int frame_seq_no)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
		.u.frame_sync.frame_sequence = frame_seq_no,
	};
	v4l2_event_queue(camsv_dev->pipeline->subdev.devnode, &event);
}

static void mtk_cam_event_request_drained(struct mtk_raw_pipeline *pipeline)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_REQUEST_DRAINED,
	};
	v4l2_event_queue(pipeline->subdev.devnode, &event);
}

static void mtk_cam_sv_event_request_drained(struct mtk_camsv_device *camsv_dev)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_REQUEST_DRAINED,
	};
	v4l2_event_queue(camsv_dev->pipeline->subdev.devnode, &event);
}

static bool mtk_cam_request_drained(struct mtk_camsys_sensor_ctrl *sensor_ctrl)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request *req, *req_prev;
	struct mtk_cam_request_stream_data *req_stream_data;
	unsigned long flags;
	int sensor_seq_no_next = sensor_ctrl->sensor_request_seq_no + 1;
	int res = 0;

	spin_lock_irqsave(&cam->running_job_lock, flags);
	list_for_each_entry_safe(req, req_prev, &cam->running_job_list, list) {
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		/* Match by the en-queued request number */
		if (req->ctx_used & (1 << ctx->stream_id) && req_stream_data)
			res = 1;
	}
	spin_unlock_irqrestore(&cam->running_job_lock, flags);
	/* Send V4L2_EVENT_REQUEST_DRAINED event */
	if (res == 0) {
		mtk_cam_event_request_drained(ctx->pipe);
		dev_dbg(ctx->cam->dev, "request_drained:(%d)\n",
			sensor_seq_no_next);
	}
	return (res == 0);
}

static void mtk_cam_sv_request_drained(struct mtk_camsv_device *camsv_dev,
		struct mtk_camsys_sensor_ctrl *sensor_ctrl)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request *req, *req_prev;
	struct mtk_cam_request_stream_data *req_stream_data;
	unsigned long flags;
	int sensor_seq_no_next = sensor_ctrl->sensor_request_seq_no + 1;
	int res = 0;

	spin_lock_irqsave(&cam->running_job_lock, flags);
	list_for_each_entry_safe(req, req_prev, &cam->running_job_list, list) {
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		/* Match by the en-queued request number */
		if (req->ctx_used & (1 << ctx->stream_id) &&
			req_stream_data->frame_seq_no == sensor_seq_no_next)
			res = 1;
	}
	spin_unlock_irqrestore(&cam->running_job_lock, flags);
	/* Send V4L2_EVENT_REQUEST_DRAINED event */
	if (res == 0) {
		mtk_cam_sv_event_request_drained(camsv_dev);
		dev_dbg(camsv_dev->dev, "request_drained:(%d)\n", sensor_seq_no_next);
	}
}

static bool mtk_cam_req_frame_sync_start(struct mtk_cam_request *req)
{
	/* All ctx with sensor is in ready state */
	struct mtk_cam_device *cam =
		container_of(req->req.mdev, struct mtk_cam_device, media_dev);
	struct mtk_cam_ctx *ctx;
	int i;
	int ctx_cnt = 0;
	struct mtk_cam_ctx *sync_ctx[MTKCAM_SUBDEV_MAX];

	for (i = 0; i < cam->max_stream_num; i++) {
		if (!(1 << i & req->ctx_used))
			continue;

		ctx = &cam->ctxs[i];
		if (!ctx->sensor) {
			dev_info(cam->dev, "%s: ctx(%d): no sensor found\n",
				 __func__, ctx->stream_id);
			continue;
		}

		sync_ctx[ctx_cnt] = ctx;
		ctx_cnt++;
	}

	mutex_lock(&req->fs_op_lock);
	if (ctx_cnt > 1) {
		if (req->fs_on_cnt) { /* not first time */
			req->fs_on_cnt++;
			mutex_unlock(&req->fs_op_lock);
			return false;
		}
		req->fs_on_cnt++;
		for (i = 0; i < ctx_cnt; i++) {
			if (!sync_ctx[i]->synced) {
				struct v4l2_ctrl *ctrl = NULL;
				/**
				 * Use V4L2_CID_FRAME_SYNC to group sensors
				 * to be frame sync.
				 */
				ctx = sync_ctx[i];
				ctrl = v4l2_ctrl_find(ctx->sensor->ctrl_handler,
						      V4L2_CID_FRAME_SYNC);
				if (ctrl) {
					v4l2_ctrl_s_ctrl(ctrl, 1);
					ctx->synced = 1;
					dev_info(cam->dev,
						 "%s: ctx(%d): apply V4L2_CID_FRAME_SYNC(1)\n",
						 __func__, ctx->stream_id);
				} else {
					dev_info(cam->dev,
						 "%s: ctx(%d): failed to find V4L2_CID_FRAME_SYNC\n",
						 __func__, ctx->stream_id);
				}
			} else {
				dev_dbg(cam->dev,
					"%s: ctx(%d): skip V4L2_CID_FRAME_SYNC (already applied)\n",
					__func__, ctx->stream_id);
			}
		}

		dev_info(cam->dev, "%s:%s:fs_sync_frame(1): sync %d ctxs: 0x%x\n",
			__func__, req->req.debug_str, ctx_cnt, req->ctx_used);

		fs_sync_frame(1);

		mutex_unlock(&req->fs_op_lock);
		return true;
	}
	mutex_unlock(&req->fs_op_lock);
	return false;
}

static bool mtk_cam_req_frame_sync_end(struct mtk_cam_request *req)
{
	/* All ctx with sensor is not in ready state */
	struct mtk_cam_device *cam =
		container_of(req->req.mdev, struct mtk_cam_device, media_dev);
	struct mtk_cam_ctx *ctx;
	int i;
	int ctx_cnt = 0;

	for (i = 0; i < cam->max_stream_num; i++) {
		if (!(1 << i & req->ctx_used))
			continue;

		ctx = &cam->ctxs[i];
		if (!ctx->sensor) {
			dev_info(cam->dev, "%s: ctx(%d): no sensor found\n",
				 __func__, ctx->stream_id);
			continue;
		}

		ctx_cnt++;
	}

	mutex_lock(&req->fs_op_lock);
	if (ctx_cnt > 1 && req->fs_on_cnt) { /* check fs on */
		req->fs_on_cnt--;
		if (req->fs_on_cnt) { /* not the last */
			mutex_unlock(&req->fs_op_lock);
			return false;
		}
		dev_info(cam->dev,
			 "%s:%s:fs_sync_frame(0): sync %d ctxs: 0x%x\n",
			 __func__, req->req.debug_str, ctx_cnt, req->ctx_used);

		fs_sync_frame(0);

		mutex_unlock(&req->fs_op_lock);
		return true;
	}
	mutex_unlock(&req->fs_op_lock);
	return false;
}


/**
 *  Not used now since mtk_cam_seninf_streaming_mux_change
 *  caused seninf workqueue corruption.
 */
void mtk_cam_req_seninf_change_new(struct mtk_cam_request *req)
{
	struct media_pipeline *m_pipe;
	struct mtk_cam_ctx *ctx;
	struct mtk_cam_device *cam =
		container_of(req->req.mdev, struct mtk_cam_device, media_dev);
	struct mtk_cam_request_stream_data *s_data;
	struct mtk_cam_seninf_mux_param param;
	struct mtk_cam_seninf_mux_setting settings[4];
	struct mtk_raw_device *raw_dev;
	int i, stream_id, setting_cnt = 0, val;

	dev_info(cam->dev, "%s, req->ctx_used:0x%x, req->ctx_link_update:0x%x\n",
		__func__, req->ctx_used, req->ctx_link_update);

	for (i = 0; i < cam->max_stream_num; i++) {
		if (req->ctx_used & (1 << i) && (req->ctx_link_update & (1 << i))) {
			stream_id = i;
			ctx = &cam->ctxs[stream_id];
			raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
			s_data = mtk_cam_req_get_s_data(req, stream_id, 0);

			dev_info(cam->dev, "%s: pipe(%d): switch seninf: %s--> %s\n",
				 __func__, stream_id, s_data->seninf_old->name,
				 s_data->seninf_new->name);

			val = readl(raw_dev->base + REG_TG_VF_CON);
			writel(val & (~TG_VFDATA_EN), raw_dev->base + REG_TG_VF_CON);

			settings[setting_cnt].seninf = s_data->seninf_old;
			settings[setting_cnt].source = PAD_SRC_RAW0;
			settings[setting_cnt].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[setting_cnt].enable  = 0;
			setting_cnt++;

			settings[setting_cnt].seninf = s_data->seninf_new;
			settings[setting_cnt].source = PAD_SRC_RAW0;
			settings[setting_cnt].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[setting_cnt].enable  = 1;
			setting_cnt++;

			m_pipe = s_data->seninf_new->entity.pipe;
			s_data->seninf_new->entity.stream_count++;
			s_data->seninf_new->entity.pipe =
				s_data->seninf_old->entity.pipe;
			s_data->seninf_old->entity.stream_count--;
			s_data->seninf_old->entity.pipe = m_pipe;

			mtk_cam_seninf_set_pixelmode(s_data->seninf_new,
						     PAD_SRC_RAW0,
						     ctx->pipe->res_config.tgo_pxl_mode);

			dev_info(cam->dev, "%s: pipe(%d): update BW for %s\n",
				 __func__, stream_id, s_data->seninf_new->name);
			mtk_cam_qos_bw_calc(ctx);
		}
	}

	dev_info(cam->dev, "%s: update DVFS\n",	 __func__);
	mtk_cam_dvfs_update_clk(cam);

	param.settings = &settings[0];
	param.num = setting_cnt;
	mtk_cam_seninf_streaming_mux_change(&param);

	for (i = 0; i < cam->max_stream_num; i++) {
		if (req->ctx_used & (1 << i) && req->ctx_link_update & (1 << i)) {
			stream_id = i;
			ctx = &cam->ctxs[stream_id];
			s_data = mtk_cam_req_get_s_data(req, stream_id, 0);
			raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);

			state_transition(&s_data->state,
				E_STATE_CAMMUX_OUTER_CFG_DELAY, E_STATE_INNER);
			state_transition(&s_data->state,
			E_STATE_SENINF, E_STATE_READY);

			val = readl(raw_dev->base + REG_TG_VF_CON);
			writel(val | TG_VFDATA_EN, raw_dev->base + REG_TG_VF_CON);

			if (ctx->prev_sensor || ctx->prev_seninf) {
				ctx->prev_sensor = NULL;
				ctx->prev_seninf = NULL;
			}
		}
	}
	dev_dbg(cam->dev, "%s: cam mux switch done\n", __func__);
}

void mtk_cam_req_seninf_change(struct mtk_cam_request *req)
{
	struct media_pipeline *m_pipe;
	struct mtk_cam_ctx *ctx;
	struct mtk_cam_device *cam =
		container_of(req->req.mdev, struct mtk_cam_device, media_dev);
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_raw_device *raw_dev;
	int i, stream_id, val;

	dev_info(cam->dev, "%s, req->ctx_used:0x%x, req->ctx_link_update:0x%x\n",
		__func__, req->ctx_used, req->ctx_link_update);

	for (i = 0; i < cam->max_stream_num; i++) {
		if (req->ctx_used & (1 << i) && (req->ctx_link_update & (1 << i))) {
			stream_id = i;
			ctx = &cam->ctxs[stream_id];
			raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
			req_stream_data = mtk_cam_req_get_s_data(req, stream_id, 0);

			dev_info(cam->dev, "%s: pipe(%d): switch seninf: %s--> %s\n",
				 __func__, stream_id, req_stream_data->seninf_old->name,
				 req_stream_data->seninf_new->name);

			val = readl(raw_dev->base + REG_TG_VF_CON);
			writel(val & (~TG_VFDATA_EN), raw_dev->base + REG_TG_VF_CON);

			m_pipe = req_stream_data->seninf_new->entity.pipe;
			req_stream_data->seninf_new->entity.stream_count++;
			req_stream_data->seninf_new->entity.pipe =
				req_stream_data->seninf_old->entity.pipe;
			req_stream_data->seninf_old->entity.stream_count--;
			req_stream_data->seninf_old->entity.pipe = m_pipe;

			mtk_cam_seninf_set_pixelmode(req_stream_data->seninf_new,
						     PAD_SRC_RAW0,
						     ctx->pipe->res_config.tgo_pxl_mode);

			dev_info(cam->dev,
				 "%s: pipe(%d): seninf_set_camtg, pad(%d) camtg(%d)",
				 __func__, stream_id, PAD_SRC_RAW0,
				 PipeIDtoTGIDX(raw_dev->id));
			mtk_cam_seninf_set_camtg(req_stream_data->seninf_new,
						 PAD_SRC_RAW0,
						 PipeIDtoTGIDX(raw_dev->id));

			dev_info(cam->dev, "%s: pipe(%d): update BW for %s\n",
				 __func__, stream_id, req_stream_data->seninf_new->name);
			mtk_cam_qos_bw_calc(ctx);
		}
	}

	dev_info(cam->dev, "%s: update DVFS\n",	 __func__);
	mtk_cam_dvfs_update_clk(cam);

	for (i = 0; i < cam->max_stream_num; i++) {
		if (req->ctx_used & (1 << i) && req->ctx_link_update & (1 << i)) {
			stream_id = i;
			ctx = &cam->ctxs[stream_id];
			req_stream_data = mtk_cam_req_get_s_data(req, stream_id, 0);
			raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);

			state_transition(&req_stream_data->state,
				E_STATE_CAMMUX_OUTER_CFG_DELAY, E_STATE_INNER);
			state_transition(&req_stream_data->state,
			E_STATE_SENINF, E_STATE_READY);

			val = readl(raw_dev->base + REG_TG_VF_CON);
			writel(val | TG_VFDATA_EN, raw_dev->base + REG_TG_VF_CON);

			if (ctx->prev_sensor || ctx->prev_seninf) {
				ctx->prev_sensor = NULL;
				ctx->prev_seninf = NULL;
			}
		}
	}
	dev_dbg(cam->dev, "%s: cam mux switch done\n", __func__);
}

static void mtk_cam_m2m_sensor_skip(struct mtk_cam_request_stream_data *req_stream_data)
{
	struct mtk_cam_request *req;
	struct mtk_cam_device *cam;
	struct media_request_object *obj;
	struct media_request_object *pipe_obj = NULL;
	struct v4l2_ctrl_handler *parent_hdl;
	struct mtk_cam_ctx *ctx;

	ctx = mtk_cam_s_data_get_ctx(req_stream_data);
	cam = ctx->cam;
	req = mtk_cam_s_data_get_req(req_stream_data);
	dev_dbg(cam->dev, "%s:%s:ctx(%d):sensor ctrl skip frame_seq_no %d\n",
		__func__, req->req.debug_str,
		ctx->stream_id, req_stream_data->frame_seq_no);

	state_transition(&req_stream_data->state,
	E_STATE_READY, E_STATE_SENSOR);

	req_stream_data->state.time_sensorset = ktime_get_boottime_ns() / 1000;

	/* request complete - time consuming*/
	list_for_each_entry(obj, &req->req.objects, list) {
		if (likely(obj))
			parent_hdl = obj->priv;
		else
			continue;
		if (parent_hdl == ctx->sensor->ctrl_handler ||
		    (ctx->prev_sensor && parent_hdl ==
		     ctx->prev_sensor->ctrl_handler)) {
#ifdef SENSOR_AE_CTRL_COMPLETE
			v4l2_ctrl_request_complete(&req->req, parent_hdl);
#else
			media_request_object_complete(obj);
#endif
		}
	}

	/* mark pipeline control completed */
	if (likely(pipe_obj))
		media_request_object_complete(pipe_obj);
}

static int mtk_cam_set_sensor_mstream_exposure(struct mtk_cam_ctx *ctx,
		struct mtk_cam_request_stream_data *req_stream_data)
{
	struct mtk_cam_device *cam = ctx->cam;
	int is_mstream_last_exposure = 0;

	if (!(req_stream_data->frame_seq_no & 0x1))
		is_mstream_last_exposure = 1;

	if (!ctx->sensor) {
		dev_info(cam->dev, "%s: ctx(%d): no sensor found\n",
			__func__, ctx->stream_id);
	} else {
		struct v4l2_ctrl *shutter_ctrl;
		struct v4l2_ctrl *gain_ctrl;
		u32 shutter, gain;

		shutter_ctrl = v4l2_ctrl_find(ctx->sensor->ctrl_handler,
				V4L2_CID_EXPOSURE);
		gain_ctrl = v4l2_ctrl_find(ctx->sensor->ctrl_handler,
				V4L2_CID_ANALOGUE_GAIN);

		if (!shutter_ctrl || !gain_ctrl) {
			dev_info(cam->dev, "%s: ctx(%d): no sensor exposure control found\n",
				__func__, ctx->stream_id);
			return is_mstream_last_exposure;
		}

		shutter = req_stream_data->mtk_cam_exposure.shutter;
		gain = req_stream_data->mtk_cam_exposure.gain;

		dev_dbg(ctx->cam->dev,
			"%s exposure:%d gain:%d\n", __func__, shutter, gain);

		v4l2_ctrl_s_ctrl(shutter_ctrl, shutter);
		v4l2_ctrl_s_ctrl(gain_ctrl, gain);
	}

	return is_mstream_last_exposure;
}

static void mtk_cam_sensor_worker(struct work_struct *work)
{
	struct mtk_cam_request *req;
	struct mtk_cam_req_work *sensor_work = (struct mtk_cam_req_work *)work;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_device *cam;
	struct media_request_object *obj;
	struct media_request_object *pipe_obj = NULL;
	struct v4l2_ctrl_handler *parent_hdl;
	struct mtk_cam_ctx *ctx;
	struct mtk_raw_device *raw_dev = NULL;
	unsigned int time_after_sof = 0;
	int sv_i;
	int i;
	int is_mstream_last_exposure = 0;

	req_stream_data = mtk_cam_req_work_get_s_data(sensor_work);
	ctx = mtk_cam_s_data_get_ctx(req_stream_data);
	cam = ctx->cam;
	req = mtk_cam_s_data_get_req(req_stream_data);

	/* Update ctx->sensor for switch sensor cases */
	if (req_stream_data->seninf_new)
		mtk_cam_update_sensor(ctx, &req_stream_data->seninf_new->entity);

	dev_dbg(cam->dev, "%s:%s:ctx(%d) req(%d):sensor try set start\n",
		__func__, req->req.debug_str, ctx->stream_id, req_stream_data->frame_seq_no);

	if (mtk_cam_req_frame_sync_start(req))
		dev_dbg(cam->dev, "%s:%s:ctx(%d): sensor ctrl with frame sync - start\n",
			__func__, req->req.debug_str, ctx->stream_id);
	if (mtk_cam_is_mstream(ctx))
		is_mstream_last_exposure =
			mtk_cam_set_sensor_mstream_exposure(ctx, req_stream_data);

	/* request setup*/
	/* 1st frame sensor setting in mstream is treated like normal frame and is set with
	 * other settings like max fps.
	 * 2nd is special, only expsure is set.
	 */
	if (!mtk_cam_is_stagger_m2m(ctx) && !is_mstream_last_exposure) {
		list_for_each_entry(obj, &req->req.objects, list) {
			if (likely(obj))
				parent_hdl = obj->priv;
			else
				continue;
			if (parent_hdl == ctx->sensor->ctrl_handler ||
			    (ctx->prev_sensor && parent_hdl ==
			     ctx->prev_sensor->ctrl_handler)) {
				struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;

				v4l2_ctrl_request_setup(&req->req, parent_hdl);
				time_after_sof = ktime_get_boottime_ns() / 1000000 -
					sensor_ctrl->sof_time;
				dev_dbg(cam->dev,
					"[SOF+%dms] Sensor request:%d[ctx:%d] setup\n",
					time_after_sof, req_stream_data->frame_seq_no,
					ctx->stream_id);
			}

			if (parent_hdl == &ctx->pipe->ctrl_handler)
				pipe_obj = obj;
		}

	}

	if (mtk_cam_is_subsample(ctx))
		state_transition(&req_stream_data->state,
		E_STATE_SUBSPL_OUTER, E_STATE_SUBSPL_SENSOR);
	else if (mtk_cam_is_time_shared(ctx))
		state_transition(&req_stream_data->state,
		E_STATE_TS_READY, E_STATE_TS_SENSOR);
	else
		state_transition(&req_stream_data->state,
		E_STATE_READY, E_STATE_SENSOR);

	if (mtk_cam_req_frame_sync_end(req))
		dev_dbg(cam->dev, "%s:ctx(%d): sensor ctrl with frame sync - stop\n",
			__func__, ctx->stream_id);
	spin_lock(&ctx->streaming_lock);
	if (ctx->streaming && ctx->used_raw_num) {
		raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
		if (req_stream_data->frame_seq_no == 1 &&
			raw_dev->vf_en == 0 && ctx->sensor_ctrl.initial_cq_done == 1) {
			unsigned int hw_scen = mtk_raw_get_hdr_scen_id(ctx);

			stream_on(raw_dev, 1);
			for (i = 0; i < ctx->used_sv_num; i++)
				mtk_cam_sv_dev_stream_on(ctx, i, 1, 1);
			for (i = MTKCAM_SUBDEV_CAMSV_END - 1;
				i >= MTKCAM_SUBDEV_CAMSV_START; i--) {
				if (mtk_cam_is_stagger(ctx) &&
				(ctx->pipe->enabled_raw & (1 << i)))
					mtk_cam_sv_dev_stream_on(
						ctx, i - MTKCAM_SUBDEV_CAMSV_START,
						1, hw_scen);
			}
		}
	}
	spin_unlock(&ctx->streaming_lock);
	req_stream_data->state.time_sensorset = ktime_get_boottime_ns() / 1000;
	dev_info(cam->dev, "%s:%s:ctx(%d)req(%d):sensor done at SOF+%dms\n",
		__func__, req->req.debug_str, ctx->stream_id,
		req_stream_data->frame_seq_no, time_after_sof);

	/* request complete - time consuming*/
	list_for_each_entry(obj, &req->req.objects, list) {
		if (likely(obj))
			parent_hdl = obj->priv;
		else
			continue;
		if (parent_hdl == ctx->sensor->ctrl_handler ||
		    (ctx->prev_sensor && parent_hdl ==
		     ctx->prev_sensor->ctrl_handler)) {
#ifdef SENSOR_AE_CTRL_COMPLETE
			v4l2_ctrl_request_complete(&req->req, parent_hdl);
#else
			media_request_object_complete(obj);
#endif
		}
	}

	mtk_cam_tg_flash_req_setup(ctx, req_stream_data);

	/* mark pipeline control completed */
	if (likely(pipe_obj))
		media_request_object_complete(pipe_obj);
	/* time sharing sv wdma flow - stream on at 1st request*/
	if (mtk_cam_is_time_shared(ctx) &&
		req_stream_data->frame_seq_no == 1) {
		unsigned int hw_scen =
			(1 << MTKCAM_IPI_HW_PATH_OFFLINE_M2M);
		for (sv_i = MTKCAM_SUBDEV_CAMSV_END - 1;
			sv_i >= MTKCAM_SUBDEV_CAMSV_START; sv_i--)
			if (ctx->pipe->enabled_raw & (1 << sv_i))
				mtk_cam_sv_dev_stream_on(ctx, sv_i - MTKCAM_SUBDEV_CAMSV_START,
							 1, hw_scen);
	}
}

static void mtk_cam_exp_switch_sensor_worker(struct work_struct *work)
{
	struct mtk_cam_request *req;
	struct mtk_cam_req_work *sensor_work = (struct mtk_cam_req_work *)work;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_device *cam;
	struct media_request_object *obj;
	struct media_request_object *pipe_obj = NULL;
	struct v4l2_ctrl_handler *parent_hdl;
	struct mtk_cam_ctx *ctx;
	unsigned int time_after_sof = 0;

	req_stream_data = mtk_cam_req_work_get_s_data(sensor_work);
	ctx = mtk_cam_s_data_get_ctx(req_stream_data);
	cam = ctx->cam;
	req = mtk_cam_s_data_get_req(req_stream_data);
	dev_dbg(cam->dev, "%s:%s:ctx(%d):sensor try set start\n",
		__func__, req->req.debug_str, ctx->stream_id);

	if (mtk_cam_req_frame_sync_start(req))
		dev_dbg(cam->dev, "%s:%s:ctx(%d): sensor ctrl with frame sync - start\n",
			__func__, req->req.debug_str, ctx->stream_id);
	/* request setup*/
	list_for_each_entry(obj, &req->req.objects, list) {
		if (likely(obj))
			parent_hdl = obj->priv;
		else
			continue;
		if (parent_hdl == ctx->sensor->ctrl_handler ||
		    (ctx->prev_sensor && parent_hdl ==
		     ctx->prev_sensor->ctrl_handler)) {
			struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;

			v4l2_ctrl_request_setup(&req->req, parent_hdl);
			time_after_sof = ktime_get_boottime_ns() / 1000000 -
				sensor_ctrl->sof_time;
			dev_dbg(cam->dev,
				"[SOF+%dms] Sensor request:%d[ctx:%d] setup\n",
				time_after_sof, req_stream_data->frame_seq_no,
				ctx->stream_id);
		}

		if (parent_hdl == &ctx->pipe->ctrl_handler)
			pipe_obj = obj;
	}
	state_transition(&req_stream_data->state,
		E_STATE_READY, E_STATE_SENSOR);
	if (mtk_cam_req_frame_sync_end(req))
		dev_dbg(cam->dev, "%s:ctx(%d): sensor ctrl with frame sync - stop\n",
			__func__, ctx->stream_id);


	req_stream_data->state.time_sensorset = ktime_get_boottime_ns() / 1000;
	dev_info(cam->dev, "%s:%s:ctx(%d)req(%d):sensor done at SOF+%dms\n",
		__func__, req->req.debug_str, ctx->stream_id,
		req_stream_data->frame_seq_no, time_after_sof);

	/* request complete - time consuming*/
	list_for_each_entry(obj, &req->req.objects, list) {
		if (likely(obj))
			parent_hdl = obj->priv;
		else
			continue;
		if (parent_hdl == ctx->sensor->ctrl_handler ||
		    (ctx->prev_sensor && parent_hdl ==
		     ctx->prev_sensor->ctrl_handler)) {
#ifdef SENSOR_AE_CTRL_COMPLETE
			v4l2_ctrl_request_complete(&req->req, parent_hdl);
#else
			media_request_object_complete(obj);
#endif
		}
	}

	/* mark pipeline control completed */
	if (likely(pipe_obj))
		media_request_object_complete(pipe_obj);
}

static int mtk_camsys_exp_switch_cam_mux(struct mtk_raw_device *raw_dev,
		struct mtk_cam_ctx *ctx, struct mtk_cam_request_stream_data *req_stream_data)
{
	struct mtk_cam_seninf_mux_param param;
	struct mtk_cam_seninf_mux_setting settings[3];
	int type = req_stream_data->feature.switch_feature_type;
	int sv_main_id, sv_sub_id;
	int config_exposure_num = 3;
	int feature_active;

	/**
	 * To identify the "max" exposure_num, we use
	 * feature_active, not req_stream_data->feature.raw_feature
	 * since the latter one stores the exposure_num information,
	 * not the max one.
	 */
	feature_active = ctx->pipe->feature_active;
	if (feature_active == STAGGER_2_EXPOSURE_LE_SE ||
	    feature_active == STAGGER_2_EXPOSURE_SE_LE)
		config_exposure_num = 2;

	if (type != EXPOSURE_CHANGE_NONE && config_exposure_num == 3) {
		sv_main_id = get_main_sv_pipe_id(ctx->cam, ctx->pipe->enabled_raw);
		sv_sub_id = get_sub_sv_pipe_id(ctx->cam, ctx->pipe->enabled_raw);
		switch (type) {
		case EXPOSURE_CHANGE_3_to_2:
		case EXPOSURE_CHANGE_1_to_2:
			settings[0].seninf = ctx->seninf;
			settings[0].source = PAD_SRC_RAW0;
			settings[0].camtg  =
				ctx->cam->sv.pipelines[
					sv_main_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[0].enable = 1;

			settings[1].seninf = ctx->seninf;
			settings[1].source = PAD_SRC_RAW1;
			settings[1].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[1].enable = 1;

			settings[2].seninf = ctx->seninf;
			settings[2].source = PAD_SRC_RAW2;
			settings[2].camtg  =
				ctx->cam->sv.pipelines[
					sv_sub_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[2].enable = 0;
			break;
		case EXPOSURE_CHANGE_3_to_1:
		case EXPOSURE_CHANGE_2_to_1:
			settings[0].seninf = ctx->seninf;
			settings[0].source = PAD_SRC_RAW0;
			settings[0].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[0].enable = 1;

			settings[1].seninf = ctx->seninf;
			settings[1].source = PAD_SRC_RAW1;
			settings[1].camtg  =
				ctx->cam->sv.pipelines[
					sv_sub_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[1].enable = 0;

			settings[2].seninf = ctx->seninf;
			settings[2].source = PAD_SRC_RAW2;
			settings[2].camtg  =
				ctx->cam->sv.pipelines[
					sv_main_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[2].enable = 0;
			break;
		case EXPOSURE_CHANGE_2_to_3:
		case EXPOSURE_CHANGE_1_to_3:
			settings[0].seninf = ctx->seninf;
			settings[0].source = PAD_SRC_RAW0;
			settings[0].camtg  =
				ctx->cam->sv.pipelines[
					sv_main_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[0].enable = 1;

			settings[1].seninf = ctx->seninf;
			settings[1].source = PAD_SRC_RAW1;
			settings[1].camtg  =
				ctx->cam->sv.pipelines[
					sv_sub_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[1].enable = 1;

			settings[2].seninf = ctx->seninf;
			settings[2].source = PAD_SRC_RAW2;
			settings[2].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[2].enable = 1;
			break;
		default:
			break;
		}
		param.settings = &settings[0];
		param.num = 3;
		mtk_cam_seninf_streaming_mux_change(&param);
		dev_info(ctx->cam->dev,
			"[%s] switch Req:%d, type:%d, cam_mux[0][1][2]:[%d/%d/%d][%d/%d/%d][%d/%d/%d]\n",
			__func__, req_stream_data->frame_seq_no, type,
			settings[0].source, settings[0].camtg, settings[0].enable,
			settings[1].source, settings[1].camtg, settings[1].enable,
			settings[2].source, settings[2].camtg, settings[2].enable);
	} else if (type != EXPOSURE_CHANGE_NONE && config_exposure_num == 2) {
		sv_main_id = get_main_sv_pipe_id(ctx->cam, ctx->pipe->enabled_raw);
		switch (type) {
		case EXPOSURE_CHANGE_2_to_1:
			settings[0].seninf = ctx->seninf;
			settings[0].source = PAD_SRC_RAW0;
			settings[0].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[0].enable = 1;
			settings[1].seninf = ctx->seninf;
			settings[1].source = PAD_SRC_RAW1;
			settings[1].camtg  =
				ctx->cam->sv.pipelines[
					sv_main_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[1].enable = 0;
			break;
		case EXPOSURE_CHANGE_1_to_2:
			settings[0].seninf = ctx->seninf;
			settings[0].source = PAD_SRC_RAW0;
			settings[0].camtg  =
				ctx->cam->sv.pipelines[
					sv_main_id - MTKCAM_SUBDEV_CAMSV_START].cammux_id;
			settings[0].enable = 1;
			settings[1].seninf = ctx->seninf;
			settings[1].source = PAD_SRC_RAW1;
			settings[1].camtg  = PipeIDtoTGIDX(raw_dev->id);
			settings[1].enable = 1;
			break;
		default:
			break;
		}
		param.settings = &settings[0];
		param.num = 2;
		mtk_cam_seninf_streaming_mux_change(&param);
		dev_info(ctx->cam->dev,
			"[%s] switch Req:%d, type:%d, cam_mux[0][1]:[%d/%d/%d][%d/%d/%d]\n",
			__func__, req_stream_data->frame_seq_no, type,
			settings[0].source, settings[0].camtg, settings[0].enable,
			settings[1].source, settings[1].camtg, settings[1].enable);
	}
	/*switch state*/
	if (type == EXPOSURE_CHANGE_3_to_1 ||
		type == EXPOSURE_CHANGE_2_to_1) {
		state_transition(&req_stream_data->state,
				E_STATE_CQ, E_STATE_CAMMUX_OUTER);
		state_transition(&req_stream_data->state,
				E_STATE_OUTER, E_STATE_CAMMUX_OUTER);
	}
	return 0;
}

static int mtk_cam_exp_sensor_switch(struct mtk_cam_ctx *ctx,
		struct mtk_cam_request_stream_data *req_stream_data)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_raw_device *raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
	int time_after_sof = ktime_get_boottime_ns() / 1000000 -
			   ctx->sensor_ctrl.sof_time;
	int type = req_stream_data->feature.switch_feature_type;

	if (!ctx->sensor_ctrl.sensorsetting_wq) {
		dev_info(cam->dev, "[set_sensor] return:workqueue null\n");
	} else {
		INIT_WORK(&req_stream_data->sensor_work.work,
		  mtk_cam_exp_switch_sensor_worker);
		queue_work(ctx->sensor_ctrl.sensorsetting_wq, &req_stream_data->sensor_work.work);
	}

	/**
	 * Normal to HDR switch case timing will be same as sensor mode
	 * switch.
	 */
	if (type == EXPOSURE_CHANGE_1_to_2 ||
		type == EXPOSURE_CHANGE_1_to_3)
		mtk_camsys_exp_switch_cam_mux(raw_dev, ctx, req_stream_data);
	dev_dbg(cam->dev,
		"[%s] [SOF+%dms]] ctx:%d, req:%d\n",
		__func__, time_after_sof, ctx->stream_id, req_stream_data->frame_seq_no);

	return 0;
}

static int mtk_cam_hdr_switch_toggle(struct mtk_cam_ctx *ctx, int raw_feature)
{
	struct mtk_raw_device *raw_dev;
	struct mtk_camsv_device *camsv_dev;
	struct mtk_cam_video_device *node;
	struct device *dev_sv;
	int sv_main_id, sv_sub_id;

	node = &ctx->pipe->vdev_nodes[MTK_RAW_MAIN_STREAM_OUT - MTK_RAW_SINK_NUM];
	raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
	sv_main_id = get_main_sv_pipe_id(ctx->cam, ctx->pipe->enabled_raw);
	dev_sv = ctx->cam->sv.devs[sv_main_id - MTKCAM_PIPE_CAMSV_0];
	camsv_dev = dev_get_drvdata(dev_sv);
	enable_tg_db(raw_dev, 0);
	mtk_cam_sv_toggle_tg_db(camsv_dev);
	if (raw_feature == STAGGER_3_EXPOSURE_LE_NE_SE ||
	    raw_feature == STAGGER_3_EXPOSURE_SE_NE_LE) {
		sv_sub_id = get_sub_sv_pipe_id(ctx->cam, ctx->pipe->enabled_raw);
		dev_sv = ctx->cam->sv.devs[sv_sub_id - MTKCAM_PIPE_CAMSV_0];
		camsv_dev = dev_get_drvdata(dev_sv);
		mtk_cam_sv_toggle_tg_db(camsv_dev);
	}
	enable_tg_db(raw_dev, 1);
	toggle_db(raw_dev);

	return 0;
}

void mtk_cam_set_sub_sample_sensor(struct mtk_raw_device *raw_dev,
			       struct mtk_cam_ctx *ctx)
{
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	int sensor_seq_no_next = sensor_ctrl->sensor_request_seq_no + 1;

	dev_dbg(ctx->cam->dev, "[%s:check] sensor_no:%d isp_no:%d\n", __func__,
				sensor_seq_no_next, sensor_ctrl->isp_request_seq_no);
	req_stream_data = mtk_cam_get_req_s_data(ctx, ctx->stream_id, sensor_seq_no_next);

	if (req_stream_data && (sensor_seq_no_next > 1) &&
		(sensor_seq_no_next > sensor_ctrl->isp_request_seq_no)) {
		if (req_stream_data->state.estate == E_STATE_SUBSPL_OUTER) {
			dev_dbg(ctx->cam->dev, "[%s:setup] sensor_no:%d stream_no:%d\n", __func__,
				sensor_seq_no_next, req_stream_data->frame_seq_no);
			INIT_WORK(&req_stream_data->sensor_work.work,
				mtk_cam_sensor_worker);
			queue_work(sensor_ctrl->sensorsetting_wq,
				&req_stream_data->sensor_work.work);
			sensor_ctrl->sensor_request_seq_no++;
		} else if (req_stream_data->state.estate == E_STATE_SUBSPL_SCQ) {
			dev_dbg(ctx->cam->dev, "[%s:setup:SCQ] sensor_no:%d stream_no:%d\n",
				__func__, sensor_seq_no_next, req_stream_data->frame_seq_no);
		}
	}
}
void mtk_cam_subspl_req_prepare(struct mtk_camsys_sensor_ctrl *sensor_ctrl)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request_stream_data *req_stream_data;
	int sensor_seq_no_next = sensor_ctrl->sensor_request_seq_no + 1;
	unsigned long flags;

	req_stream_data = mtk_cam_get_req_s_data(ctx, ctx->stream_id, sensor_seq_no_next);
	if (req_stream_data) {
		if (req_stream_data->state.estate == E_STATE_READY) {
			req_stream_data->state.time_swirq_timer =
				ktime_get_boottime_ns() / 1000;
			dev_dbg(cam->dev, "[%s] sensor_no:%d stream_no:%d\n", __func__,
					sensor_seq_no_next, req_stream_data->frame_seq_no);
			/* Increase the request ref count for camsys_state_list's usage*/
			media_request_get(&req_stream_data->req->req);

			/* EnQ this request's state element to state_list (STATE:READY) */
			spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
			list_add_tail(&req_stream_data->state.state_element,
					  &sensor_ctrl->camsys_state_list);
			state_transition(&req_stream_data->state,
				E_STATE_READY, E_STATE_SUBSPL_READY);
			spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
		}
	}
}

static void
mtk_cam_set_sensor(struct mtk_cam_request_stream_data *s_data,
		   struct mtk_camsys_sensor_ctrl *sensor_ctrl)
{
	unsigned long flags;

	/* Increase the request ref count for camsys_state_list's usage*/
	media_request_get(&s_data->req->req);
	/* EnQ this request's state element to state_list (STATE:READY) */
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_add_tail(&s_data->state.state_element,
		      &sensor_ctrl->camsys_state_list);
	sensor_ctrl->sensor_request_seq_no = s_data->frame_seq_no;
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);

	if (s_data->feature.switch_feature_type &&
	    !mtk_cam_feature_change_is_mstream(s_data->feature.switch_feature_type)) {
		dev_info(sensor_ctrl->ctx->cam->dev,
			 "[TimerIRQ] switch type:%d request:%d - pass sensor\n",
			 s_data->feature.switch_feature_type,
			 s_data->frame_seq_no);
		return;
	}

	if (!sensor_ctrl->sensorsetting_wq) {
		pr_info("[set_sensor] return:workqueue null\n");
	} else {
		if (mtk_cam_is_stagger_m2m(sensor_ctrl->ctx))
			mtk_cam_m2m_sensor_skip(s_data);
		else {
			INIT_WORK(&s_data->sensor_work.work,
				  mtk_cam_sensor_worker);
			queue_work(sensor_ctrl->sensorsetting_wq,
				   &s_data->sensor_work.work);
		}
	}
}

static enum hrtimer_restart sensor_set_handler(struct hrtimer *t)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl =
		container_of(t, struct mtk_camsys_sensor_ctrl,
			     sensor_deadline_timer);
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_camsys_ctrl_state *state_entry;
	unsigned long flags;
	int sensor_seq_no_next = sensor_ctrl->sensor_request_seq_no + 1;
	int time_after_sof = ktime_get_boottime_ns() / 1000000 -
			   ctx->sensor_ctrl.sof_time;
	struct mtk_cam_request *req;

	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	/* Check if previous state was without cq done */
	list_for_each_entry(state_entry, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_entry);
		if (req_stream_data->frame_seq_no == sensor_ctrl->sensor_request_seq_no) {
			if (state_entry->estate == E_STATE_CQ && USINGSCQ &&
			    req_stream_data->frame_seq_no > INITIAL_DROP_FRAME_CNT &&
			    !mtk_cam_is_stagger(ctx)) {
				state_entry->estate = E_STATE_CQ_SCQ_DELAY;
				spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
				dev_info(ctx->cam->dev,
					 "[TimerIRQ] SCQ DELAY STATE at SOF+%dms\n",
					 time_after_sof);
				return HRTIMER_NORESTART;
			} else if (state_entry->estate == E_STATE_CAMMUX_OUTER_CFG) {
				state_entry->estate = E_STATE_CAMMUX_OUTER_CFG_DELAY;
				dev_dbg(ctx->cam->dev,
					"[TimerIRQ] CAMMUX OUTTER CFG DELAY STATE\n");
				spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
				return HRTIMER_NORESTART;

			} else if (state_entry->estate <= E_STATE_SENSOR) {
				spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
				dev_info(ctx->cam->dev,
					 "[TimerIRQ] wrong state:%d (sensor workqueue delay)\n",
					 state_entry->estate);
				return HRTIMER_NORESTART;
			}
		} else if (req_stream_data->frame_seq_no ==
			sensor_ctrl->sensor_request_seq_no - 1) {
			if (state_entry->estate < E_STATE_INNER) {
				spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
				dev_info(ctx->cam->dev,
					 "[TimerIRQ] req:%d isn't arrive inner at SOF+%dms\n",
					 req_stream_data->frame_seq_no, time_after_sof);
				return HRTIMER_NORESTART;
			}
		}
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);

	req_stream_data =  mtk_cam_get_req_s_data(ctx, ctx->stream_id, sensor_seq_no_next);
	if (req_stream_data) {
		req = mtk_cam_s_data_get_req(req_stream_data);
		if (req_stream_data->state.estate == E_STATE_SENINF &&
			!(req->flags & MTK_CAM_REQ_FLAG_SENINF_CHANGED)) {
			dev_info(ctx->cam->dev,
				 "[TimerIRQ] wrong state:%d (seninf change delay)\n",
				 state_entry->estate);
			return HRTIMER_NORESTART;
		}
		req_stream_data->state.time_swirq_timer = ktime_get_boottime_ns() / 1000;
		mtk_cam_set_sensor(req_stream_data, &ctx->sensor_ctrl);
		dev_dbg(cam->dev,
			"%s:[TimerIRQ [SOF+%dms]:] ctx:%d, sensor_req_seq_no:%d\n",
			__func__, time_after_sof, ctx->stream_id, sensor_seq_no_next);
	} else {
		dev_dbg(cam->dev,
			"%s:[TimerIRQ [SOF+%dms]] ctx:%d, empty req_queue, sensor_req_seq_no:%d\n",
			__func__, time_after_sof, ctx->stream_id,
			sensor_ctrl->sensor_request_seq_no);
	}

	return HRTIMER_NORESTART;
}
static enum hrtimer_restart sensor_deadline_timer_handler(struct hrtimer *t)
{
	unsigned int i;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl =
		container_of(t, struct mtk_camsys_sensor_ctrl,
			     sensor_deadline_timer);
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_camsv_device *camsv_dev;
	ktime_t m_kt;
	int time_after_sof = ktime_get_boottime_ns() / 1000000 -
			   sensor_ctrl->sof_time;
	bool drained_res = false;

	sensor_ctrl->sensor_deadline_timer.function = sensor_set_handler;
	m_kt = ktime_set(0, sensor_ctrl->timer_req_sensor * 1000000);

	if (ctx->used_raw_num) {
		/* handle V4L2_EVENT_REQUEST_DRAINED event */
		drained_res = mtk_cam_request_drained(sensor_ctrl);
	}
	for (i = 0; i < ctx->used_sv_num; i++) {
		camsv_dev = cam->camsys_ctrl.camsv_dev[ctx->sv_pipe[i]->id -
			MTKCAM_SUBDEV_CAMSV_START];
		dev_dbg(camsv_dev->dev, "[SOF+%dms]\n", time_after_sof);
		/* handle V4L2_EVENT_REQUEST_DRAINED event */
		mtk_cam_sv_request_drained(camsv_dev, sensor_ctrl);
	}
	dev_dbg(cam->dev,
			"[TimerIRQ [SOF+%dms]] ctx:%d, sensor_req_seq_no:%d\n",
			time_after_sof, ctx->stream_id,
			sensor_ctrl->sensor_request_seq_no);
	if (mtk_cam_is_subsample(ctx)) {
		if (!drained_res)
			mtk_cam_subspl_req_prepare(sensor_ctrl);
		return HRTIMER_NORESTART;
	}
	hrtimer_forward_now(&sensor_ctrl->sensor_deadline_timer, m_kt);

	return HRTIMER_RESTART;
}

static void mtk_cam_sof_timer_setup(struct mtk_cam_ctx *ctx)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	ktime_t m_kt;
	struct mtk_seninf_sof_notify_param param;

	/*notify sof to sensor*/
	param.sd = ctx->seninf;
	param.sof_cnt = sensor_ctrl->sensor_request_seq_no;
	mtk_cam_seninf_sof_notify(&param);

	sensor_ctrl->sof_time = ktime_get_boottime_ns() / 1000000;
	sensor_ctrl->sensor_deadline_timer.function =
		sensor_deadline_timer_handler;
	sensor_ctrl->ctx = ctx;
	m_kt = ktime_set(0, sensor_ctrl->timer_req_event * 1000000);
	hrtimer_start(&sensor_ctrl->sensor_deadline_timer, m_kt,
		      HRTIMER_MODE_REL);
}

static void
mtk_cam_set_timestamp(struct mtk_cam_request_stream_data *stream_data,
		      u64 time_boot,
		      u64 time_mono)
{
	stream_data->timestamp = time_boot;
	stream_data->timestamp_mono = time_mono;
}

int mtk_camsys_raw_subspl_state_handle(struct mtk_raw_device *raw_dev,
				   struct mtk_camsys_sensor_ctrl *sensor_ctrl,
		struct mtk_camsys_ctrl_state **current_state,
		int frame_inner_idx)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_camsys_ctrl_state *state_temp, *state_outer = NULL;
	struct mtk_camsys_ctrl_state *state_ready = NULL;
	struct mtk_camsys_ctrl_state *state_rec[STATE_NUM_AT_SOF];
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int stateidx;
	int que_cnt = 0;
	unsigned long flags;
	u64 time_boot = ktime_get_boottime_ns();
	u64 time_mono = ktime_get_ns();

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_temp);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		stateidx = (sensor_ctrl->sensor_request_seq_no + 1) -
			   req_stream_data->frame_seq_no;
		if (stateidx < STATE_NUM_AT_SOF && stateidx > -1) {
			state_rec[stateidx] = state_temp;
			/* Find outer state element */
			if (state_temp->estate == E_STATE_SUBSPL_SENSOR ||
				state_temp->estate == E_STATE_SUBSPL_OUTER) {
				state_outer = state_temp;
				mtk_cam_set_timestamp(req_stream_data,
						      time_boot, time_mono);
				req_stream_data->state.time_irq_sof2 =
							ktime_get_boottime_ns() / 1000;
			}
			if (state_temp->estate == E_STATE_SUBSPL_READY ||
				state_temp->estate == E_STATE_SUBSPL_SCQ_DELAY) {
				state_ready = state_temp;
				req_stream_data->state.time_irq_sof1 =
							ktime_get_boottime_ns() / 1000;
			}
			dev_dbg(raw_dev->dev,
			"[SOF-subsample] STATE_CHECK [N-%d] Req:%d / State:0x%x\n",
			stateidx, req_stream_data->frame_seq_no,
			state_rec[stateidx]->estate);
		}
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
	/* HW imcomplete case */
	if (que_cnt >= STATE_NUM_AT_SOF)
		dev_dbg(raw_dev->dev, "[SOF-subsample] HW_DELAY state\n");
	/* Trigger high resolution timer to try sensor setting */
	mtk_cam_sof_timer_setup(ctx);
	/* Transit outer state to inner state */
	if (state_outer && sensor_ctrl->sensorsetting_wq) {
		req = mtk_cam_ctrl_state_get_req(state_outer);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		if (req_stream_data->frame_seq_no == frame_inner_idx) {
			if (frame_inner_idx > sensor_ctrl->isp_request_seq_no) {
				if (state_outer->estate == E_STATE_SUBSPL_OUTER) {
					mtk_cam_set_sub_sample_sensor(raw_dev, ctx);
					state_transition(state_outer, E_STATE_SUBSPL_OUTER,
						 E_STATE_SUBSPL_INNER);
					dev_dbg(raw_dev->dev, "sensor delay to SOF\n");
				}
				state_transition(state_outer, E_STATE_SUBSPL_SENSOR,
						 E_STATE_SUBSPL_INNER);
				sensor_ctrl->isp_request_seq_no =
					frame_inner_idx;
				dev_dbg(raw_dev->dev,
					"[SOF-subsample] frame_seq_no:%d, SENSOR/OUTER->INNER state:0x%x\n",
					req_stream_data->frame_seq_no, state_outer->estate);
			}
		}
	}
	/* Initial request case */
	if (sensor_ctrl->sensor_request_seq_no <
		INITIAL_DROP_FRAME_CNT) {
		sensor_ctrl->isp_request_seq_no = frame_inner_idx;
		dev_dbg(raw_dev->dev, "[SOF-subsample] INIT STATE cnt:%d\n", que_cnt);
		if (que_cnt > 0 && state_ready)
			state_transition(state_ready, E_STATE_SUBSPL_READY,
					 E_STATE_SUBSPL_SCQ);
		return STATE_RESULT_PASS_CQ_INIT;
	}
	if (que_cnt > 0 && state_ready) {
		/* CQ triggering judgment*/
		if (state_ready->estate == E_STATE_SUBSPL_READY) {
			*current_state = state_ready;
			return STATE_RESULT_TRIGGER_CQ;
		}
		/* last SCQ triggering delay judgment*/
		if (state_ready->estate == E_STATE_SUBSPL_SCQ_DELAY) {
			dev_dbg(raw_dev->dev, "[SOF-subsample] SCQ_DELAY state:0x%x\n",
				state_ready->estate);
			return STATE_RESULT_PASS_CQ_SCQ_DELAY;
		}
	}

	return STATE_RESULT_PASS_CQ_SW_DELAY;
}

static int mtk_camsys_raw_state_handle(struct mtk_raw_device *raw_dev,
				   struct mtk_camsys_sensor_ctrl *sensor_ctrl,
		struct mtk_camsys_ctrl_state **current_state,
		int frame_inner_idx)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_camsys_ctrl_state *state_temp, *state_outer = NULL;
	struct mtk_camsys_ctrl_state *state_sensor = NULL;
	struct mtk_camsys_ctrl_state *state_inner = NULL;
	struct mtk_camsys_ctrl_state *state_rec[STATE_NUM_AT_SOF];
	struct mtk_cam_request_stream_data *req_stream_data;
	int stateidx;
	int que_cnt = 0;
	int write_cnt;
	unsigned long flags;
	u64 time_boot = ktime_get_boottime_ns();
	u64 time_mono = ktime_get_ns();
	int working_req_found = 0;
	int switch_type;

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_temp);
		stateidx = sensor_ctrl->sensor_request_seq_no -
			   req_stream_data->frame_seq_no;
		if (stateidx < STATE_NUM_AT_SOF && stateidx > -1) {
			state_rec[stateidx] = state_temp;
			if (stateidx == 0)
				working_req_found = 1;
			/* Find outer state element */
			if (state_temp->estate == E_STATE_OUTER ||
				state_temp->estate == E_STATE_CAMMUX_OUTER ||
			    state_temp->estate == E_STATE_OUTER_HW_DELAY) {
				state_outer = state_temp;
				mtk_cam_set_timestamp(req_stream_data,
						      time_boot, time_mono);
				req_stream_data->state.time_irq_sof2 =
							ktime_get_boottime_ns() / 1000;
			}
			/* Find inner state element request*/
			if (state_temp->estate == E_STATE_INNER ||
			    state_temp->estate == E_STATE_INNER_HW_DELAY) {
				state_inner = state_temp;
			}
			/* Find sensor state element request*/
			if (state_temp->estate <= E_STATE_SENSOR) {
				state_sensor = state_temp;
				req_stream_data->state.time_irq_sof1 =
							ktime_get_boottime_ns() / 1000;
			}
			dev_dbg(raw_dev->dev,
			"[SOF] STATE_CHECK [N-%d] Req:%d / State:%d\n",
			stateidx, req_stream_data->frame_seq_no,
			state_rec[stateidx]->estate);
		}
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
	/* HW imcomplete case */
	if (state_inner) {
		req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_inner);
		write_cnt = (sensor_ctrl->isp_request_seq_no / 256) * 256 + raw_dev->write_cnt;
		if (frame_inner_idx > sensor_ctrl->isp_request_seq_no ||
			req_stream_data->frame_done_queue_work == 1) {
			dev_dbg(raw_dev->dev, "[SOF] frame done work too late\n");
		} else if (write_cnt >= req_stream_data->frame_seq_no) {
			dev_info_ratelimited(raw_dev->dev, "[SOF] frame done reading lost %d frames\n",
				write_cnt - req_stream_data->frame_seq_no + 1);
			mtk_cam_set_timestamp(req_stream_data,
						      time_boot - 1000, time_mono - 1000);
			mtk_camsys_frame_done(ctx, write_cnt, ctx->stream_id);
		} else if (mtk_cam_is_stagger(ctx)) {
			dev_dbg(raw_dev->dev, "[SOF:%d] HDR SWD over SOF case\n", frame_inner_idx);
		} else {
			state_transition(state_inner, E_STATE_INNER,
				 E_STATE_INNER_HW_DELAY);
			if (state_outer) {
				state_transition(state_outer, E_STATE_OUTER,
				 E_STATE_OUTER_HW_DELAY);
				state_transition(state_outer, E_STATE_CAMMUX_OUTER,
				 E_STATE_OUTER_HW_DELAY);
			}
			dev_info_ratelimited(raw_dev->dev, "[SOF] HW_IMCOMPLETE state\n");
			return STATE_RESULT_PASS_CQ_HW_DELAY;
		}
	}
	/* Trigger high resolution timer to try sensor setting */
	mtk_cam_sof_timer_setup(ctx);
	/* Transit outer state to inner state */
	if (state_outer && sensor_ctrl->sensorsetting_wq) {
		req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_outer);
		if (sensor_ctrl->initial_drop_frame_cnt == 0 &&
			req_stream_data->frame_seq_no == frame_inner_idx) {
			if (frame_inner_idx > sensor_ctrl->isp_request_seq_no) {
				state_transition(state_outer,
						 E_STATE_OUTER_HW_DELAY,
						 E_STATE_INNER_HW_DELAY);
				state_transition(state_outer, E_STATE_OUTER,
						 E_STATE_INNER);
				state_transition(state_outer, E_STATE_CAMMUX_OUTER,
						 E_STATE_INNER);
				sensor_ctrl->isp_request_seq_no =
					frame_inner_idx;
				dev_dbg(raw_dev->dev,
					"[SOF-DBLOAD] frame_seq_no:%d, OUTER->INNER state:%d\n",
					req_stream_data->frame_seq_no, state_outer->estate);
			}
		}
	}
	/* Initial request case - 1st sensor wasn't set yet or initial drop wasn't finished*/
	if (sensor_ctrl->sensor_request_seq_no <= INITIAL_DROP_FRAME_CNT ||
		sensor_ctrl->initial_drop_frame_cnt) {
		dev_dbg(raw_dev->dev, "[SOF] INIT STATE cnt:%d\n", que_cnt);
		if (que_cnt > 0) {
			state_temp = state_rec[0];
			req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_temp);
			if (req_stream_data->frame_seq_no == 1)
				state_transition(state_temp, E_STATE_SENSOR,
					 E_STATE_OUTER);
		}
		return STATE_RESULT_PASS_CQ_INIT;
	}
	if (que_cnt > 0) {
		/*handle exposure switch at frame start*/
		if (state_sensor) {
			req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_sensor);
			switch_type = req_stream_data->feature.switch_feature_type;
			if (switch_type &&
			    !mtk_cam_feature_change_is_mstream(switch_type)) {
				mtk_cam_exp_sensor_switch(ctx, req_stream_data);
				state_transition(state_sensor, E_STATE_READY,
						 E_STATE_SENSOR);
				*current_state = state_sensor;
				return STATE_RESULT_TRIGGER_CQ;
			}
		}
		if (working_req_found) {
			if (state_rec[0]->estate == E_STATE_READY)
				dev_info(raw_dev->dev, "[SOF] sensor delay\n");
			/* CQ triggering judgment*/
			if (state_rec[0]->estate == E_STATE_SENSOR) {
				*current_state = state_rec[0];
				return STATE_RESULT_TRIGGER_CQ;
			}
			/* last SCQ triggering delay judgment*/
			if (state_rec[0]->estate == E_STATE_CQ_SCQ_DELAY) {
				state_transition(state_rec[0], E_STATE_CQ_SCQ_DELAY,
						E_STATE_OUTER);
				dev_info(raw_dev->dev, "[SOF] SCQ_DELAY state:%d\n",
					state_rec[0]->estate);
				return STATE_RESULT_PASS_CQ_SCQ_DELAY;
			}
		} else {
			dev_dbg(raw_dev->dev, "[SOF] working request not found\n");
		}
	}
	return STATE_RESULT_PASS_CQ_SW_DELAY;
}

static void mtk_camsys_ts_sv_done(struct mtk_cam_ctx *ctx,
				       unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_request *req = NULL;
	struct mtk_cam_request_stream_data *req_stream_data;

	/* Find request of this sv dequeued frame */
	req = mtk_cam_get_req(ctx, dequeued_frame_seq_no);
	if (req != NULL) {
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		state_transition(&req_stream_data->state, E_STATE_TS_SV,
					E_STATE_TS_MEM);
		dev_info(ctx->cam->dev,
		"TS-SVD[ctx:%d-#%d], SV done state:0x%x\n",
		ctx->stream_id, dequeued_frame_seq_no, req_stream_data->state.estate);
	}

}


static void mtk_camsys_ts_raw_try_set(struct mtk_raw_device *raw_dev,
				       struct mtk_cam_ctx *ctx,
				       unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_request *req = NULL;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_working_buf_entry *buf_entry;
	dma_addr_t base_addr;

	/* Find request of this dequeued frame */
	req = mtk_cam_get_req(ctx, dequeued_frame_seq_no);
	if (!req) {
		dev_info(raw_dev->dev,
			"TS-CQ[ctx:%d-#%d], request drained\n",
			ctx->stream_id, dequeued_frame_seq_no);
		return;
	}
	req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
	if (raw_dev->time_shared_busy ||
		req_stream_data->state.estate != E_STATE_TS_MEM) {
		dev_info(raw_dev->dev,
			"TS-CQ[ctx:%d-#%d], CQ isn't updated [busy:%d/state:0x%x]\n",
			ctx->stream_id, dequeued_frame_seq_no,
			raw_dev->time_shared_busy, req_stream_data->state.estate);
		return;
	}
	raw_dev->time_shared_busy = true;
	/* Update CQ base address if needed */
	if (ctx->composed_frame_seq_no < dequeued_frame_seq_no) {
		dev_info(raw_dev->dev,
			"TS-CQ[ctx:%d-#%d], CQ isn't updated [composed_frame_deq (%d) ]\n",
			ctx->stream_id, dequeued_frame_seq_no,
			ctx->composed_frame_seq_no);
		return;
	}
	/* apply next composed buffer */
	spin_lock(&ctx->composed_buffer_list.lock);
	if (list_empty(&ctx->composed_buffer_list.list)) {
		dev_info(raw_dev->dev,
			"TS-CQ, no buffer update, cq_num:%d, frame_seq:%d\n",
			ctx->composed_frame_seq_no, dequeued_frame_seq_no);
		spin_unlock(&ctx->composed_buffer_list.lock);
	} else {
		buf_entry = list_first_entry(&ctx->composed_buffer_list.list,
					     struct mtk_cam_working_buf_entry,
					     list_entry);
		list_del(&buf_entry->list_entry);
		ctx->composed_buffer_list.cnt--;
		spin_unlock(&ctx->composed_buffer_list.lock);
		spin_lock(&ctx->processing_buffer_list.lock);
		list_add_tail(&buf_entry->list_entry,
			      &ctx->processing_buffer_list.list);
		ctx->processing_buffer_list.cnt++;
		spin_unlock(&ctx->processing_buffer_list.lock);
		base_addr = buf_entry->buffer.iova;
		apply_cq(raw_dev,
			base_addr,
			buf_entry->cq_desc_size, buf_entry->cq_desc_offset, 0,
			buf_entry->sub_cq_desc_size, buf_entry->sub_cq_desc_offset);
		state_transition(&req_stream_data->state, E_STATE_TS_MEM,
						E_STATE_TS_CQ);
		raw_dev->time_shared_busy_ctx_id = ctx->stream_id;
		dev_info(raw_dev->dev,
			"TS-CQ[ctx:%d-#%d], update CQ state:0x%x [composed_req(%d)]\n",
			ctx->stream_id, dequeued_frame_seq_no, req_stream_data->state.estate,
			ctx->composed_frame_seq_no);
	}
}
static int mtk_camsys_ts_state_handle(
		struct mtk_camsys_sensor_ctrl *sensor_ctrl,
		struct mtk_camsys_ctrl_state **current_state,
		int frame_inner_idx)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_camsys_ctrl_state *state_temp = NULL;
	struct mtk_camsys_ctrl_state *state_rec[STATE_NUM_AT_SOF];
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int stateidx;
	int que_cnt = 0;
	unsigned long flags;
	u64 time_boot = ktime_get_boottime_ns();
	u64 time_mono = ktime_get_ns();

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_temp);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		stateidx = sensor_ctrl->sensor_request_seq_no -
			   req_stream_data->frame_seq_no;
		if (stateidx < STATE_NUM_AT_SOF && stateidx > -1) {
			state_rec[stateidx] = state_temp;
			if (state_temp->estate <= E_STATE_TS_SENSOR)
				req_stream_data->state.time_irq_sof1 =
					ktime_get_boottime_ns() / 1000;
			if (state_temp->estate == E_STATE_TS_SV) {
				req_stream_data->timestamp = time_boot;
				req_stream_data->timestamp_mono = time_mono;
			}
			dev_info(ctx->cam->dev,
			"[TS-SOF] ctx:%d STATE_CHECK [N-%d] Req:%d / State:0x%x\n",
			ctx->stream_id, stateidx, req_stream_data->frame_seq_no,
			state_rec[stateidx]->estate);
		}
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
	if (que_cnt > 0) {
		if (state_rec[0]->estate == E_STATE_TS_READY) {
			dev_info(ctx->cam->dev, "[TS-SOF] sensor delay\n");
			return STATE_RESULT_PASS_CQ_SW_DELAY;
		}
	}
	/* Trigger high resolution timer to try sensor setting */
	mtk_cam_sof_timer_setup(ctx);
	if (que_cnt > 0) {
		/* camsv enque judgment*/
		if (state_rec[0]->estate == E_STATE_TS_SENSOR) {
			*current_state = state_rec[0];
			return STATE_RESULT_TRIGGER_CQ;
		}
	}

	return STATE_RESULT_PASS_CQ_SW_DELAY;
}
static void mtk_camsys_ts_frame_start(struct mtk_cam_ctx *ctx,
				       unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request *req_cq = NULL;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *current_state;
	enum MTK_CAMSYS_STATE_RESULT state_handle_ret;
	struct mtk_camsv_device *camsv_dev;
	struct device *dev_sv;
	int sv_i;

	/* Send V4L2_EVENT_FRAME_SYNC event */
	mtk_cam_event_frame_sync(ctx->pipe, dequeued_frame_seq_no);

	if (ctx->sensor) {
		state_handle_ret =
			mtk_camsys_ts_state_handle(sensor_ctrl,
						&current_state,
						dequeued_frame_seq_no);
		if (state_handle_ret != STATE_RESULT_TRIGGER_CQ) {
			dev_info(cam->dev, "[TS-SOF] SV-ENQUE drop s:%d deq:%d\n",
				state_handle_ret, dequeued_frame_seq_no);
			return;
		}
	}
	/* Transit state from Sensor -> CQ */
	if (ctx->sensor) {
		state_transition(current_state,
			E_STATE_TS_SENSOR, E_STATE_TS_SV);
		req_cq = mtk_cam_ctrl_state_get_req(current_state);
		req_stream_data = mtk_cam_req_get_s_data(req_cq, ctx->stream_id, 0);
		/* time sharing sv wdma flow - stream on at 1st request*/
		for (sv_i = MTKCAM_SUBDEV_CAMSV_END - 1;
			sv_i >= MTKCAM_SUBDEV_CAMSV_START; sv_i--) {
			if (ctx->pipe->enabled_raw & (1 << sv_i)) {
				dev_sv = cam->sv.devs[sv_i - MTKCAM_SUBDEV_CAMSV_START];
				camsv_dev = dev_get_drvdata(dev_sv);
				mtk_cam_sv_enquehwbuf(camsv_dev,
				req_stream_data->frame_params.img_ins[0].buf[0].iova,
				req_stream_data->frame_seq_no);
			}
		}
		req_stream_data->timestamp = ktime_get_boottime_ns();
		req_stream_data->state.time_cqset = ktime_get_boottime_ns() / 1000;
		dev_info(cam->dev,
		"TS-SOF[ctx:%d-#%d], SV-ENQ req:%d is update, composed:%d, iova:0x%x, time:%lld\n",
		ctx->stream_id, dequeued_frame_seq_no, req_stream_data->frame_seq_no,
		ctx->composed_frame_seq_no, req_stream_data->frame_params.img_ins[0].buf[0].iova,
		req_stream_data->timestamp);
	}
}

static void mtk_camsys_raw_m2m_frame_done(struct mtk_raw_device *raw_dev,
				       struct mtk_cam_ctx *ctx,
				       unsigned int dequeued_frame_seq_no)
{
	struct mtk_camsys_ctrl_state *state_temp, *state_inner = NULL;
	struct mtk_camsys_ctrl_state *state_sensor = NULL;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_working_buf_entry *buf_entry;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	dma_addr_t base_addr;
	int que_cnt = 0;
	unsigned long flags;
	u64 time_boot = ktime_get_boottime_ns();
	u64 time_mono = ktime_get_ns();
	bool dequeue_result = false;

	/* Send V4L2_EVENT_FRAME_SYNC event */
	mtk_cam_event_frame_sync(ctx->pipe, dequeued_frame_seq_no);

	ctx->dequeued_frame_seq_no = dequeued_frame_seq_no;

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_temp);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);

		if (state_temp->estate == E_STATE_INNER && state_inner == NULL)
			state_inner = state_temp;
		else if (state_temp->estate == E_STATE_SENSOR && state_sensor == NULL)
			state_sensor = state_temp;
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);

	/* Transit inner state to done state */
	if (state_inner) {
		req = mtk_cam_ctrl_state_get_req(state_inner);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);

		dev_dbg(raw_dev->dev,
			"[M2M P1 Don] req_stream_data->frame_seq_no:%d dequeued_frame_seq_no:%d\n",
			req_stream_data->frame_seq_no, dequeued_frame_seq_no);

		if (req_stream_data->frame_seq_no == dequeued_frame_seq_no) {
			state_transition(state_inner, E_STATE_INNER,
			      E_STATE_DONE_NORMAL);
			sensor_ctrl->isp_request_seq_no =
			      dequeued_frame_seq_no;
			dev_dbg(raw_dev->dev,
			      "[Frame done] frame_seq_no:%d, INNER->DONE_NORMAL state:%d\n",
			      req_stream_data->frame_seq_no, state_inner->estate);
		}
	}

	spin_lock(&ctx->m2m_lock);
	dequeue_result = mtk_cam_dequeue_req_frame(ctx, dequeued_frame_seq_no, ctx->stream_id);

	/* apply next composed buffer */
	spin_lock(&ctx->composed_buffer_list.lock);
	dev_dbg(raw_dev->dev,
		"[M2M check next action] que_cnt:%d composed_buffer_list.cnt:%d\n",
		que_cnt, ctx->composed_buffer_list.cnt);

	if (list_empty(&ctx->composed_buffer_list.list)) {
		dev_info(raw_dev->dev,
			"[M2M] no buffer, cq_num:%d, frame_seq:%d, composed_buffer_list.cnt :%d\n",
			ctx->composed_frame_seq_no, dequeued_frame_seq_no,
			ctx->composed_buffer_list.cnt);
		spin_unlock(&ctx->composed_buffer_list.lock);
		spin_unlock(&ctx->m2m_lock);
	} else {
		buf_entry = list_first_entry(&ctx->composed_buffer_list.list,
					     struct mtk_cam_working_buf_entry,
					     list_entry);
		list_del(&buf_entry->list_entry);
		ctx->composed_buffer_list.cnt--;
		spin_unlock(&ctx->composed_buffer_list.lock);
		spin_lock(&ctx->processing_buffer_list.lock);
		list_add_tail(&buf_entry->list_entry,
			      &ctx->processing_buffer_list.list);
		ctx->processing_buffer_list.cnt++;

		dev_dbg(raw_dev->dev,
			"[M2M P1 Don] ctx->processing_buffer_list.cnt:%d\n",
			ctx->processing_buffer_list.cnt);

		spin_unlock(&ctx->processing_buffer_list.lock);
		spin_unlock(&ctx->m2m_lock);

		base_addr = buf_entry->buffer.iova;

		if (state_sensor == NULL) {
			dev_info(raw_dev->dev, "[M2M P1 Don] Invalid state_sensor\n");
			return;
		}

		req = mtk_cam_ctrl_state_get_req(state_sensor);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		req_stream_data->timestamp = time_boot;
		req_stream_data->timestamp_mono = time_mono;

		apply_cq(raw_dev,
			base_addr,
			buf_entry->cq_desc_size, buf_entry->cq_desc_offset, 0,
			buf_entry->sub_cq_desc_size, buf_entry->sub_cq_desc_offset);
		/* Transit state from Sensor -> CQ */
		if (ctx->sensor) {
			state_transition(state_sensor, E_STATE_SENSOR, E_STATE_CQ);

			dev_dbg(raw_dev->dev,
			"M2M apply_cq [ctx:%d-#%d], CQ-%d, composed:%d, cq_addr:0x%x\n",
			ctx->stream_id, dequeued_frame_seq_no, req_stream_data->frame_seq_no,
			ctx->composed_frame_seq_no, base_addr);

			dev_dbg(raw_dev->dev,
			"M2M apply_cq: composed_buffer_list.cnt:%d time:%lld, monotime:%lld\n",
			ctx->composed_buffer_list.cnt, req_stream_data->timestamp,
			req_stream_data->timestamp_mono);
		}
	}

	if (dequeue_result) {
		mutex_lock(&ctx->cam->op_lock);
		mtk_cam_dev_req_try_queue(ctx->cam);
		mutex_unlock(&ctx->cam->op_lock);
	}
}

static void mtk_cam_mstream_frame_sync(struct mtk_raw_device *raw_dev,
					struct mtk_cam_ctx *ctx,
					unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_request *req;

	req = mtk_cam_get_req(ctx, dequeued_frame_seq_no);
	if (req) {
		struct mtk_cam_request_stream_data *s_data =
			mtk_cam_req_get_s_data(req, ctx->stream_id, 0);

		if (mtk_cam_feature_is_mstream(s_data->feature.raw_feature)) {
			/* report on first exp */
			s_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 1);
			if (dequeued_frame_seq_no == s_data->frame_seq_no) {
				dev_dbg(raw_dev->dev,
					"%s mstream [SOF] frame:%d sof:%d enque_req_cnt:%d\n",
					__func__, dequeued_frame_seq_no,
					req->p_data[ctx->stream_id].req_seq,
					ctx->enqueued_request_cnt);
				ctx->next_sof_mask_frame_seq_no =
					dequeued_frame_seq_no + 1;
				ctx->working_request_seq =
					req->p_data[ctx->stream_id].req_seq;
				mtk_cam_event_frame_sync(ctx->pipe,
					req->p_data[ctx->stream_id].req_seq);
			}
		} else {
			/* mstream 1exp case */
			ctx->working_request_seq =
				req->p_data[ctx->stream_id].req_seq;
			mtk_cam_event_frame_sync(ctx->pipe,
				req->p_data[ctx->stream_id].req_seq);
		}
	} else if (dequeued_frame_seq_no ==
			ctx->next_sof_mask_frame_seq_no) {
		/* when frame request is already remove sof_done case */
		dev_dbg(raw_dev->dev, "mstream [SOF-mask] frame:%d\n",
			ctx->next_sof_mask_frame_seq_no);
	} else {
		/* except: keep report current working request sequence */
		dev_dbg(raw_dev->dev, "mstream [SOF] frame:%d\n",
			ctx->working_request_seq);
		mtk_cam_event_frame_sync(ctx->pipe,
				ctx->working_request_seq);
	}
}

static void mtk_camsys_raw_frame_start(struct mtk_raw_device *raw_dev,
				       struct mtk_cam_ctx *ctx,
				       unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_cam_working_buf_entry *buf_entry;
	struct mtk_camsys_ctrl_state *current_state;
	dma_addr_t base_addr;
	enum MTK_CAMSYS_STATE_RESULT state_handle_ret;
	bool is_apply = false;

	/* inner register dequeue number */
	if (!mtk_cam_is_stagger(ctx))
		ctx->dequeued_frame_seq_no = dequeued_frame_seq_no;
	/* Send V4L2_EVENT_FRAME_SYNC event */
	if (mtk_cam_is_mstream(ctx) || ctx->next_sof_mask_frame_seq_no != 0) {
		mtk_cam_mstream_frame_sync(raw_dev, ctx, dequeued_frame_seq_no);
	} else {
		/* normal */
		mtk_cam_event_frame_sync(ctx->pipe, dequeued_frame_seq_no);
	}

	/* Find request of this dequeued frame */
	req_stream_data = mtk_cam_get_req_s_data(ctx, ctx->stream_id, dequeued_frame_seq_no);
	/* Detect no frame done and trigger camsys dump for debugging */
	mtk_cam_debug_detect_dequeue_failed(req_stream_data, 10);
	if (ctx->sensor) {
		if (mtk_cam_is_subsample(ctx))
			state_handle_ret =
			mtk_camsys_raw_subspl_state_handle(raw_dev, sensor_ctrl,
						&current_state,
						dequeued_frame_seq_no);
		else
			state_handle_ret =
			mtk_camsys_raw_state_handle(raw_dev, sensor_ctrl,
						&current_state,
						dequeued_frame_seq_no);
		if (state_handle_ret != STATE_RESULT_TRIGGER_CQ) {
			dev_dbg(raw_dev->dev, "[SOF] CQ drop s:%d deq:%d\n",
				state_handle_ret, dequeued_frame_seq_no);
			return;
		}
	}
	/* Update CQ base address if needed */
	if (ctx->composed_frame_seq_no <= dequeued_frame_seq_no) {
		dev_info(raw_dev->dev,
			"SOF[ctx:%d-#%d], CQ isn't updated [composed_frame_deq (%d) ]\n",
			ctx->stream_id, dequeued_frame_seq_no,
			ctx->composed_frame_seq_no);
		return;
	}
	/* apply next composed buffer */
	spin_lock(&ctx->composed_buffer_list.lock);
	if (list_empty(&ctx->composed_buffer_list.list)) {
		dev_info(raw_dev->dev,
			"SOF_INT_ST, no buffer update, cq_num:%d, frame_seq:%d\n",
			ctx->composed_frame_seq_no, dequeued_frame_seq_no);
		spin_unlock(&ctx->composed_buffer_list.lock);
	} else {
		is_apply = true;
		buf_entry = list_first_entry(&ctx->composed_buffer_list.list,
					     struct mtk_cam_working_buf_entry,
					     list_entry);
		list_del(&buf_entry->list_entry);
		ctx->composed_buffer_list.cnt--;
		spin_unlock(&ctx->composed_buffer_list.lock);
		spin_lock(&ctx->processing_buffer_list.lock);
		list_add_tail(&buf_entry->list_entry,
			      &ctx->processing_buffer_list.list);
		ctx->processing_buffer_list.cnt++;
		spin_unlock(&ctx->processing_buffer_list.lock);
		base_addr = buf_entry->buffer.iova;
		apply_cq(raw_dev,
			base_addr,
			buf_entry->cq_desc_size, buf_entry->cq_desc_offset, 0,
			buf_entry->sub_cq_desc_size, buf_entry->sub_cq_desc_offset);
		/* Transit state from Sensor -> CQ */
		if (ctx->sensor) {
			if (mtk_cam_is_subsample(ctx))
				state_transition(current_state,
				E_STATE_SUBSPL_READY, E_STATE_SUBSPL_SCQ);
			else
				state_transition(current_state,
				E_STATE_SENSOR, E_STATE_CQ);

			/* req_stream_data of req_cq*/
			req_stream_data = mtk_cam_ctrl_state_to_req_s_data(current_state);
			req_stream_data->state.time_cqset = ktime_get_boottime_ns() / 1000;
			dev_dbg(raw_dev->dev,
			"SOF[ctx:%d-#%d], CQ-%d is update, composed:%d, cq_addr:0x%x, time:%lld, monotime:%lld\n",
			ctx->stream_id, dequeued_frame_seq_no, req_stream_data->frame_seq_no,
			ctx->composed_frame_seq_no, base_addr, req_stream_data->timestamp,
			req_stream_data->timestamp_mono);
		}
	}
	if (ctx->used_sv_num && is_apply) {
		if (mtk_cam_sv_apply_next_buffer(ctx) == 0)
			dev_info(raw_dev->dev, "sv apply next buffer failed");
	}
}

int mtk_cam_hdr_last_frame_start(struct mtk_raw_device *raw_dev,
			struct mtk_cam_ctx *ctx, int deque_frame_no)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_temp;
	struct mtk_camsys_ctrl_state *state_switch = NULL;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int stateidx;
	int que_cnt = 0;
	unsigned long flags;

	sensor_ctrl->ctx->dequeued_frame_seq_no = deque_frame_no;

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_temp);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		stateidx = sensor_ctrl->sensor_request_seq_no -
			   req_stream_data->frame_seq_no;
		if (stateidx < STATE_NUM_AT_SOF && stateidx > -1) {
			/* Find switch element for switch request*/
			if (state_temp->estate > E_STATE_SENSOR &&
			    state_temp->estate < E_STATE_CAMMUX_OUTER &&
			    req_stream_data->feature.switch_feature_type) {
				state_switch = state_temp;
			}
			dev_dbg(ctx->cam->dev,
			"[%s] STATE_CHECK [N-%d] Req:%d / State:%d\n",
			__func__, stateidx, req_stream_data->frame_seq_no, state_temp->estate);
		}
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
	/*1-exp - as normal mode*/
	if (!raw_dev->stagger_en && !state_switch) {
		mtk_camsys_raw_frame_start(raw_dev, ctx, deque_frame_no);
		return 0;
	}
	/*HDR to Normal cam mux switch case timing will be at last sof*/
	if (state_switch) {
		req = mtk_cam_ctrl_state_get_req(state_switch);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		mtk_camsys_exp_switch_cam_mux(raw_dev, ctx, req_stream_data);
		dev_info(ctx->cam->dev,
			"[%s] switch Req:%d / State:%d\n",
			__func__, req_stream_data->frame_seq_no, state_switch->estate);
	}

	return 0;
}

// TODO(mstream): check mux switch case
static void mtk_cam_handle_mux_switch(struct mtk_raw_device *raw_dev,
				   struct mtk_cam_ctx *ctx,
				   struct mtk_cam_request *req)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_cam_request_stream_data *req_stream_data;
	int cnt_need_change_mux_cq_done = 0;
	int cnt_need_change_mux = 0;
	struct mtk_cam_request_stream_data *stream_data_change[MTKCAM_SUBDEV_MAX];
	struct mtk_cam_seninf_mux_setting mux_settings[
			MTKCAM_SUBDEV_RAW_END - MTKCAM_SUBDEV_RAW_START];
	int i;

	if (!(req->ctx_used & cam->streaming_ctx & req->ctx_link_update))
		return;

	if (req->flags & MTK_CAM_REQ_FLAG_SENINF_CHANGED) {
		for (i = MTKCAM_SUBDEV_RAW_START; i < MTKCAM_SUBDEV_RAW_END; i++) {
			if ((1 << i) & req->ctx_link_update) {
				raw_dev = get_master_raw_dev(ctx->cam, cam->ctxs[i].pipe);
				enable_tg_db(raw_dev, 0);
				enable_tg_db(raw_dev, 1);
				toggle_db(raw_dev);
			}
		}
		return;
	}
	/* Check if all ctx is ready to change mux though double buffer */
	for (i = 0; i < cam->max_stream_num; i++) {
		if (1 << i & (req->ctx_used & cam->streaming_ctx & req->ctx_link_update)) {
			cnt_need_change_mux++;
			req_stream_data = mtk_cam_req_get_s_data(req, i, 0);
			if (req_stream_data->state.estate == E_STATE_OUTER) {
				stream_data_change[cnt_need_change_mux_cq_done] = req_stream_data;
				mux_settings[cnt_need_change_mux_cq_done].seninf =
					req_stream_data->seninf_new;
				mux_settings[cnt_need_change_mux_cq_done].source =
					i -  MTKCAM_SUBDEV_RAW_0 +	PAD_SRC_RAW0;
				mux_settings[cnt_need_change_mux_cq_done].camtg =
					i;
				cnt_need_change_mux_cq_done++;

			}
		}
	}

	if (!cnt_need_change_mux ||
		cnt_need_change_mux != cnt_need_change_mux_cq_done) {
		dev_dbg(raw_dev->dev,
				"%s:%s: No cam mux change,ctx_used(0x%x),link_update(0x%x),streaming_ctx(0x%x)\n",
				__func__, req->req.debug_str, req->ctx_used, req->ctx_link_update,
				cam->streaming_ctx);
		return;
	}

	for (i = 0 ; i < cnt_need_change_mux_cq_done; i++) {
		state_transition(&(stream_data_change[i]->state), E_STATE_OUTER,
			E_STATE_CAMMUX_OUTER_CFG);
	}

}

static void mtk_camsys_raw_m2m_cq_done(struct mtk_raw_device *raw_dev,
				   struct mtk_cam_ctx *ctx,
				   unsigned int frame_seq_no_outer)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_entry;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;

	if (frame_seq_no_outer == 1)
		stream_on(raw_dev, 1);

	dev_info(raw_dev->dev,
		"[M2M CQD] frame_seq_no_outer:%d composed_buffer_list.cnt:%d\n",
		frame_seq_no_outer, ctx->composed_buffer_list.cnt);

	spin_lock(&sensor_ctrl->camsys_state_lock);
	list_for_each_entry(state_entry, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_entry);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);

		if (req_stream_data->frame_seq_no == frame_seq_no_outer) {
			if (frame_seq_no_outer > sensor_ctrl->isp_request_seq_no) {
				/**
				 * outer number is 1 more from last SOF's
				 * inner number
				 */
				state_transition(state_entry, E_STATE_CQ,
						E_STATE_OUTER);

				req_stream_data->state.time_irq_outer =
					ktime_get_boottime_ns() / 1000;
				dev_dbg(raw_dev->dev,
					"[M2M CQD] req:%d, CQ->OUTER state:%d\n",
					req_stream_data->frame_seq_no, state_entry->estate);
			}
		}
	}
	spin_unlock(&sensor_ctrl->camsys_state_lock);
}

static void mtk_camsys_raw_cq_done(struct mtk_raw_device *raw_dev,
				   struct mtk_cam_ctx *ctx,
				   unsigned int frame_seq_no_outer)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_entry;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int i, type;

	/* initial CQ done */
	if (raw_dev->sof_count == 0) {
		req_stream_data = mtk_cam_get_req_s_data(ctx, ctx->stream_id, 1);
		req = req_stream_data->req;
		sensor_ctrl->initial_cq_done = 1;
		if (req_stream_data->state.estate >= E_STATE_SENSOR ||
			!ctx->sensor) {
			unsigned int hw_scen = mtk_raw_get_hdr_scen_id(ctx);

			spin_lock(&ctx->streaming_lock);
			if (ctx->streaming) {
				stream_on(raw_dev, 1);
				for (i = 0; i < ctx->used_sv_num; i++)
					mtk_cam_sv_dev_stream_on(ctx, i, 1, 1);

				for (i = MTKCAM_SUBDEV_CAMSV_END - 1;
					i >= MTKCAM_SUBDEV_CAMSV_START; i--) {
					if (mtk_cam_is_stagger(ctx) &&
						ctx->pipe->enabled_raw & (1 << i))
						mtk_cam_sv_dev_stream_on(
							ctx, i - MTKCAM_SUBDEV_CAMSV_START,
							1, hw_scen);
				}

			}
			spin_unlock(&ctx->streaming_lock);
		} else {
			dev_dbg(raw_dev->dev,
				"[CQD] 1st sensor not set yet, req:%d, state:%d\n",
				req_stream_data->frame_seq_no, req_stream_data->state.estate);
		}
	}
	/* Legacy CQ done will be always happened at frame done */
	spin_lock(&sensor_ctrl->camsys_state_lock);
	list_for_each_entry(state_entry, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_entry);
		req = req_stream_data->req;
		if (mtk_cam_is_subsample(ctx)) {
			state_transition(state_entry, E_STATE_SUBSPL_SCQ,
						E_STATE_SUBSPL_OUTER);
			state_transition(state_entry, E_STATE_SUBSPL_SCQ_DELAY,
						E_STATE_SUBSPL_OUTER);
			if (raw_dev->sof_count == 0)
				state_transition(state_entry, E_STATE_SUBSPL_READY,
						E_STATE_SUBSPL_OUTER);
			req_stream_data->state.time_irq_outer = ktime_get_boottime_ns() / 1000;
			dev_dbg(raw_dev->dev,
					"[CQD-subsample] req:%d, CQ->OUTER state:0x%x\n",
					req_stream_data->frame_seq_no, state_entry->estate);
		} else if (mtk_cam_is_time_shared(ctx)) {
			if (req_stream_data->frame_seq_no == frame_seq_no_outer &&
				frame_seq_no_outer > sensor_ctrl->isp_request_seq_no) {
				state_transition(state_entry, E_STATE_TS_CQ,
						E_STATE_TS_INNER);
				dev_info(raw_dev->dev, "[TS-SOF] ctx:%d sw trigger rawi_r2 req:%d->%d, state:0x%x\n",
						ctx->stream_id, ctx->dequeued_frame_seq_no,
						req_stream_data->frame_seq_no, state_entry->estate);
				ctx->dequeued_frame_seq_no = frame_seq_no_outer;
				writel_relaxed(RAWI_R2_TRIG, raw_dev->base + REG_CTL_RAWI_TRIG);
				raw_dev->sof_count++;
				wmb(); /* TBC */
			}
		} else if (req_stream_data->frame_seq_no == frame_seq_no_outer) {
			if (frame_seq_no_outer > sensor_ctrl->isp_request_seq_no) {
				/**
				 * outer number is 1 more from last SOF's
				 * inner number
				 */
				state_transition(state_entry, E_STATE_CQ,
						E_STATE_OUTER);
				state_transition(state_entry,
						E_STATE_CQ_SCQ_DELAY,
						E_STATE_OUTER);
				req_stream_data->state.time_irq_outer =
						ktime_get_boottime_ns() / 1000;
				type = req_stream_data->feature.switch_feature_type;
				if (type != 0 && (!mtk_cam_is_mstream(ctx) &&
						!mtk_cam_feature_change_is_mstream(type))) {
					if (type == EXPOSURE_CHANGE_3_to_1 ||
						type == EXPOSURE_CHANGE_2_to_1)
						stagger_disable(raw_dev);
					else if (type == EXPOSURE_CHANGE_1_to_2 ||
						type == EXPOSURE_CHANGE_1_to_3)
						stagger_enable(raw_dev);
					dev_dbg(raw_dev->dev,
						"[CQD-switch] req:%d type:%d\n",
						req_stream_data->frame_seq_no, type);
				}
				dev_dbg(raw_dev->dev,
					"[CQD] req:%d, CQ->OUTER state:%d\n",
					req_stream_data->frame_seq_no, state_entry->estate);
				mtk_cam_handle_mux_switch(raw_dev, ctx, req);
			}
		}
	}
	spin_unlock(&sensor_ctrl->camsys_state_lock);
}

static void mtk_camsys_raw_m2m_trigger(struct mtk_raw_device *raw_dev,
				   struct mtk_cam_ctx *ctx,
				   unsigned int frame_seq_no_outer)
{

	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_entry;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;

	if (!(raw_dev->pipeline->feature_active & MTK_CAM_FEATURE_STAGGER_M2M_MASK))
		return;

	trigger_rawi(raw_dev);

	spin_lock(&sensor_ctrl->camsys_state_lock);
	list_for_each_entry(state_entry, &sensor_ctrl->camsys_state_list,
			    state_element) {
		req = mtk_cam_ctrl_state_get_req(state_entry);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		if (req_stream_data->frame_seq_no == frame_seq_no_outer) {
			/**
			 * outer number is 1 more from last SOF's
			 * inner number
			 */
			sensor_ctrl->isp_request_seq_no = frame_seq_no_outer;
			state_transition(state_entry, E_STATE_OUTER,
					E_STATE_INNER);
			req_stream_data->state.time_irq_sof1 = ktime_get_boottime_ns() / 1000;
			dev_dbg(raw_dev->dev,
				"[SW Trigger] req:%d, M2M CQ->INNER state:%d frame_seq_no:%d\n",
				req_stream_data->frame_seq_no, state_entry->estate,
				frame_seq_no_outer);
		}
	}
	spin_unlock(&sensor_ctrl->camsys_state_lock);

}

static bool
mtk_cam_raw_prepare_mstream_frame_done(struct mtk_cam_ctx *ctx,
				       struct mtk_cam_request_stream_data *req_stream_data)
{
	unsigned int frame_undone;
	struct mtk_cam_device *cam;
	struct mtk_cam_request_stream_data *s_data;
	struct mtk_cam_request *state_req;
	struct mtk_camsys_ctrl_state *state_entry;

	cam = ctx->cam;
	state_req = mtk_cam_s_data_get_req(req_stream_data);
	state_entry = &req_stream_data->state;
	s_data = mtk_cam_req_get_s_data(state_req, ctx->stream_id, 0);
	if (mtk_cam_feature_is_mstream(s_data->feature.raw_feature)) {
		if (req_stream_data->frame_seq_no == s_data->frame_seq_no)
			frame_undone = 0;
		else
			frame_undone = 1;

		dev_dbg(cam->dev,
			"[mstream][SWD] req:%d/state:%d/time:%lld/sync_id:%lld/frame_undone:%d\n",
			req_stream_data->frame_seq_no, state_entry->estate,
			req_stream_data->timestamp, state_req->sync_id,
			frame_undone);

		if (frame_undone)
			return false;
	} else {
		dev_dbg(cam->dev, "[mstream][SWD] req:%d/state:%d/time:%lld/sync_id:%lld\n",
			req_stream_data->frame_seq_no, state_entry->estate,
			req_stream_data->timestamp,
			state_req->sync_id);
	}

	return true;
}

static bool
mtk_camsys_raw_prepare_frame_done(struct mtk_raw_device *raw_dev,
				  struct mtk_cam_ctx *ctx,
				  unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct mtk_camsys_sensor_ctrl *camsys_sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_entry;
	struct mtk_cam_request *state_req;
	struct mtk_cam_request_stream_data *s_data;
	unsigned long flags;

	if (!ctx->sensor) {
		dev_info(cam->dev, "%s: no sensor found in ctx:%d, req:%d",
			 __func__, ctx->stream_id, dequeued_frame_seq_no);

		return true;
	}

	spin_lock_irqsave(&camsys_sensor_ctrl->camsys_state_lock, flags);
	/**
	 * Find inner register number's request and transit to
	 * STATE_DONE_xxx
	 */
	list_for_each_entry(state_entry,
			    &camsys_sensor_ctrl->camsys_state_list,
			    state_element) {
		state_req = mtk_cam_ctrl_state_get_req(state_entry);
		s_data = mtk_cam_ctrl_state_to_req_s_data(state_entry);
		if (s_data->frame_seq_no == dequeued_frame_seq_no) {
			if (mtk_cam_is_subsample(ctx)) {
				state_transition(state_entry,
						 E_STATE_SUBSPL_INNER,
						 E_STATE_SUBSPL_DONE_NORMAL);
				if (state_entry->estate == E_STATE_SUBSPL_DONE_NORMAL)
					s_data->state.time_irq_done =
						ktime_get_boottime_ns() / 1000;
				dev_dbg(cam->dev, "[SWD-subspl] req:%d/state:0x%x/time:%lld\n",
					s_data->frame_seq_no, state_entry->estate,
					s_data->timestamp);
			} else if (mtk_cam_is_time_shared(ctx)) {
				state_transition(state_entry,
						 E_STATE_TS_INNER,
						 E_STATE_TS_DONE_NORMAL);
				if (state_entry->estate == E_STATE_TS_DONE_NORMAL)
					s_data->state.time_irq_done =
						ktime_get_boottime_ns() / 1000;
					dev_dbg(cam->dev, "[TS-SWD] ctx:%d req:%d/state:0x%x/time:%lld\n",
						ctx->stream_id, s_data->frame_seq_no,
						state_entry->estate, s_data->timestamp);
			} else {
				state_transition(state_entry,
						 E_STATE_INNER_HW_DELAY,
						 E_STATE_DONE_MISMATCH);
				state_transition(state_entry, E_STATE_INNER,
						 E_STATE_DONE_NORMAL);
				if (state_entry->estate == E_STATE_DONE_NORMAL)
					s_data->state.time_irq_done =
						ktime_get_boottime_ns() / 1000;
				if (camsys_sensor_ctrl->isp_request_seq_no == 0)
					state_transition(state_entry,
							 E_STATE_CQ,
							 E_STATE_OUTER);

				/* mstream 2 and 1 exposure */
				if (mtk_cam_feature_is_mstream(s_data->feature.raw_feature) ||
				    ctx->next_sof_mask_frame_seq_no != 0) {
					if (!mtk_cam_raw_prepare_mstream_frame_done
						(ctx, s_data)) {
						spin_unlock_irqrestore
							(&camsys_sensor_ctrl->camsys_state_lock,
							 flags);
						return false;
					}
				} else {
					dev_dbg(cam->dev,
						"[SWD] req:%d/state:%d/time:%lld/sync_id:%lld\n",
						s_data->frame_seq_no,
						state_entry->estate,
						s_data->timestamp,
						state_req->sync_id);
				}

				// if (state_req->sync_id != -1)
				//	imgsys_cmdq_setevent(state_req->sync_id);
			}
		}
	}
	spin_unlock_irqrestore(&camsys_sensor_ctrl->camsys_state_lock, flags);

	return true;
}

static void
mtk_camsys_raw_change_pipeline(struct mtk_raw_device *raw_dev,
			       struct mtk_cam_ctx *ctx,
			       struct mtk_camsys_sensor_ctrl *sensor_ctrl,
			       unsigned int dequeued_frame_seq_no)
{
	int i;
	struct mtk_cam_device *cam = raw_dev->cam;
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int frame_seq = dequeued_frame_seq_no + 1;

	req = mtk_cam_get_req(ctx, frame_seq);

	if (!req) {
		dev_dbg(raw_dev->dev, "%s next req (%d) not queued\n", __func__, frame_seq);
		return;
	}

	if (!req->ctx_link_update) {
		dev_dbg(raw_dev->dev, "%s next req (%d) no link stup\n", __func__, frame_seq);
		return;
	}

	dev_dbg(raw_dev->dev, "%s:req(%d) check: req->ctx_used:0x%x, req->ctx_link_update0x%x\n",
		__func__, frame_seq, req->ctx_used, req->ctx_link_update);

	/* Check if all ctx is ready to change link */
	for (i = 0; i < cam->max_stream_num; i++) {
		if ((req->ctx_used & 1 << i) && (req->ctx_link_update & (1 << i))) {
			/**
			 * Switch cammux double buffer write delay, we have to disable the
			 * mux (mask the data and sof to raw) and than switch it.
			 */
			req_stream_data = mtk_cam_req_get_s_data(req, i, 0);
			if (req_stream_data->state.estate == E_STATE_CAMMUX_OUTER_CFG_DELAY) {
				/**
				 * To be move to the start of frame done hanlding
				 * INIT_WORK(&req->link_work, mtk_cam_link_change_worker);
				 * queue_work(cam->link_change_wq, &req->link_work);
				 */
				dev_info(raw_dev->dev, "Exchange streams at req(%d), update link ctx (0x%x)\n",
				frame_seq, ctx->stream_id, req->ctx_link_update);
				mtk_cam_req_seninf_change(req);
				return;
			}
		}
	}
	dev_info(raw_dev->dev, "%s:req(%d) no link update data found!\n",
		__func__, frame_seq);

}

static void mtk_cam_handle_frame_done(struct mtk_cam_ctx *ctx,
				      unsigned int frame_seq_no,
				      unsigned int pipe_id)
{
	struct mtk_raw_device *raw_dev = NULL;
	bool need_dequeue;
	unsigned long flags;

	/**
	 * If ctx is already off, just return; mtk_cam_dev_req_cleanup()
	 * triggered by mtk_cam_vb2_stop_streaming() puts the all media
	 * requests back.
	 */
	spin_lock_irqsave(&ctx->streaming_lock, flags);
	if (!ctx->streaming) {
		dev_dbg(ctx->cam->dev,
			 "%s: skip frame done for stream off ctx:%d\n",
			 __func__, ctx->stream_id);
		spin_unlock_irqrestore(&ctx->streaming_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&ctx->streaming_lock, flags);

	if (is_camsv_subdev(pipe_id)) {
		need_dequeue = true;
	} else {
		raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
		need_dequeue =
			mtk_camsys_raw_prepare_frame_done(raw_dev, ctx,
							  frame_seq_no);
	}

	if (!need_dequeue)
		return;

	dev_info(ctx->cam->dev, "[%s] job done ctx-%d:pipe-%d:req(%d)\n",
		 __func__, ctx->stream_id, pipe_id, frame_seq_no);
	if (mtk_cam_dequeue_req_frame(ctx, frame_seq_no, pipe_id)) {
		mutex_lock(&ctx->cam->op_lock);
		mtk_cam_dev_req_try_queue(ctx->cam);
		mutex_unlock(&ctx->cam->op_lock);
		if (is_raw_subdev(pipe_id))
			mtk_camsys_raw_change_pipeline(raw_dev, ctx,
						       &ctx->sensor_ctrl,
						       frame_seq_no);
	}
}

void mtk_cam_meta1_done_work(struct work_struct *work)
{
	struct mtk_cam_req_work *meta1_done_work = (struct mtk_cam_req_work *)work;
	struct mtk_cam_request_stream_data *s_data, *s_data_ctx;
	struct mtk_cam_ctx *ctx;
	struct mtk_cam_request *req;
	struct mtk_cam_buffer *buf;
	struct vb2_buffer *vb;
	struct mtk_cam_video_device *node;
	void *vaddr;

	s_data = mtk_cam_req_work_get_s_data(meta1_done_work);
	ctx = mtk_cam_s_data_get_ctx(s_data);
	req = mtk_cam_s_data_get_req(s_data);
	s_data_ctx = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);

	dev_dbg(ctx->cam->dev, "%s: ctx:%d\n", __func__, ctx->stream_id);

	spin_lock(&ctx->streaming_lock);
	if (!ctx->streaming) {
		spin_unlock(&ctx->streaming_lock);
		dev_info(ctx->cam->dev, "%s: skip for stream off ctx:%d\n",
			 __func__, ctx->stream_id);
		return;
	}
	spin_unlock(&ctx->streaming_lock);

	if (!s_data) {
		dev_info(ctx->cam->dev,
			 "%s:ctx(%d): can't get s_data\n",
			 __func__, ctx->stream_id);
		return;
	}

	/* Copy the meta1 output content to user buffer */
	buf = mtk_cam_s_data_get_vbuf(s_data, MTK_RAW_META_OUT_1);
	if (!buf) {
		dev_info(ctx->cam->dev,
			 "ctx(%d): can't get MTK_RAW_META_OUT_1 buf from req(%d)\n",
			 ctx->stream_id, s_data->frame_seq_no);
		return;
	}

	vb = &buf->vbb.vb2_buf;
	if (!vb) {
		dev_info(ctx->cam->dev,
			 "%s:ctx(%d): can't get vb2 buf\n",
			 __func__, ctx->stream_id);
		return;
	}

	node = mtk_cam_vbq_to_vdev(vb->vb2_queue);

	vaddr = vb2_plane_vaddr(&buf->vbb.vb2_buf, 0);
	if (!vaddr) {
		dev_info(ctx->cam->dev,
			 "%s:ctx(%d): can't get plane_vadd\n",
			 __func__, ctx->stream_id);
		return;
	}

	if (!s_data->working_buf->meta_buffer.size) {
		dev_info(ctx->cam->dev,
			 "%s:ctx(%d): can't get s_data working buf\n",
			 __func__, ctx->stream_id);
		return;
	}

	memcpy(vaddr, s_data->working_buf->meta_buffer.va,
	       s_data->working_buf->meta_buffer.size);

	spin_lock(&node->buf_list_lock);
	list_del(&buf->list);
	spin_unlock(&node->buf_list_lock);

	/* Update the timestamp for the buffer*/
	mtk_cam_s_data_update_timestamp(ctx, buf, s_data_ctx);

	/* clean the stream data for req reinit case */
	mtk_cam_s_data_reset_vbuf(s_data, MTK_RAW_META_OUT_1);

	/* Let user get the buffer */
	vb2_buffer_done(&buf->vbb.vb2_buf, VB2_BUF_STATE_DONE);

	/* clear workstate*/
	atomic_set(&meta1_done_work->is_queued, 0);

	dev_info(ctx->cam->dev, "%s:%s: req(%d) done\n",
		 __func__, req->req.debug_str, s_data->frame_seq_no);
}

void mtk_cam_sv_work(struct work_struct *work)
{
	struct mtk_cam_req_work *sv_work = (struct mtk_cam_req_work *)work;
	struct mtk_cam_request_stream_data *s_data;
	struct mtk_cam_ctx *ctx;
	struct device *dev_sv;
	struct mtk_camsv_device *camsv_dev;
	unsigned int seq_no;
	dma_addr_t base_addr;

	s_data = mtk_cam_req_work_get_s_data(sv_work);
	ctx = mtk_cam_s_data_get_ctx(s_data);
	seq_no = s_data->frame_seq_no;
	base_addr = s_data->sv_frame_params.img_out.buf[0][0].iova;
	dev_sv = ctx->cam->sv.devs[s_data->pipe_id - MTKCAM_SUBDEV_CAMSV_START];
	camsv_dev = dev_get_drvdata(dev_sv);
	mtk_cam_sv_enquehwbuf(camsv_dev, base_addr, seq_no);
}

static void mtk_cam_meta1_done(struct mtk_cam_ctx *ctx,
			       unsigned int frame_seq_no,
			       unsigned int pipe_id)
{
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_req_work *meta1_done_work;
	struct mtk_camsys_sensor_ctrl *camsys_sensor_ctrl = &ctx->sensor_ctrl;

	req = mtk_cam_get_req(ctx, frame_seq_no);
	if (!req) {
		dev_info(ctx->cam->dev, "%s:ctx-%d:pipe-%d:req(%d) not found!\n",
			 __func__, ctx->stream_id, pipe_id, frame_seq_no);
		return;
	}

	req_stream_data = mtk_cam_req_get_s_data(req, pipe_id, 0);
	if (!req_stream_data) {
		dev_info(ctx->cam->dev, "%s:ctx-%d:pipe-%d:s_data not found!\n",
			 __func__, ctx->stream_id, pipe_id);
		return;
	}

	if (!(req_stream_data->flags & MTK_CAM_REQ_S_DATA_FLAG_META1_INDEPENDENT))
		return;

	/* Initial request readout will be delayed 1 frame*/
	if (ctx->sensor) {
		if (camsys_sensor_ctrl->isp_request_seq_no == 0 &&
			(!req_stream_data->feature.raw_feature)) {
			dev_info(ctx->cam->dev,
				 "1st META1 done passed for initial request setting\n");
			return;
		}
	}

	meta1_done_work = &req_stream_data->meta1_done_work;
	atomic_set(&meta1_done_work->is_queued, 1);
	queue_work(ctx->frame_done_wq, &meta1_done_work->work);
}

void mtk_cam_frame_done_work(struct work_struct *work)
{
	struct mtk_cam_req_work *frame_done_work = (struct mtk_cam_req_work *)work;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_cam_ctx *ctx;

	req_stream_data = mtk_cam_req_work_get_s_data(frame_done_work);
	ctx = mtk_cam_s_data_get_ctx(req_stream_data);
	mtk_cam_handle_frame_done(ctx,
				  req_stream_data->frame_seq_no,
				  req_stream_data->pipe_id);
}

void mtk_camsys_frame_done(struct mtk_cam_ctx *ctx,
				  unsigned int frame_seq_no,
				  unsigned int pipe_id)
{
	struct mtk_cam_request *req;
	struct mtk_cam_req_work *frame_done_work;
	struct mtk_camsys_sensor_ctrl *camsys_sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_raw_device *raw_dev;
	int i;
	int switch_type;
	int feature;

	if (mtk_cam_is_stagger(ctx) && is_raw_subdev(pipe_id)) {
		req = mtk_cam_get_req(ctx, frame_seq_no + 1);
		if (req) {
			req_stream_data = mtk_cam_req_get_s_data(req, pipe_id, 0);
			switch_type = req_stream_data->feature.switch_feature_type;
			feature = req_stream_data->feature.raw_feature;
			if (switch_type &&
			    !mtk_cam_feature_change_is_mstream(switch_type)) {
				mtk_cam_hdr_switch_toggle(ctx, feature);
				dev_dbg(ctx->cam->dev,
					"[SWD] switch req toggle check req:%d type:%d\n",
					req_stream_data->frame_seq_no,
					switch_type);
			}
		}
	}

	/* Initial request readout will be delayed 1 frame*/
	if (ctx->sensor) {
		if (camsys_sensor_ctrl->isp_request_seq_no == 0 &&
			(!ctx->pipe->feature_active)) {
			dev_info(ctx->cam->dev,
					"1st SWD passed for initial request setting\n");
			if (ctx->stream_id == pipe_id)
				camsys_sensor_ctrl->initial_drop_frame_cnt--;
			return;
		}
	}

	req_stream_data = mtk_cam_get_req_s_data(ctx, pipe_id, frame_seq_no);
	if (req_stream_data) {
		req = mtk_cam_s_data_get_req(req_stream_data);
	} else {
		dev_dbg(ctx->cam->dev, "%s:ctx-%d:pipe-%d:req(%d) not found!\n",
			 __func__, ctx->stream_id, pipe_id, frame_seq_no);
		return;
	}

	if (req_stream_data->frame_done_queue_work) {
		dev_info(ctx->cam->dev,
			"already queue done work %d\n", req_stream_data->frame_seq_no);
		return;
	}

	atomic_set(&req_stream_data->seninf_dump_state, MTK_CAM_REQ_DBGWORK_S_FINISHED);
	req_stream_data->frame_done_queue_work = 1;
	frame_done_work = &req_stream_data->frame_done_work;
	queue_work(ctx->frame_done_wq, &frame_done_work->work);
	if (mtk_cam_is_time_shared(ctx)) {
		raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
		raw_dev->time_shared_busy = false;
		/*try set other ctx in one request first*/
		if (req->pipe_used != (1 << ctx->stream_id)) {
			struct mtk_cam_ctx *ctx_2 = NULL;
			int pipe_used_remain = req->pipe_used & (~(1 << ctx->stream_id));

			for (i = 0;  i < ctx->cam->max_stream_num; i++)
				if (pipe_used_remain == (1 << i)) {
					ctx_2 = &ctx->cam->ctxs[i];
					break;
				}

			if (!ctx_2) {
				dev_dbg(raw_dev->dev, "%s: time sharing ctx-%d deq_no(%d)\n",
				 __func__, ctx_2->stream_id, ctx_2->dequeued_frame_seq_no+1);
				mtk_camsys_ts_raw_try_set(raw_dev, ctx_2,
								ctx_2->dequeued_frame_seq_no + 1);
			}
		}
		mtk_camsys_ts_raw_try_set(raw_dev, ctx, ctx->dequeued_frame_seq_no + 1);
	}
}


void mtk_camsys_state_delete(struct mtk_cam_ctx *ctx,
				struct mtk_camsys_sensor_ctrl *sensor_ctrl,
				struct mtk_cam_request *req)
{
	struct mtk_camsys_ctrl_state *state_entry, *state_entry_prev;
	struct mtk_cam_request_stream_data *s_data;
	struct mtk_camsys_ctrl_state *req_state;
	struct mtk_cam_request *req_tmp;
	int state_found = 0;

	if (ctx->sensor) {
		spin_lock(&sensor_ctrl->camsys_state_lock);
		list_for_each_entry_safe(state_entry, state_entry_prev,
				&sensor_ctrl->camsys_state_list,
				state_element) {
			s_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
			req_state = &s_data->state;

			if (state_entry == req_state) {
				list_del(&state_entry->state_element);
				/* Decrease the request ref count for camsys_state_list's usage */
				req_tmp = mtk_cam_ctrl_state_get_req(state_entry);
				media_request_put(&req_tmp->req);
				state_found = 1;
			}

			if (mtk_cam_feature_is_mstream(s_data->feature.raw_feature)) {
				s_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 1);
				req_state = &s_data->state;
				if (state_entry == req_state) {
					list_del(&state_entry->state_element);
					state_found = 1;
				}
			}
		}
		spin_unlock(&sensor_ctrl->camsys_state_lock);
		if (state_found == 0)
			dev_dbg(ctx->cam->dev, "state not found\n");
	}
}

static int mtk_camsys_camsv_state_handle(
		struct mtk_camsv_device *camsv_dev,
		struct mtk_camsys_sensor_ctrl *sensor_ctrl,
		struct mtk_camsys_ctrl_state **current_state,
		int frame_inner_idx)
{
	struct mtk_cam_ctx *ctx = sensor_ctrl->ctx;
	struct mtk_camsys_ctrl_state *state_temp, *state_outer = NULL;
	struct mtk_camsys_ctrl_state *state_rec[STATE_NUM_AT_SOF];
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	int stateidx;
	int que_cnt = 0;
	unsigned long flags;

	/* List state-queue status*/
	spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry(state_temp,
	&sensor_ctrl->camsys_state_list, state_element) {
		req = mtk_cam_ctrl_state_get_req(state_temp);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		stateidx = sensor_ctrl->sensor_request_seq_no -
			   req_stream_data->frame_seq_no;

		if (stateidx < STATE_NUM_AT_SOF && stateidx > -1) {
			state_rec[stateidx] = state_temp;
			/* Find outer state element */
			if (state_temp->estate == E_STATE_OUTER ||
				state_temp->estate == E_STATE_OUTER_HW_DELAY)
				state_outer = state_temp;
			dev_dbg(camsv_dev->dev,
				"[SOF] STATE_CHECK [N-%d] Req:%d / State:%d\n",
				stateidx, req_stream_data->frame_seq_no,
				state_rec[stateidx]->estate);
		}
		/* counter for state queue*/
		que_cnt++;
	}
	spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);

	/* HW imcomplete case */
	if (que_cnt >= STATE_NUM_AT_SOF) {
		state_transition(state_rec[2], E_STATE_INNER, E_STATE_INNER_HW_DELAY);
		state_transition(state_rec[1], E_STATE_OUTER, E_STATE_OUTER_HW_DELAY);
		dev_dbg(camsv_dev->dev, "[SOF] HW_DELAY state\n");
		return STATE_RESULT_PASS_CQ_HW_DELAY;
	}

	/* Trigger high resolution timer to try sensor setting */
	mtk_cam_sof_timer_setup(ctx);

	/* Transit outer state to inner state */
	if (state_outer != NULL) {
		req = mtk_cam_ctrl_state_get_req(state_outer);
		req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
		if (req_stream_data->frame_seq_no == frame_inner_idx) {
			if (frame_inner_idx == (sensor_ctrl->isp_request_seq_no + 1)) {
				state_transition(state_outer,
					E_STATE_OUTER_HW_DELAY, E_STATE_INNER_HW_DELAY);
				state_transition(state_outer, E_STATE_OUTER, E_STATE_INNER);
				sensor_ctrl->isp_request_seq_no = frame_inner_idx;
				dev_dbg(camsv_dev->dev, "[SOF-DBLOAD] req:%d, OUTER->INNER state:%d\n",
						req_stream_data->frame_seq_no, state_outer->estate);
			}
		}
	}

	if (que_cnt > 0) {
		/* CQ triggering judgment*/
		if (state_rec[0]->estate == E_STATE_SENSOR) {
			*current_state = state_rec[0];
			return STATE_RESULT_TRIGGER_CQ;
		}
	}

	return STATE_RESULT_PASS_CQ_SW_DELAY;
}

static void mtk_camsys_camsv_check_frame_done(struct mtk_cam_ctx *ctx,
	unsigned int dequeued_frame_seq_no, unsigned int pipe_id)
{
#define CHECK_STATE_DEPTH 3
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_temp;
	struct mtk_cam_request_stream_data *req_stream_data;
	unsigned long flags;
	unsigned int seqList[CHECK_STATE_DEPTH];
	unsigned int cnt = 0;
	int i;

	if (ctx->sensor) {
		spin_lock_irqsave(&sensor_ctrl->camsys_state_lock, flags);
		list_for_each_entry(state_temp, &sensor_ctrl->camsys_state_list,
						state_element) {
			req_stream_data = mtk_cam_ctrl_state_to_req_s_data(state_temp);
			if (req_stream_data->frame_seq_no < dequeued_frame_seq_no) {
				seqList[cnt++] = req_stream_data->frame_seq_no;
				if (cnt == CHECK_STATE_DEPTH)
					break;
			}
		}
		spin_unlock_irqrestore(&sensor_ctrl->camsys_state_lock, flags);
		for (i = 0; i < cnt; i++)
			mtk_camsys_frame_done(ctx, seqList[i], pipe_id);
	}
}

static void mtk_camsys_camsv_frame_start(struct mtk_camsv_device *camsv_dev,
	struct mtk_cam_ctx *ctx, unsigned int dequeued_frame_seq_no)
{
	struct mtk_cam_request *req;
	struct mtk_cam_request_stream_data *req_stream_data;
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *current_state;
	enum MTK_CAMSYS_STATE_RESULT state_handle_ret;
	int sv_dev_index;

	/* inner register dequeue number */
	sv_dev_index = mtk_cam_find_sv_dev_index(ctx, camsv_dev->id);
	if (sv_dev_index == -1) {
		dev_dbg(camsv_dev->dev, "cannot find sv_dev_index(%d)", camsv_dev->id);
		return;
	}
	ctx->sv_dequeued_frame_seq_no[sv_dev_index] = dequeued_frame_seq_no;
	/* Send V4L2_EVENT_FRAME_SYNC event */
	mtk_cam_sv_event_frame_sync(camsv_dev, dequeued_frame_seq_no);

	mtk_camsys_camsv_check_frame_done(ctx, dequeued_frame_seq_no,
		camsv_dev->id + MTKCAM_SUBDEV_CAMSV_START);

	if (ctx->sensor &&
		(ctx->stream_id >= MTKCAM_SUBDEV_CAMSV_START &&
		ctx->stream_id < MTKCAM_SUBDEV_CAMSV_END)) {
		state_handle_ret = mtk_camsys_camsv_state_handle(camsv_dev, sensor_ctrl,
				&current_state, dequeued_frame_seq_no);
		if (state_handle_ret != STATE_RESULT_TRIGGER_CQ)
			return;
	}

	/* Find request of this dequeued frame */
	if (ctx->stream_id >= MTKCAM_SUBDEV_CAMSV_START &&
		ctx->stream_id < MTKCAM_SUBDEV_CAMSV_END) {
		req = mtk_cam_get_req(ctx, dequeued_frame_seq_no);
		if (req) {
			req_stream_data = mtk_cam_req_get_s_data(req, ctx->stream_id, 0);
			req_stream_data->timestamp = ktime_get_boottime_ns();
			req_stream_data->timestamp_mono = ktime_get_ns();
		}
	}

	/* apply next buffer */
	if (ctx->stream_id >= MTKCAM_SUBDEV_CAMSV_START &&
		ctx->stream_id < MTKCAM_SUBDEV_CAMSV_END) {
		if (mtk_cam_sv_apply_next_buffer(ctx)) {
			/* Transit state from Sensor -> Outer */
			if (ctx->sensor)
				state_transition(current_state, E_STATE_SENSOR, E_STATE_OUTER);
		} else {
			dev_info(camsv_dev->dev, "sv apply next buffer failed");
		}
	}
}

int mtk_camsys_isr_event(struct mtk_cam_device *cam,
			 struct mtk_camsys_irq_info *irq_info)
{
	struct mtk_raw_device *raw_dev;
	struct mtk_camsv_device *camsv_dev;
	struct mtk_cam_ctx *ctx;
	int sub_engine_type = irq_info->engine_id & MTK_CAMSYS_ENGINE_IDXMASK;
	int ret = 0;
	int sv_dev_index;
	unsigned int stream_id;
	unsigned int seq;

	/**
	 * Here it will be implemented dispatch rules for some scenarios
	 * like twin/stagger/m-stream,
	 * such cases that camsys will collect all coworked sub-engine's
	 * signals and trigger some engine of them to do some job
	 * individually.
	 * twin - rawx2
	 * stagger - rawx1, camsv x2
	 * m-stream - rawx1 , camsv x2
	 */
	switch (sub_engine_type) {
	case MTK_CAMSYS_ENGINE_RAW_TAG:
		raw_dev = cam->camsys_ctrl.raw_dev[irq_info->engine_id -
			CAMSYS_ENGINE_RAW_BEGIN];
		if (raw_dev->pipeline->feature_active & MTK_CAM_FEATURE_TIMESHARE_MASK)
			ctx = &cam->ctxs[raw_dev->time_shared_busy_ctx_id];
		else
			ctx = mtk_cam_find_ctx(cam, &raw_dev->pipeline->subdev.entity);
		if (!ctx) {
			dev_dbg(raw_dev->dev, "cannot find ctx\n");
			ret = -EINVAL;
			break;
		}
		/* Twin: skip all */
		if (irq_info->slave_engine)
			return ret;

		/* raw's CQ done */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_SETTING_DONE)) {
			if (ctx->pipe->feature_active & MTK_CAM_FEATURE_STAGGER_M2M_MASK) {
				mtk_camsys_raw_m2m_cq_done(raw_dev, ctx, irq_info->frame_idx);
				mtk_camsys_raw_m2m_trigger(raw_dev, ctx, irq_info->frame_idx);
			} else
				mtk_camsys_raw_cq_done(raw_dev, ctx, irq_info->frame_idx);
		}
		/* raw's subsample sensor setting */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_SUBSAMPLE_SENSOR_SET))
			mtk_cam_set_sub_sample_sensor(raw_dev, ctx);

		/* raw's DMA done, we only allow AFO done here */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_AFO_DONE)) {
			mtk_cam_meta1_done(ctx, ctx->dequeued_frame_seq_no, ctx->stream_id);
		}

		/* raw's SW done */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_FRAME_DONE)) {
			if (ctx->pipe->feature_active & MTK_CAM_FEATURE_STAGGER_M2M_MASK) {
				mtk_camsys_raw_m2m_frame_done(raw_dev, ctx,
						   irq_info->frame_inner_idx);
			} else
				mtk_camsys_frame_done(ctx, ctx->dequeued_frame_seq_no,
					ctx->stream_id);
		}
		/* raw's SOF */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_FRAME_START)) {
			if (mtk_cam_is_stagger(ctx)) {
				dev_dbg(raw_dev->dev, "[stagger] last frame_start\n");
				mtk_cam_hdr_last_frame_start(raw_dev, ctx,
					irq_info->frame_inner_idx);
			} else {
				mtk_camsys_raw_frame_start(raw_dev, ctx,
						  irq_info->frame_inner_idx);
			}
		}
		break;
	case MTK_CAMSYS_ENGINE_MRAW_TAG:
		/* struct mtk_mraw_device *mraw_dev; */
		break;
	case MTK_CAMSYS_ENGINE_CAMSV_TAG:
		camsv_dev = cam->camsys_ctrl.camsv_dev[irq_info->engine_id -
			CAMSYS_ENGINE_CAMSV_BEGIN];
		if (camsv_dev->pipeline->hw_scen &
			MTK_CAMSV_SUPPORTED_SPECIAL_HW_SCENARIO) {
			struct mtk_raw_pipeline *pipeline = &cam->raw
			.pipelines[camsv_dev->pipeline->master_pipe_id];
			struct mtk_raw_device *raw_dev =
				get_master_raw_dev(cam, pipeline);
			struct mtk_cam_ctx *ctx =
				mtk_cam_find_ctx(cam, &pipeline->subdev.entity);

			dev_dbg(camsv_dev->dev, "sv special hw scenario: %d/%d/%d\n",
				camsv_dev->pipeline->master_pipe_id,
				raw_dev->id, ctx->stream_id);

			// first exposure camsv's SOF
			if (irq_info->irq_type & (1 << CAMSYS_IRQ_FRAME_START)) {
				if (camsv_dev->pipeline->exp_order == 0)
					mtk_camsys_raw_frame_start(raw_dev, ctx,
						   irq_info->frame_inner_idx);
				else if (camsv_dev->pipeline->exp_order == 2)
					mtk_cam_hdr_last_frame_start(raw_dev, ctx,
						   irq_info->frame_inner_idx);
			}
			// time sharing - camsv write DRAM mode
			if (camsv_dev->pipeline->hw_scen &
				(1 << MTKCAM_IPI_HW_PATH_OFFLINE_M2M)) {
				if (irq_info->irq_type & (1<<CAMSYS_IRQ_FRAME_DONE)) {
					mtk_camsys_ts_sv_done(ctx, irq_info->frame_inner_idx);
					mtk_camsys_ts_raw_try_set(
						raw_dev, ctx, ctx->dequeued_frame_seq_no + 1);
				}
				if (irq_info->irq_type & (1<<CAMSYS_IRQ_FRAME_START)) {
					mtk_camsys_ts_frame_start(ctx, irq_info->frame_inner_idx);
				}
			}
		} else {
			ctx = mtk_cam_find_ctx(cam, &camsv_dev->pipeline->subdev.entity);
			if (!ctx) {
				dev_dbg(camsv_dev->dev, "cannot find ctx\n");
				ret = -EINVAL;
				break;
			}
			stream_id = irq_info->engine_id - CAMSYS_ENGINE_CAMSV_BEGIN +
				MTKCAM_SUBDEV_CAMSV_START;
			/* camsv's SW done */
			if (irq_info->irq_type & (1<<CAMSYS_IRQ_FRAME_DONE)) {
				sv_dev_index = mtk_cam_find_sv_dev_index(ctx, camsv_dev->id);
				if (sv_dev_index == -1) {
					dev_dbg(camsv_dev->dev,
						"cannot find sv_dev_index(%d)", camsv_dev->id);
					ret = -EINVAL;
					break;
				}
				seq = ctx->sv_dequeued_frame_seq_no[sv_dev_index];
				mtk_camsys_frame_done(ctx, seq, stream_id);
			}
			/* camsv's SOF */
			if (irq_info->irq_type & (1<<CAMSYS_IRQ_FRAME_START))
				mtk_camsys_camsv_frame_start(camsv_dev, ctx,
					irq_info->frame_inner_idx);
		}
		break;
	case MTK_CAMSYS_ENGINE_SENINF_TAG:
		/* ToDo - cam mux setting delay handling */
		if (irq_info->irq_type & (1 << CAMSYS_IRQ_FRAME_DROP))
			dev_info(cam->dev, "MTK_CAMSYS_ENGINE_SENINF_TAG engine:%d type:0x%x\n",
				irq_info->engine_id, irq_info->irq_type);
		break;
	default:
		break;
	}

	return ret;
}

void mtk_cam_mstream_initial_sensor_setup(struct mtk_cam_request *initial_req,
				  struct mtk_cam_ctx *ctx)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_cam_request_stream_data *req_stream_data =
		mtk_cam_req_get_s_data(initial_req, ctx->stream_id, 1);
	sensor_ctrl->ctx = ctx;
	req_stream_data->state.time_swirq_timer = ktime_get_boottime_ns() / 1000;
	mtk_cam_set_sensor(req_stream_data, sensor_ctrl);
	dev_info(ctx->cam->dev, "[mstream] Initial sensor timer setup, seq_no(%d)\n",
				req_stream_data->frame_seq_no);
}

void mtk_cam_initial_sensor_setup(struct mtk_cam_request *initial_req,
				  struct mtk_cam_ctx *ctx)
{
	struct mtk_camsys_sensor_ctrl *sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_cam_request_stream_data *req_stream_data;

	sensor_ctrl->ctx = ctx;
	req_stream_data = mtk_cam_req_get_s_data(initial_req, ctx->stream_id, 0);
	req_stream_data->state.time_swirq_timer =
		ktime_get_boottime_ns() / 1000;
	mtk_cam_set_sensor(req_stream_data, sensor_ctrl);
	if (mtk_cam_is_subsample(ctx))
		state_transition(&req_stream_data->state,
			E_STATE_READY, E_STATE_SUBSPL_READY);
	dev_info(ctx->cam->dev, "Initial sensor timer setup\n");
}

void mtk_cam_req_ctrl_setup(struct mtk_raw_pipeline *raw_pipe,
			    struct mtk_cam_request *req)
{
	struct mtk_cam_request_stream_data *req_stream_data;
	struct media_request_object *obj;
	struct v4l2_ctrl_handler *parent_hdl;

	req_stream_data = mtk_cam_req_get_s_data(req, raw_pipe->id, 0);

	/* Setup raw pipeline's ctrls */
	list_for_each_entry(obj, &req->req.objects, list) {
		if (likely(obj))
			parent_hdl = obj->priv;
		else
			continue;
		if (parent_hdl == &raw_pipe->ctrl_handler) {
			dev_dbg(raw_pipe->subdev.v4l2_dev->dev,
				"%s:%s:%s:raw ctrl set start (seq:%d)\n",
				__func__, raw_pipe->subdev.name, req->req.debug_str,
				req_stream_data->frame_seq_no);
			v4l2_ctrl_request_setup(&req->req, parent_hdl);
		}
	}
}

static int timer_reqdrained_chk(int fps_ratio, int sub_sample)
{
	int timer_ms = 0;

	if (sub_sample > 0) {
		if (fps_ratio > 1)
			timer_ms = SENSOR_SET_DEADLINE_MS;
		else
			timer_ms = SENSOR_SET_DEADLINE_MS * fps_ratio;
	} else {
		if (fps_ratio > 1)
			timer_ms = SENSOR_SET_DEADLINE_MS / fps_ratio;
		else
			timer_ms = SENSOR_SET_DEADLINE_MS;
	}
	/* earlier request drained event*/
	if (sub_sample == 0 && fps_ratio > 1)
		timer_ms = timer_ms > 8 ? 8 : timer_ms;

	return timer_ms;
}
static int timer_setsensor(int fps_ratio, int sub_sample)
{
	int timer_ms = 0;

	if (sub_sample > 0) {
		if (fps_ratio > 1)
			timer_ms = SENSOR_SET_RESERVED_MS;
		else
			timer_ms = SENSOR_SET_RESERVED_MS * fps_ratio;
	} else {
		if (fps_ratio > 1)
			timer_ms = SENSOR_SET_RESERVED_MS / fps_ratio;
		else
			timer_ms = SENSOR_SET_RESERVED_MS;
	}
	/* faster sensor setting*/
	if (sub_sample == 0 && fps_ratio > 1)
		timer_ms = timer_ms > 3 ? 3 : timer_ms;

	return timer_ms;
}

int mtk_camsys_ctrl_start(struct mtk_cam_ctx *ctx)
{
	struct mtk_camsys_ctrl *camsys_ctrl = &ctx->cam->camsys_ctrl;
	struct mtk_camsys_sensor_ctrl *camsys_sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_raw_device *raw_dev, *raw_dev_slave, *raw_dev_slave2;
	struct v4l2_subdev_frame_interval fi;
	unsigned int i;
	int fps_factor = 1, sub_ratio = 0;

	if (ctx->used_raw_num) {
		fi.pad = 0;
		v4l2_set_frame_interval_which(fi, V4L2_SUBDEV_FORMAT_ACTIVE);
		v4l2_subdev_call(ctx->sensor, video, g_frame_interval, &fi);
		fps_factor = (fi.interval.numerator > 0) ?
				(fi.interval.denominator / fi.interval.numerator / 30) : 1;
		raw_dev = get_master_raw_dev(ctx->cam, ctx->pipe);
		camsys_ctrl->raw_dev[raw_dev->id] = raw_dev;
		if (ctx->pipe->res_config.raw_num_used != 1) {
			raw_dev_slave = get_slave_raw_dev(ctx->cam, ctx->pipe);
			camsys_ctrl->raw_dev[raw_dev_slave->id] = raw_dev_slave;
			if (ctx->pipe->res_config.raw_num_used == 3) {
				raw_dev_slave2 = get_slave2_raw_dev(ctx->cam, ctx->pipe);
				camsys_ctrl->raw_dev[raw_dev_slave2->id] = raw_dev_slave2;
			}
		}
		sub_ratio =
			mtk_cam_get_subsample_ratio(ctx->pipe->res_config.raw_feature);
	}
	for (i = 0; i < ctx->used_sv_num; i++) {
		camsys_ctrl->camsv_dev[ctx->sv_pipe[i]->id -
			MTKCAM_SUBDEV_CAMSV_START] =
			dev_get_drvdata(ctx->cam->sv.devs[ctx->sv_pipe[i]->id -
			MTKCAM_SUBDEV_CAMSV_START]);
	}
	camsys_sensor_ctrl->ctx = ctx;
	camsys_sensor_ctrl->sensor_request_seq_no = 0;
	camsys_sensor_ctrl->isp_request_seq_no = 0;
	camsys_sensor_ctrl->initial_cq_done = 0;
	camsys_sensor_ctrl->sof_time = 0;
	if (ctx->used_raw_num) {
		if (!ctx->pipe->feature_active)
			camsys_sensor_ctrl->initial_drop_frame_cnt = INITIAL_DROP_FRAME_CNT;
		else
			camsys_sensor_ctrl->initial_drop_frame_cnt = 0;
	}

	camsys_sensor_ctrl->timer_req_event =
		timer_reqdrained_chk(fps_factor, sub_ratio);
	camsys_sensor_ctrl->timer_req_sensor =
		timer_setsensor(fps_factor, sub_ratio);
	INIT_LIST_HEAD(&camsys_sensor_ctrl->camsys_state_list);
	spin_lock_init(&camsys_sensor_ctrl->camsys_state_lock);
	if (ctx->sensor) {
		hrtimer_init(&camsys_sensor_ctrl->sensor_deadline_timer,
			     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		camsys_sensor_ctrl->sensor_deadline_timer.function =
			sensor_deadline_timer_handler;
		camsys_sensor_ctrl->sensorsetting_wq =
			alloc_ordered_workqueue(dev_name(ctx->cam->dev),
						WQ_HIGHPRI | WQ_FREEZABLE);
		if (!camsys_sensor_ctrl->sensorsetting_wq) {
			dev_dbg(ctx->cam->dev,
				"failed to alloc sensor setting workqueue\n");
			return -ENOMEM;
		}
	}

	dev_info(ctx->cam->dev, "[%s] ctx:%d/raw_dev:0x%x drained/sensor (%d)%d/%d\n",
		__func__, ctx->stream_id, ctx->used_raw_dev, fps_factor,
		camsys_sensor_ctrl->timer_req_event, camsys_sensor_ctrl->timer_req_sensor);

	return 0;
}

void mtk_camsys_ctrl_stop(struct mtk_cam_ctx *ctx)
{
	struct mtk_camsys_sensor_ctrl *camsys_sensor_ctrl = &ctx->sensor_ctrl;
	struct mtk_camsys_ctrl_state *state_entry, *state_entry_prev;
	struct mtk_cam_request *req;
	unsigned long flags;

	spin_lock_irqsave(&camsys_sensor_ctrl->camsys_state_lock, flags);
	list_for_each_entry_safe(state_entry, state_entry_prev,
				 &camsys_sensor_ctrl->camsys_state_list,
				 state_element) {
		list_del(&state_entry->state_element);
		/* Decrease the request ref count for camsys_state_list's usage */
		req = mtk_cam_ctrl_state_get_req(state_entry);
		media_request_put(&req->req);
	}
	spin_unlock_irqrestore(&camsys_sensor_ctrl->camsys_state_lock, flags);
	if (ctx->sensor) {
		hrtimer_cancel(&camsys_sensor_ctrl->sensor_deadline_timer);
		drain_workqueue(camsys_sensor_ctrl->sensorsetting_wq);
		destroy_workqueue(camsys_sensor_ctrl->sensorsetting_wq);
		camsys_sensor_ctrl->sensorsetting_wq = NULL;
	}
	if (ctx->used_raw_num)
		mtk_cam_event_eos(ctx->pipe);
	dev_info(ctx->cam->dev, "[%s] ctx:%d/raw_dev:0x%x\n",
		__func__, ctx->stream_id, ctx->used_raw_dev);
}

void mtk_cam_m2m_enter_cq_state(struct mtk_camsys_ctrl_state *ctrl_state)
{
	state_transition(ctrl_state, E_STATE_SENSOR, E_STATE_CQ);
}
