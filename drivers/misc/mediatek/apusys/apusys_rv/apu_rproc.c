// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/remoteproc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <linux/memremap.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/time64.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/sched/clock.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>

#include "apu.h"
#include "apu_excep.h"
#include "apu_config.h"

static void *apu_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	void *ptr = NULL;
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;

	if (da >= DRAM_OFFSET && da < DRAM_OFFSET + CODE_BUF_SIZE) {
		ptr = apu->code_buf + (da - DRAM_OFFSET);
		//pr_info("%s(DRAM): da = 0x%llx, len = 0x%x\n",
		//	__func__, da, len);
	} else if (da >= TCM_OFFSET && da < TCM_OFFSET + TCM_SIZE) {
		ptr = apu->md32_tcm + (da - TCM_OFFSET);
		//pr_info("%s(TCM): da = 0x%llx, len = 0x%x\n",
		//	__func__, da, len);
	}
	return ptr;
}

static int __apu_run(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	struct device *dev = apu->dev;
	struct apu_run *run = &apu->run;
	struct timespec64 begin, end, delta;
	int ret;

	if (!hw_ops->start) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->start(apu);

	/* check if boot success */
	ktime_get_ts64(&begin);

	ret = wait_event_interruptible_timeout(
					run->wq,
					run->signaled,
					msecs_to_jiffies(2000));

	ktime_get_ts64(&end);

	if (ret == 0) {
		dev_info(dev, "APU initialization timeout!!\n");
		ret = -ETIME;
		goto stop;
	}
	if (ret == -ERESTARTSYS) {
		dev_info(dev, "wait APU interrupted by a signal!!\n");
		goto stop;
	}

	delta = timespec64_sub(end, begin);
	dev_info(dev, "APU uP boot success. boot time: %llu s, %llu ns\n",
		  (uint64_t) delta.tv_sec, (uint64_t) delta.tv_nsec);

	return 0;

stop:
	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->stop(apu);

	return ret;
}

static int apu_start(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;

	dev_info(apu->dev, "%s: try to boot uP\n", __func__);
	return __apu_run(rproc);
}

static int apu_attach(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;

	dev_info(apu->dev, "%s: try to boot uP\n", __func__);
	return __apu_run(rproc);
}

static int apu_stop(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;

	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->stop(apu);

	return 0;
}

static const struct rproc_ops apu_ops = {
	.start		= apu_start,
	.stop		= apu_stop,
	.attach		= apu_attach,
	.da_to_va	= apu_da_to_va,
};

static void apu_dram_boot_remove(struct mtk_apu *apu)
{
	void *domain = iommu_get_domain_for_dev(apu->dev);
	u32 boundary = (u32) upper_32_bits(apu->code_da);
	u64 iova = CODE_BUF_DA | ((u64) boundary << 32);

	if (apu->platdata->flags & F_SECURE_BOOT)
		return;

	if ((apu->platdata->flags & F_VDRAM_BOOT) == 0)
		if (domain != NULL)
			iommu_unmap(domain, iova, CODE_BUF_SIZE);
	dma_free_coherent(apu->dev, CODE_BUF_SIZE, apu->code_buf, apu->code_da);
}

