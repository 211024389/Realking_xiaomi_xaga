/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef _MTK_DRM_PANEL_HELPER_H_
#define _MTK_DRM_PANEL_HELPER_H_

#include <linux/list.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_device.h>
#include <drm/drm_connector.h>
#include "mtk_drm_gateic.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "mtk_round_corner/mtk_drm_rc.h"

extern unsigned long long mtk_lcm_total_size;

#define MTK_LCM_NAME_LENGTH (128)
#define MTK_LCM_MODE_UNIT (4)
#define MTK_LCM_DEBUG_DUMP (0)

#define DO_LCM_KZALLOC(buf, size, flag, debug) \
do { \
	buf = kzalloc(roundup(size, 4), flag); \
	if (buf != NULL) \
		mtk_lcm_total_size += size; \
	if (debug == 1 && buf != NULL) \
		pr_notice("%s, %d, buf:0x%lx, size:%u, align:%u, flag:0x%x\n", \
			__func__, __LINE__, (unsigned long)buf, \
			(unsigned int)size, (unsigned int)roundup(size, 4), \
			(unsigned int)flag); \
} while (0)

#define LCM_KZALLOC(buf, size, flag) DO_LCM_KZALLOC(buf, size, flag, 0)

#define DO_LCM_KFREE(buf, size, debug) \
do { \
	if (debug == 1) \
		pr_notice("%s, %d, size:%u\n", \
			__func__, __LINE__, (unsigned int)roundup(size, 4)); \
	if (mtk_lcm_total_size >= roundup(size, 4)) \
		mtk_lcm_total_size -= roundup(size, 4); \
	else \
		mtk_lcm_total_size = 0; \
	kfree(buf); \
	buf = NULL; \
} while (0)

#define LCM_KFREE(buf, size) DO_LCM_KFREE(buf, size, 0)

struct mtk_lcm_params_dbi {
	unsigned int dbi_private_data;
};

struct mtk_lcm_params_dpi {
	unsigned int dpi_private_data;
};

struct mtk_lcm_mode_dsi {
/* key word */
	unsigned int id;
	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int voltage;
	struct list_head list;
/* params */
	struct drm_display_mode mode;
	struct mtk_panel_params ext_param;
/* ops */
	unsigned int fps_switch_bfoff_size;
	unsigned int fps_switch_afon_size;
	struct mtk_lcm_ops_data *fps_switch_bfoff;
	struct mtk_lcm_ops_data *fps_switch_afon;

};

struct mtk_lcm_params_dsi {
	unsigned int density;
	unsigned int fake_resolution[2];
	unsigned int need_fake_resolution;
	unsigned int phy_type;
	unsigned long long mode_flags;
	unsigned long long mode_flags_doze_on;
	unsigned long long mode_flags_doze_off;
	unsigned int format;
	unsigned int lanes;
	struct mtk_lcm_mode_dsi *default_mode;
	unsigned int mode_count;
	struct list_head mode_list;
	struct device lcm_gpio_dev;
	unsigned int lcm_pinctrl_count;
	const char **lcm_pinctrl_name;
};

struct mtk_lcm_params {
	const char *name;
	unsigned int type;
	unsigned int resolution[2];
	unsigned int physical_width;
	unsigned int physical_height;
	struct mtk_lcm_params_dbi dbi_params;
	struct mtk_lcm_params_dpi dpi_params;
	struct mtk_lcm_params_dsi dsi_params;
};

/* mtk_lcm_dcs_cmd_data
 * used to MTK_LCM_CMD_TYPE_WRITE_BUFFER, MTK_LCM_CMD_TYPE_READ_BUFFER
 * cmd: the read/write command or address
 * data: the write data buffer, or returned read buffer
 * data_len: the data buffer length
 * start_id: for read command the returned data will be saved at which index of out buffer
 */
struct mtk_lcm_dcs_cmd_data {
	u8 cmd;
	u8 *data;
	size_t data_len;
	unsigned int start_id;
};

/* mtk_lcm_gpio_data
 * used to MTK_LCM_GPIO_TYPE_MODE, MTK_LCM_GPIO_TYPE_OUT
 * gpio_id: gpio index
 * data: the settings of gpio
 */
struct mtk_lcm_gpio_data {
	u8 gpio_id;
	u8 data;
};

