// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef GBE_H
#define GBE_H
enum GBE_KICKER {
	KIR_GBE1,
	KIR_GBE2,
	KIR_NUM,
};
void gbe_boost(enum GBE_KICKER kicker, int boost);
void gbe_trace_printk(int pid, char *module, char *string);
void gbe_trace_count(int tid, unsigned long long bufID, int val, const char *fmt, ...);
int init_gbe_common(void);
void exit_gbe_common(void);
extern void (*gbe_get_cmd_fp)(int *cmd, int *value1, int *value2);

extern struct dentry *gbe_debugfs_dir;
#endif