static int apu_dram_boot_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret = 0;
	int map_sg_sz = 0;
	void *domain;
	struct sg_table sgt;
	phys_addr_t pa;
	u32 boundary;
	u64 iova;

	if (apu->platdata->flags & F_SECURE_BOOT)
		return 0;

	if ((apu->platdata->flags & F_VDRAM_BOOT) == 0) {
		domain = iommu_get_domain_for_dev(apu->dev);
		if (domain == NULL) {
			dev_info(dev, "%s: iommu_get_domain_for_dev fail\n", __func__);
			return -ENOMEM;
		}
	}

	/* Allocate code buffer */
	apu->code_buf = dma_alloc_coherent(apu->dev, CODE_BUF_SIZE,
					&apu->code_da, GFP_KERNEL);
	if (apu->code_buf == NULL || apu->code_da == 0) {
		dev_info(dev, "%s: dma_alloc_coherent fail\n", __func__);
		return -ENOMEM;
	}
	memset(apu->code_buf, 0, CODE_BUF_SIZE);

	boundary = (u32) upper_32_bits(apu->code_da);
	iova = CODE_BUF_DA | ((u64) boundary << 32);
	dev_info(dev, "%s: boundary = %u, iova = 0x%llx\n",
		__func__, boundary, iova);

	if ((apu->platdata->flags & F_VDRAM_BOOT) == 0) {
		sgt.sgl = NULL;
		/* Convert IOVA to sgtable */
		ret = dma_get_sgtable(apu->dev, &sgt, apu->code_buf,
			apu->code_da, CODE_BUF_SIZE);
		if (ret < 0 || sgt.sgl == NULL) {
			dev_info(dev, "get sgtable fail\n");
			return -EINVAL;
		}

		dev_info(dev, "%s: sgt.nents = %d, sgt.orig_nents = %d\n",
			__func__, sgt.nents, sgt.orig_nents);
		/* Map sg_list to MD32_BOOT_ADDR */
		map_sg_sz = iommu_map_sg(domain, iova, sgt.sgl,
			sgt.nents, IOMMU_READ|IOMMU_WRITE);
		dev_info(dev, "%s: sgt.nents = %d, sgt.orig_nents = %d\n",
			__func__, sgt.nents, sgt.orig_nents);
		dev_info(dev, "%s: map_sg_sz = %d\n", __func__, map_sg_sz);
		if (map_sg_sz != CODE_BUF_SIZE)
			dev_info(dev, "%s: iommu_map_sg fail(%d)\n", __func__, ret);

		pa = iommu_iova_to_phys(domain,
			iova + CODE_BUF_SIZE - SZ_4K);
		dev_info(dev, "%s: pa = 0x%llx\n",
			__func__, (uint64_t) pa);
		if (!pa) // pa should not be null
			dev_info(dev, "%s: check pa fail(0x%llx)\n",
				__func__, (uint64_t) pa);
	}

	dev_info(dev, "%s: apu->code_buf = 0x%llx, apu->code_da = 0x%llx\n",
		__func__, (uint64_t) apu->code_buf, (uint64_t) apu->code_da);

	return ret;
}