/* mtk_lcm_cb_id_data
 * used to MTK_LCM_CB_TYPE_xx
 * id: the input parameter index
 * buffer_data: the ddic command data
 */
struct mtk_lcm_cb_id_data {
	unsigned int id_count;
	u8 *id;
	unsigned int data_count;
	u8 *buffer_data;
};

/* mtk_lcm_buf_con_data
 * used to MTK_LCM_CMD_TYPE_WRITE_BUFFER_CONDITION
 * condition: the execution condition
 * name: the input data name
 * data: the ddic command data
 * data_len: the ddic command data count
 */
struct mtk_lcm_buf_con_data {
	u8 name;
	u8 condition;
	u8 *data;
	size_t data_len;
};

/* mtk_lcm_buf_runtime_data
 * used to MTK_LCM_CMD_TYPE_WRITE_BUFFER_RUNTIME_INPUT
 * id: the parameter index of runtime input
 * name: the input data name
 * data: the ddic command data
 * data_len: the ddic command data count
 */
struct mtk_lcm_buf_runtime_data {
	u8 name;
	u8 id;
	u8 *data;
	size_t data_len;
};

/* the union of lcm operation data*/
union mtk_lcm_ops_data_params {
	u8 *buffer_data;
	unsigned int util_data;
	struct mtk_lcm_dcs_cmd_data cmd_data;
	struct mtk_lcm_gpio_data gpio_data;
	struct mtk_lcm_cb_id_data cb_id_data;
	struct mtk_lcm_buf_con_data buf_con_data;
	struct mtk_lcm_buf_runtime_data buf_runtime_data;
	void *cust_data;
};

/* mtk_lcm_ops_data
 * used to save lcm operation cmd
 * func: MTK_LCM_FUNC_DSI/DBI/DPI
 * type: operation type from MTK_LCM_UTIL_TYPE_START to MTK_LCM_CUST_TYPE_END
 * size: the dts string length for parsing lcm operation
 * param: the parsing result of lcm operation params
 */
struct mtk_lcm_ops_data {
	unsigned int func;
	unsigned int type;
	unsigned int size;
	union mtk_lcm_ops_data_params param;
};

/* mtk_lcm_ops_dbi
 * used to save the dbi operation list
 */
struct mtk_lcm_ops_dbi {
	unsigned int dbi_private_data;
};

/* mtk_lcm_ops_dpi
 * used to save the dpi operation list
 */
struct mtk_lcm_ops_dpi {
	unsigned int dpi_private_data;
};

/* mtk_lcm_ops_dsi
 * used to save the dsi operation list
 * xx_size: the operation count of xx function
 * xx: the operation data list of xx function
 */
struct mtk_lcm_ops_dsi {
	/* panel init & deinit */
	unsigned int prepare_size;
	struct mtk_lcm_ops_data *prepare;
	unsigned int unprepare_size;
	struct mtk_lcm_ops_data *unprepare;
	unsigned int enable_size;
	struct mtk_lcm_ops_data *enable;
	unsigned int disable_size;
	struct mtk_lcm_ops_data *disable;

	/* panel backlight update*/
	unsigned int set_backlight_mask;
	unsigned int set_backlight_cmdq_size;
	struct mtk_lcm_ops_data *set_backlight_cmdq;
	unsigned int set_backlight_grp_cmdq_size;
	struct mtk_lcm_ops_data *set_backlight_grp_cmdq;

	unsigned int set_aod_light_mask;
	unsigned int set_aod_light_size;
	struct mtk_lcm_ops_data *set_aod_light;

	/* panel aod check*/
	unsigned int ata_check_size;
	unsigned int ata_id_value_length;
	u8 *ata_id_value_data;
	struct mtk_lcm_ops_data *ata_check;

#ifdef MTK_PANEL_SUPPORT_COMPARE_ID
	/* panel compare id check*/
	unsigned int compare_id_size;
	unsigned int compare_id_value_length;
	u8 *compare_id_value_data;
	struct mtk_lcm_ops_data *compare_id;
#endif

	/* doze feature support*/
	unsigned int doze_enable_start_size;
	struct mtk_lcm_ops_data *doze_enable_start;
	unsigned int doze_enable_size;
	struct mtk_lcm_ops_data *doze_enable;
	unsigned int doze_disable_size;
	struct mtk_lcm_ops_data *doze_disable;
	unsigned int doze_area_size;
	struct mtk_lcm_ops_data *doze_area;
	unsigned int doze_post_disp_on_size;
	struct mtk_lcm_ops_data *doze_post_disp_on;

