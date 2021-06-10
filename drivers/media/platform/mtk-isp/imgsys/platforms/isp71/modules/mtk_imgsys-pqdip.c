// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 *
 * Author: Daniel Huang <daniel.huang@mediatek.com>
 *
 */

 // Standard C header file

// kernel header file
#include <linux/device.h>
#include <linux/of_address.h>
#include <linux/dma-iommu.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc.h>

// mtk imgsys local header file

// Local header file
#include "mtk_imgsys-pqdip.h"

/********************************************************************
 * Global Define
 ********************************************************************/
#define PQDIP_HW_SET		2

#define PQDIP_BASE_ADDR		0x15140000
#define PQDIP_DMA_OFST		0x1000
#define PQDIP_PIMGI_OFST	0x1200
#define PQDIP_WROT1_OFST	0x2000
#define PQDIP_WROT2_OFST	0x2F00
#define PQDIP_OFST		0x10000

#define PQDIP_CTL_REG_CNT	0x300
#define PQDIP_DMA_REG_CNT	0x80
#define PQDIP_PIMGI_REG_CNT	0x120
#define PQDIP_WROT1_REG_CNT	0x100
#define PQDIP_WROT2_REG_CNT	0x40

/********************************************************************
 * Global Variable
 ********************************************************************/
const struct mtk_imgsys_init_array
			mtk_imgsys_pqdip_init_ary[] = {
	{0x0050, 0x80000000},	/* PQDIPCTL_P1A_REG_PQDIPCTL_INT1_EN */
	{0x0060, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_INT2_EN */
	{0x0070, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_CQ_INT1_EN */
	{0x0080, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_CQ_INT2_EN */
	{0x0090, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_CQ_INT3_EN */
	{0x00B0, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_PQDIP_DCM_DIS */
	{0x00B4, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_DMA_DCM_DIS */
	{0x00B8, 0x0},		/* PQDIPCTL_P1A_REG_PQDIPCTL_TOP_DCM_DIS */
	{0x0210, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR0_CTL */
	{0x0218, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR0_DESC_SIZE */
	{0x021C, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR1_CTL */
	{0x0224, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR1_DESC_SIZE */
	{0x0228, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR2_CTL */
	{0x0230, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR2_DESC_SIZE */
	{0x0234, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR3_CTL */
	{0x023C, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR3_DESC_SIZE */
	{0x0240, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR4_CTL */
	{0x0248, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR4_DESC_SIZE */
	{0x024C, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR5_CTL */
	{0x0254, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR5_DESC_SIZE */
	{0x0258, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR6_CTL */
	{0x0260, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR6_DESC_SIZE */
	{0x0264, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR7_CTL */
	{0x026C, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR7_DESC_SIZE */
	{0x0270, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR8_CTL */
	{0x0278, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR8_DESC_SIZE */
	{0x027C, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR9_CTL */
	{0x0284, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR9_DESC_SIZE */
	{0x0288, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR10_CTL */
	{0x0290, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR10_DESC_SIZE */
	{0x0294, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR11_CTL */
	{0x029C, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR11_DESC_SIZE */
	{0x02A0, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR12_CTL */
	{0x02A8, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR12_DESC_SIZE */
	{0x02AC, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR13_CTL */
	{0x02B4, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR13_DESC_SIZE */
	{0x02B8, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR14_CTL */
	{0x02C0, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR14_DESC_SIZE */
	{0x02C4, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR15_CTL */
	{0x02CC, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR15_DESC_SIZE */
	{0x02D0, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR16_CTL */
	{0x02D8, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR16_DESC_SIZE */
	{0x02DC, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR17_CTL */
	{0x02E4, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR17_DESC_SIZE */
	{0x02E8, 0x11},		/* DIPCQ_P1A_REG_DIPCQ_CQ_THR18_CTL */
	{0x02F0, 0x400},	/* DIPCQ_P1A_REG_DIPCQ_CQ_THR18_DESC_SIZE */
	{0x121C, 0x80000100},	/* PIMGI_P1A_REG_ORIRDMA_CON0 */
	{0x1220, 0x10AA008A},	/* PIMGI_P1A_REG_ORIRDMA_CON1 */
	{0x1224, 0x01000100},	/* PIMGI_P1A_REG_ORIRDMA_CON2 */
	{0x127C, 0x80000080},	/* PIMGBI_P1A_REG_ORIRDMA_CON0 */
	{0x1280, 0x10550045},	/* PIMGBI_P1A_REG_ORIRDMA_CON1 */
	{0x1284, 0x00800080},	/* PIMGBI_P1A_REG_ORIRDMA_CON2 */
	{0x12DC, 0x80000080},	/* PIMGCI_P1A_REG_ORIRDMA_CON0 */
	{0x12E0, 0x10550045},	/* PIMGCI_P1A_REG_ORIRDMA_CON1 */
	{0x12E4, 0x00800080}	/* PIMGCI_P1A_REG_ORIRDMA_CON2 */
};

#define PQDIP_INIT_ARRAY_COUNT	ARRAY_SIZE(mtk_imgsys_pqdip_init_ary)

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// Public Functions
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
void imgsys_pqdip_set_initial_value(struct mtk_imgsys_dev *imgsys_dev)
{
	void __iomem *pqdipRegBA = 0L;
	void __iomem *ofset = NULL;
	unsigned int hw_idx = 0;
	unsigned int i = 0;

	dev_dbg(imgsys_dev->dev, "%s: +\n", __func__);

	for (hw_idx = 0 ; hw_idx < PQDIP_HW_SET ; hw_idx++) {
		/* iomap registers */
		pqdipRegBA = of_iomap(imgsys_dev->dev->of_node, REG_MAP_E_PQDIP_A + hw_idx);
		for (i = 0 ; i < PQDIP_INIT_ARRAY_COUNT ; i++) {
			ofset = pqdipRegBA + mtk_imgsys_pqdip_init_ary[i].ofset;
			writel(mtk_imgsys_pqdip_init_ary[i].val, ofset);
		}
	iounmap(pqdipRegBA);
	}

	dev_dbg(imgsys_dev->dev, "%s: -\n", __func__);
}
EXPORT_SYMBOL(imgsys_pqdip_set_initial_value);

void imgsys_pqdip_debug_dump(struct mtk_imgsys_dev *imgsys_dev,
							unsigned int engine)
{
	void __iomem *pqdipRegBA = 0L;
	unsigned int hw_idx = 0;
	unsigned int i = 0;

	dev_info(imgsys_dev->dev, "%s: +\n", __func__);

	for (hw_idx = 0 ; hw_idx < PQDIP_HW_SET ; hw_idx++) {
		/* iomap registers */
		pqdipRegBA = of_iomap(imgsys_dev->dev->of_node, REG_MAP_E_PQDIP_A + hw_idx);
		if (!pqdipRegBA) {
			dev_info(imgsys_dev->dev, "%s of_iomap fail, devnode(%s).\n",
				__func__, imgsys_dev->dev->of_node->name);
		}

		/* PQ_DIP control registers */
		for (i = 0x0; i < PQDIP_CTL_REG_CNT; i += 0x10) {
			dev_info(imgsys_dev->dev, "%s:  [0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x",
			__func__, (unsigned int)(PQDIP_BASE_ADDR + (PQDIP_OFST * hw_idx) + i),
			(unsigned int)ioread32((void *)(pqdipRegBA + i + 0x00)),
			(unsigned int)ioread32((void *)(pqdipRegBA + i + 0x04)),
			(unsigned int)ioread32((void *)(pqdipRegBA + i + 0x08)),
			(unsigned int)ioread32((void *)(pqdipRegBA + i + 0x0c)));
		}

		/* PQ_DIP dma registers */
		for (i = 0; i < PQDIP_DMA_REG_CNT; i += 0x10) {
			dev_info(imgsys_dev->dev, "%s:  [0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x",
			__func__, (unsigned int)(PQDIP_BASE_ADDR + (PQDIP_OFST * hw_idx)
				+ PQDIP_DMA_OFST + i),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_DMA_OFST + i + 0x00)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_DMA_OFST + i + 0x04)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_DMA_OFST + i + 0x08)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_DMA_OFST + i + 0x0c)));
		}

		/* PQ_DIP PIMGI registers */
		for (i = 0; i < PQDIP_PIMGI_REG_CNT; i += 0x10) {
			dev_info(imgsys_dev->dev, "%s:  [0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x",
			__func__, (unsigned int)(PQDIP_BASE_ADDR + (PQDIP_OFST * hw_idx)
				+ PQDIP_PIMGI_OFST + i),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_PIMGI_OFST + i + 0x00)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_PIMGI_OFST + i + 0x04)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_PIMGI_OFST + i + 0x08)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_PIMGI_OFST + i + 0x0c)));
		}

		/* PQ_DIP WROT1 registers */
		for (i = 0; i < PQDIP_WROT1_REG_CNT; i += 0x10) {
			dev_info(imgsys_dev->dev, "%s:  [0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x",
			__func__, (unsigned int)(PQDIP_BASE_ADDR + (PQDIP_OFST * hw_idx)
				+ PQDIP_WROT1_OFST + i),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT1_OFST + i + 0x00)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT1_OFST + i + 0x04)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT1_OFST + i + 0x08)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT1_OFST + i + 0x0c)));
		}

		/* PQ_DIP WROT2 registers */
		for (i = 0; i < PQDIP_WROT2_REG_CNT; i += 0x10) {
			dev_info(imgsys_dev->dev, "%s:  [0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x",
			__func__, (unsigned int)(PQDIP_BASE_ADDR + (PQDIP_OFST * hw_idx)
				+ PQDIP_WROT2_OFST + i),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT2_OFST + i + 0x00)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT2_OFST + i + 0x04)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT2_OFST + i + 0x08)),
			(unsigned int)ioread32((void *)(pqdipRegBA + PQDIP_WROT2_OFST + i + 0x0c)));
		}
	iounmap(pqdipRegBA);
	}

	dev_info(imgsys_dev->dev, "%s: -\n", __func__);
}
EXPORT_SYMBOL(imgsys_pqdip_debug_dump);