static int apu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rproc *rproc;
	struct mtk_apu *apu;
	struct mtk_apu_platdata *data;
	struct mtk_apu_hw_ops *hw_ops;
	char *fw_name = "mrv.elf";
	int ret = 0;

	dev_info(dev, "%s: enter\n", __func__);
	data = (struct mtk_apu_platdata *)of_device_get_match_data(dev);
	if (!data) {
		dev_info(dev, "%s: of_device_get_match_data fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: platdata flags=0x%08x\n", __func__, data->flags);
	hw_ops = &data->ops;

	ret = device_rename(dev, "apusys_rv");
	if (ret) {
		dev_info(dev, "unable to modify device name\n");
		return -ENOMEM;
	}

	rproc = rproc_alloc(dev,
						np->name,
						&apu_ops,
						fw_name,
						sizeof(struct mtk_apu));

	if (!rproc) {
		dev_info(dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	/*
	 * auto boot off
	 * make sure the firmware is in /vendor/firmware/mrv.elf
	 * before boot up.
	 *
	 * use below command to run uP:
	 * echo start > /sys/class/remoteproc/remoteproc0/state
	 */
	if (data->flags & F_IS_BRINGUP)
		rproc->auto_boot = true;
	else
		rproc->auto_boot = false;

	apu = (struct mtk_apu *)rproc->priv;
	dev_info(dev, "%s: apu=%p\n", __func__, apu);
	apu->rproc = rproc;
	apu->pdev = pdev;
	apu->dev = dev;
	apu->platdata = data;
	platform_set_drvdata(pdev, apu);
	spin_lock_init(&apu->reg_lock);

	dev_info(dev, "%s %d\n", __func__, __LINE__);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
	dev_info(dev, "%s %d\n", __func__, __LINE__);

	if (!hw_ops->apu_memmap_init) {
		WARN_ON(1);
		return -EINVAL;
	}
	ret = hw_ops->apu_memmap_init(apu);
	if (ret)
		goto remove_apu_memmap;

	ret = apu_mem_init(apu);
	if (ret)
		goto remove_apu_mem;

	ret = apu_config_setup(apu);
	if (ret)
		goto remove_apu_config_setup;

	ret = apu_coredump_init(apu);
	if (ret)
		goto remove_apu_coredump;

	ret = apu_dram_boot_init(apu);
	if (ret)
		goto remove_apu_dram_boot;

	ret = apu_ipi_init(pdev, apu);
	if (ret)
		goto remove_apu_ipi;

	ret = apu_sysfs_init(pdev);
	if (ret)
		goto remove_apu_sysfs;

	ret = apu_excep_init(pdev, apu);
	if (ret < 0)
		goto remove_apu_excep;

	if (data->flags & F_PRELOAD_FIRMWARE)
		rproc->state = RPROC_DETACHED;

	ret = rproc_add(rproc);
	if (ret < 0) {
		dev_info(dev, "boot fail ret:%d\n", ret);
		rproc_del(rproc);
		goto remove_apu_excep;
	}

	if (hw_ops->init) {
		ret = hw_ops->init(apu);
		if (ret)
			goto del_rproc;
	}

	return 0;

del_rproc:
	rproc_del(rproc);

remove_apu_excep:
	apu_excep_remove(pdev, apu);

remove_apu_sysfs:
	apu_sysfs_remove(pdev);

remove_apu_ipi:
	apu_ipi_remove(apu);

remove_apu_dram_boot:
	apu_dram_boot_remove(apu);

remove_apu_coredump:
	apu_coredump_remove(apu);

remove_apu_config_setup:
	apu_config_remove(apu);

remove_apu_mem:
	apu_mem_remove(apu);

remove_apu_memmap:
	if (!hw_ops->apu_memmap_remove) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->apu_memmap_remove(apu);

	return ret;
}

static int apu_remove(struct platform_device *pdev)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);
	struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;

	if (hw_ops->exit)
		hw_ops->exit(apu);

	rproc_del(apu->rproc);

	apu_excep_remove(pdev, apu);
	apu_sysfs_remove(pdev);
	apu_ipi_remove(apu);
	apu_dram_boot_remove(apu);
	apu_coredump_remove(apu);
	apu_config_remove(apu);
	apu_mem_remove(apu);
	if (!hw_ops->apu_memmap_remove) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->apu_memmap_remove(apu);

	rproc_free(apu->rproc);

	return 0;
}

#ifndef MT6879_APUSYS_RV_PLAT_DATA
const struct mtk_apu_platdata mt6879_platdata;
#endif
#ifndef MT6893_APUSYS_RV_PLAT_DATA
const struct mtk_apu_platdata mt6893_platdata;
#endif
#ifndef MT6983_APUSYS_RV_PLAT_DATA
const struct mtk_apu_platdata mt6983_platdata;
#endif

static const struct of_device_id mtk_apu_of_match[] = {
	{ .compatible = "mediatek,mt6879-apusys_rv", .data = &mt6879_platdata},
	{ .compatible = "mediatek,mt6893-apusys_rv", .data = &mt6893_platdata},
	{ .compatible = "mediatek,mt6983-apusys_rv", .data = &mt6983_platdata},
	{},
};

static struct platform_driver mtk_apu_driver = {
	.probe = apu_probe,
	.remove = apu_remove,
	.driver = {
		.name = "mtk-apu",
		.of_match_table = of_match_ptr(mtk_apu_of_match),
	},
};

int apu_rproc_init(void)
{
	int ret;

	ret = platform_driver_register(&mtk_apu_driver);
	if (ret)
		pr_info("failed to register mtk_apu_driver\n");

	return ret;
}

void apu_rproc_exit(void)
{
	platform_driver_unregister(&mtk_apu_driver);
}