	/* hight backligth mode feature support*/
	unsigned int hbm_set_cmdq_size;
	unsigned int hbm_set_cmdq_switch_on;
	unsigned int hbm_set_cmdq_switch_off;
	struct mtk_lcm_ops_data *hbm_set_cmdq;

#if MTK_LCM_DEBUG_DUMP
	unsigned int gpio_test_size;
	struct mtk_lcm_ops_data *gpio_test;
#endif
};

struct mtk_lcm_ops {
	struct mtk_lcm_ops_dbi *dbi_ops;
	struct mtk_lcm_ops_dpi *dpi_ops;
	struct mtk_lcm_ops_dsi *dsi_ops;
};

struct mtk_lcm_ops_input {
	u8 name;
	unsigned int length;
	void *data;
};

struct mtk_lcm_ops_input_packet {
	unsigned int data_count;
	unsigned int condition_count;
	struct mtk_lcm_ops_input *data;
	struct mtk_lcm_ops_input *condition;
};

/* customization callback of private panel operation
 * parse_params:
 *      used to save panel parameters parsed from dtsi
 * parse_ops:
 *      used to save panel operation cmd list parsed from dtsi
 * func:
 *      used to execute the customized operation cmd
 * dump:
 *      used to dump the customized settings in params (optional)
 * free:
 *      used to deallocate the memory buffer of panel parsing result
 */
struct mtk_panel_cust {
	atomic_t cust_enabled;
	int (*parse_params)(struct device_node *np);
	int (*parse_ops)(unsigned int func,
		int type, u8 *data_in, size_t size_in,
		void *cust_data);
	int (*func)(struct mtk_lcm_ops_data *op,
		struct mtk_lcm_ops_input_packet *input);
	void (*dump_params)(void);
	void (*dump_ops)(struct mtk_lcm_ops_data *op,
		const char *owner, unsigned int id);
	void (*free_ops)(unsigned int func);
	void (*free_params)(unsigned int func);
};

struct mtk_panel_resource {
	unsigned int version;
	struct mtk_lcm_params params;
	struct mtk_lcm_ops ops;
	struct mtk_panel_cust cust;
};

int load_panel_resource_from_dts(struct device_node *lcm_np,
		struct mtk_panel_resource *data);

int parse_lcm_ops_func(struct device_node *np,
		struct mtk_lcm_ops_data *table,
		char *func, unsigned int size,
		unsigned int panel_type,
		struct mtk_panel_cust *cust, unsigned int phase);

/* function: execute lcm operations
 * input: table: lcm operation list
 *        table_size: lcm operation count
 *        panel_resource: lcm parameters
 *        data: the private data buffer of lcm operation
 *        size: the private data buffer size of lcm operation
 *        owner: the owner description
 * output: 0 for success, else of failed
 */
int mtk_panel_execute_operation(void *dev,
		struct mtk_lcm_ops_data *table, unsigned int table_size,
		const struct mtk_panel_resource *panel_resource,
		struct mtk_lcm_ops_input_packet *input, char *owner);

void mtk_lcm_dts_read_u32(struct device_node *np, char *prop,
		u32 *out);
int mtk_lcm_dts_read_u32_array(struct device_node *np, char *prop,
		u32 *out, int min_len, int max_len);
void mtk_lcm_dts_read_u8(struct device_node *np, char *prop,
		u8 *out);
int mtk_lcm_dts_read_u8_array(struct device_node *np, char *prop,
		u8 *out, int min_len, int max_len);

/* function: parse lcm parameters
 * input: fdt: dts, nodeoffset: dts node,
 *        params:the returned parsing result
 * output: 0 for success, else of failed
 */
int parse_lcm_params_dbi(struct device_node *np,
		struct mtk_lcm_params_dbi *params);
int parse_lcm_params_dpi(struct device_node *np,
		struct mtk_lcm_params_dpi *params);
int parse_lcm_params_dsi(struct device_node *np,
		struct mtk_lcm_params_dsi *params);

/* function: parse lcm operations
 * input: fdt: dts, nodeoffset: dts node,
 *        ops:the returned parsing result
 *        params: the lcm parameters already parsed
 * output: 0 for success, else of failed
 */
