/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2019 MediaTek Inc.

#ifndef __MTK_CAM_SENINF_HW_H__
#define __MTK_CAM_SENINF_HW_H__

//#include <linux/irqreturn.h>
#include <linux/interrupt.h>

struct mtk_cam_seninf_mux_meter {
	u32 width;
	u32 height;
	u32 h_valid;
	u32 h_blank;
	u32 v_valid;
	u32 v_blank;
	s64 mipi_pixel_rate;
	s64 vb_in_us;
	s64 hb_in_us;
	s64 line_time_in_us;
};

extern int update_isp_clk(struct seninf_ctx *ctx);

struct mtk_cam_seninf_ops {
	int (*_init_iomem)(struct seninf_ctx *ctx,
			      void __iomem *if_base, void __iomem *ana_base);
	int (*_init_port)(struct seninf_ctx *ctx, int port);
	int (*_is_cammux_used)(struct seninf_ctx *ctx, int cam_mux);
	int (*_cammux)(struct seninf_ctx *ctx, int cam_mux);
	int (*_disable_cammux)(struct seninf_ctx *ctx, int cam_mux);
	int (*_disable_all_cammux)(struct seninf_ctx *ctx);
	int (*_set_top_mux_ctrl)(struct seninf_ctx *ctx,
						int mux_idx, int seninf_src);
	int (*_get_top_mux_ctrl)(struct seninf_ctx *ctx, int mux_idx);
	int (*_get_cammux_ctrl)(struct seninf_ctx *ctx, int cam_mux);
	u32 (*_get_cammux_res)(struct seninf_ctx *ctx, int cam_mux);
	int (*_set_cammux_vc)(struct seninf_ctx *ctx, int cam_mux,
					 int vc_sel, int dt_sel, int vc_en, int dt_en);
	int (*_set_cammux_src)(struct seninf_ctx *ctx, int src,
					  int target, int exp_hsize, int exp_vsize);
	int (*_set_vc)(struct seninf_ctx *ctx, int seninfIdx,
				  struct seninf_vcinfo *vcinfo);
	int (*_set_mux_ctrl)(struct seninf_ctx *ctx, int mux,
					int hsPol, int vsPol, int src_sel,
					int pixel_mode);
	int (*_set_mux_crop)(struct seninf_ctx *ctx, int mux,
					int start_x, int end_x, int enable);
	int (*_is_mux_used)(struct seninf_ctx *ctx, int mux);
	int (*_mux)(struct seninf_ctx *ctx, int mux);
	int (*_disable_mux)(struct seninf_ctx *ctx, int mux);
	int (*_disable_all_mux)(struct seninf_ctx *ctx);
	int (*_set_cammux_chk_pixel_mode)(struct seninf_ctx *ctx,
							 int cam_mux, int pixelMode);
	int (*_set_test_model)(struct seninf_ctx *ctx,
					  int mux, int cam_mux, int pixelMode);
	int (*_set_csi_mipi)(struct seninf_ctx *ctx);
	int (*_poweroff)(struct seninf_ctx *ctx);
	int (*_reset)(struct seninf_ctx *ctx, int seninfIdx);
	int (*_set_idle)(struct seninf_ctx *ctx);
	int (*_get_mux_meter)(struct seninf_ctx *ctx, int mux,
					 struct mtk_cam_seninf_mux_meter *meter);
	ssize_t (*_show_status)(struct device *dev, struct device_attribute *attr, char *buf);
	int (*_switch_to_cammux_inner_page)(struct seninf_ctx *ctx, bool inner);
	int (*_set_cammux_next_ctrl)(struct seninf_ctx *ctx, int src, int target);
	int (*_update_mux_pixel_mode)(struct seninf_ctx *ctx, int mux, int pixel_mode);
	int (*_irq_handler)(int irq, void *data);
	int (*_set_sw_cfg_busy)(struct seninf_ctx *ctx, bool enable, int index);
	int (*_set_cam_mux_dyn_en)(struct seninf_ctx *ctx, bool enable, int cam_mux, int index);
	int (*_reset_cam_mux_dyn_en)(struct seninf_ctx *ctx, int index);
	int (*_enable_global_drop_irq)(struct seninf_ctx *ctx, bool enable, int index);
	int (*_enable_cam_mux_vsync_irq)(struct seninf_ctx *ctx, bool enable, int cam_mux);
	int (*_disable_all_cam_mux_vsync_irq)(struct seninf_ctx *ctx);
	int (*_debug)(struct seninf_ctx *ctx);
	unsigned char seninf_num;
	unsigned char mux_num;
	unsigned char cam_mux_num;

};

extern struct mtk_cam_seninf_ops mtk_csi_phy_3_0;
extern struct mtk_cam_seninf_ops mtk_csi_phy_2_0;
extern struct mtk_cam_seninf_ops *g_seninf_ops;

#endif