int parse_lcm_ops_dbi(struct device_node *np,
		struct mtk_lcm_ops_dbi *ops,
		struct mtk_lcm_params_dbi *params,
		struct mtk_panel_cust *cust);
int parse_lcm_ops_dpi(struct device_node *np,
		struct mtk_lcm_ops_dpi *ops,
		struct mtk_lcm_params_dpi *params,
		struct mtk_panel_cust *cust);
int parse_lcm_ops_dsi(struct device_node *np,
		struct mtk_lcm_ops_dsi *ops,
		struct mtk_lcm_params_dsi *params,
		struct mtk_panel_cust *cust);

void free_lcm_params_dbi(struct mtk_lcm_params_dbi *params);
void free_lcm_params_dpi(struct mtk_lcm_params_dpi *params);
void free_lcm_params_dsi(struct mtk_lcm_params_dsi *params);
void free_lcm_ops_dbi(struct mtk_lcm_ops_dbi *ops);
void free_lcm_ops_dpi(struct mtk_lcm_ops_dpi *ops);
void free_lcm_ops_dsi(struct mtk_lcm_ops_dsi *ops);

/* function: dump dts settings of lcm driver*/
void dump_lcm_ops_func(struct mtk_lcm_ops_data *table,
		unsigned int size,
		struct mtk_panel_cust *cust,
		const char *owner);
void dump_lcm_dsi_fps_settings(struct mtk_lcm_mode_dsi *mode);
void dump_lcm_params_basic(struct mtk_lcm_params *params);
void dump_lcm_params_dsi(struct mtk_lcm_params_dsi *params,
		struct mtk_panel_cust *cust);
void dump_lcm_ops_dsi(struct mtk_lcm_ops_dsi *ops,
		struct mtk_lcm_params_dsi *params,
		struct mtk_panel_cust *cust);
void dump_lcm_params_dbi(struct mtk_lcm_params_dbi *params,
		struct mtk_panel_cust *cust);
void dump_lcm_ops_dbi(struct mtk_lcm_ops_dbi *ops,
		struct mtk_lcm_params_dbi *params,
		struct mtk_panel_cust *cust);
void dump_lcm_params_dpi(struct mtk_lcm_params_dpi *params,
		struct mtk_panel_cust *cust);
void dump_lcm_ops_dpi(struct mtk_lcm_ops_dpi *ops,
		struct mtk_lcm_params_dpi *params,
		struct mtk_panel_cust *cust);
void mtk_lcm_dump_all(char func, struct mtk_panel_resource *resource,
		struct mtk_panel_cust *cust);

/* function: dsi ddic write
 * input: data: the data buffer
 * output: 0 for success, else for failed
 */
int mtk_panel_dsi_dcs_write_buffer(struct mipi_dsi_device *dsi,
		const void *data, size_t len);
int mtk_panel_dsi_dcs_write(struct mipi_dsi_device *dsi,
		u8 cmd, void *data, size_t len);
int mtk_panel_dsi_dcs_read(struct mipi_dsi_device *dsi,
		u8 cmd, void *data, size_t len);

/* function: dsi ddic write
 * input: data: the returned data buffer
 * output: 0 for success, else for failed
 */
int mtk_panel_dsi_dcs_read_buffer(struct mipi_dsi_device *dsi,
		const void *data_in, size_t len_in,
		void *data_out, size_t len_out);

/* function: free lcm operation data
 * input: operation cmd list, and size
 */
void free_lcm_ops_table(struct mtk_lcm_ops_data *table,
		unsigned int table_size);
void free_lcm_resource(char func, struct mtk_panel_resource *data);

/* function: create an input package
 * input: the input package address, the data count and condition count
 * output: 0 for success, else for failed
 */
int mtk_lcm_create_input_packet(struct mtk_lcm_ops_input_packet *input,
		unsigned int data_count, unsigned int condition_count);

/* function: destroy an input package
 * input: the input package address, the data count and condition count
 */
void mtk_lcm_destroy_input_packet(struct mtk_lcm_ops_input_packet *input);

/* function: create an input data
 * input: the input data address and data length
 *       name: the input data name
 * output: 0 for success, else for failed
 */
int mtk_lcm_create_input(struct mtk_lcm_ops_input *input,
		unsigned int data_len, u8 name);

/* function: destroy an input data
 * input: the input package address and data length
 */
void mtk_lcm_destroy_input(struct mtk_lcm_ops_input *input);
#endif
