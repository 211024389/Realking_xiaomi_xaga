// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_device.h>

#include "mdw_cmn.h"
#include "mdw_mem.h"

struct mdw_mem_dma_attachment {
	struct sg_table sgt;
	struct device *dev;
	struct list_head node;
};

struct mdw_mem_dma {
	struct dma_buf *dbuf;
	dma_addr_t dma_addr;
	unsigned int dma_size;
	union {
		struct {
			int handle;
			void *vaddr;
			struct list_head attachments;
		} a;
		struct {
			struct dma_buf_attachment *attach;
			struct sg_table *sgt;
		} m;
	};

	struct mutex mtx;
	struct mdw_device *mdev;
};

static int mdw_dmabuf_attach(struct dma_buf *dbuf,
	struct dma_buf_attachment *attach)
{
	struct mdw_mem_dma_attachment *a = NULL;
	struct mdw_mem_dma *mdbuf = dbuf->priv;
	int ret = 0;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = dma_get_sgtable(&mdbuf->mdev->pdev->dev, &a->sgt,
		mdbuf->a.vaddr, mdbuf->dma_addr, mdbuf->dma_size);
	if (ret < 0) {
		mdw_drv_err("failed to get sgtable\n");
		kfree(a);
		return ret;
	}

	mdw_mem_debug("%s: %d\n", __func__, __LINE__);
	a->dev = attach->dev;
	attach->priv = a;
	mutex_lock(&mdbuf->mtx);
	list_add(&a->node, &mdbuf->a.attachments);
	mutex_unlock(&mdbuf->mtx);

	return 0;
}

static void mdw_dmabuf_detach(struct dma_buf *dbuf,
	struct dma_buf_attachment *attach)
{
	struct mdw_mem_dma_attachment *a = attach->priv;
	struct mdw_mem_dma *mdbuf = dbuf->priv;

	mdw_mem_debug("%s: %d\n", __func__, __LINE__);
	mutex_lock(&mdbuf->mtx);
	list_del(&a->node);
	mutex_unlock(&mdbuf->mtx);
	sg_free_table(&a->sgt);
	kfree(a);
}

static struct sg_table *mdw_dmabuf_map_dma(struct dma_buf_attachment *attach,
	enum dma_data_direction dir)
{
	struct mdw_mem_dma_attachment *a = attach->priv;
	struct sg_table *t = NULL;
	int ret = 0;

	mdw_mem_debug("%s: %d\n", __func__, __LINE__);

	t = &a->sgt;
	ret = dma_map_sgtable(attach->dev, &a->sgt, dir, 0);
	if (ret)
		t = ERR_PTR(ret);

	return t;
}

static void mdw_dmabuf_unmap_dma(struct dma_buf_attachment *attach,
				    struct sg_table *sgt,
				    enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
}

static void *mdw_dmabuf_vmap(struct dma_buf *dbuf)
{
	struct mdw_mem_dma *mdbuf = dbuf->priv;

	mdw_mem_debug("dmabuf vmap:%p\n", mdbuf->a.vaddr);
	return mdbuf->a.vaddr;
}

static void mdw_dmabuf_release(struct dma_buf *dbuf)
{
	struct mdw_mem_dma *mdbuf = dbuf->priv;

	mdw_mem_debug("%s: %d %p\n", __func__, __LINE__, dbuf->priv);
	dma_free_coherent(&mdbuf->mdev->pdev->dev, mdbuf->dma_size,
		mdbuf->a.vaddr, mdbuf->dma_addr);

	kfree(mdbuf);
}

static int mdw_dmabuf_mmap(struct dma_buf *dbuf,
				  struct vm_area_struct *vma)
{
	struct mdw_mem_dma *mdbuf = dbuf->priv;
	int ret = 0;

	mdw_mem_debug("%s: %d\n", __func__, __LINE__);
	ret = dma_mmap_coherent(&mdbuf->mdev->pdev->dev, vma, mdbuf->a.vaddr,
				mdbuf->dma_addr, mdbuf->dma_size);
	if (ret)
		mdw_drv_err("mmap dma-buf error(%d)\n", ret);

	return ret;
}

static struct dma_buf_ops mdw_dmabuf_ops = {
	.attach = mdw_dmabuf_attach,
	.detach = mdw_dmabuf_detach,
	.map_dma_buf = mdw_dmabuf_map_dma,
	.unmap_dma_buf = mdw_dmabuf_unmap_dma,
	.vmap = mdw_dmabuf_vmap,
	.mmap = mdw_dmabuf_mmap,
	.release = mdw_dmabuf_release,
};

int mdw_mem_dma_alloc(struct mdw_fpriv *mpriv, struct mdw_mem *mem)
{
	struct mdw_device *mdev = mpriv->mdev;
	struct mdw_mem_dma *mdbuf = NULL;
	int ret = 0;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	/* alloc mdw dma-buf container */
	mdbuf = kzalloc(sizeof(*mdbuf), GFP_KERNEL);
	if (!mdbuf)
		return -ENOMEM;

	mutex_init(&mdbuf->mtx);
	INIT_LIST_HEAD(&mdbuf->a.attachments);
	mdbuf->mdev = mdev;

	/* alloc buffer by dma */
	mdbuf->dma_size = PAGE_ALIGN(mem->size);
	mdw_mem_debug("alloc mem(%p)(%u/%u)\n",
		mem, mem->size, mdbuf->dma_size);

	/* TODO, handle cache */
	mdbuf->a.vaddr = dma_alloc_coherent(&mdev->pdev->dev, mdbuf->dma_size,
		&mdbuf->dma_addr, GFP_KERNEL);

	if (!mdbuf->a.vaddr)
		goto free_mdw_dbuf;

	/* export as dma-buf */
	exp_info.ops = &mdw_dmabuf_ops;
	exp_info.size = mdbuf->dma_size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = mdbuf;

	mdbuf->dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(mdbuf->dbuf))
		goto free_dma_buf;

	mdbuf->dbuf->priv = mdbuf;

	/* create fd from dma-buf */
	mdbuf->a.handle =  dma_buf_fd(mdbuf->dbuf,
		(O_RDWR | O_CLOEXEC) & ~O_ACCMODE);
	if (mdbuf->a.handle < 0) {
		dma_buf_put(mdbuf->dbuf);
		return -EINVAL;
	}

	/* access data to mdw_mem */
	mem->device_va = mdbuf->dma_addr;
	mem->handle = mdbuf->a.handle;
	mem->vaddr = mdbuf->a.vaddr;

	mdw_mem_debug("alloc mem(%p)(%p/%u)(0x%llx/%u) done\n",
		mem, mem->vaddr, mem->size, mdbuf->dma_addr, mdbuf->dma_size);

	return 0;

free_dma_buf:
	dma_free_coherent(&mdbuf->mdev->pdev->dev, mdbuf->dma_size,
		mdbuf->a.vaddr, mdbuf->dma_addr);
free_mdw_dbuf:
	kfree(mdbuf);

	return ret;
}

int mdw_mem_dma_free(struct mdw_fpriv *mpriv, struct mdw_mem *mem)
{
	struct dma_buf *dbuf = NULL;

	dbuf = dma_buf_get(mem->handle);
	if (IS_ERR_OR_NULL(dbuf)) {
		mdw_drv_err("mem invalid handle(%d)\n", mem, mem->handle);
		return -EINVAL;
	}

	mdw_mem_debug("free mem(%p)\n", mem);
	dma_buf_put(dbuf);
	dma_buf_put(dbuf);

	return 0;
}

int mdw_mem_dma_map(struct mdw_fpriv *mpriv, struct mdw_mem *mem)
{
	int ret = 0;
	struct mdw_mem_dma *mdbuf = NULL;
	struct dma_buf *dbuf = NULL;
	struct mdw_device *mdev = mpriv->mdev;

	if (mem->device_va || mem->priv) {
		mdw_drv_err("mem(%p) already has dva(0x%llx/%p)\n",
			mem, mem->device_va, mem->priv);
		return -EINVAL;
	}

	dbuf = dma_buf_get(mem->handle);
	if (IS_ERR_OR_NULL(dbuf))
		return -ENOMEM;

	mem->priv = kzalloc(sizeof(*mdbuf), GFP_KERNEL);
	if (!mem->priv) {
		ret = -ENOMEM;
		goto put_dbuf;
	}

	mdbuf = mem->priv;
	mdbuf->dbuf = dbuf;
	mdbuf->m.attach = dma_buf_attach(mdbuf->dbuf, &mdev->pdev->dev);
	if (IS_ERR(mdbuf->m.attach)) {
		ret = PTR_ERR(mdbuf->m.attach);
		mdw_drv_err("dma_buf_attach failed: %d\n", ret);
		goto free_mdbuf;
	}

	mdbuf->m.sgt = dma_buf_map_attachment(mdbuf->m.attach,
		DMA_BIDIRECTIONAL);
	if (IS_ERR(mdbuf->m.sgt)) {
		ret = PTR_ERR(mdbuf->m.sgt);
		mdw_drv_err("dma_buf_map_attachment failed: %d\n", ret);
		goto detach_dbuf;
	}

	mdbuf->dma_addr = sg_dma_address(mdbuf->m.sgt->sgl);
	mdbuf->dma_size = sg_dma_len(mdbuf->m.sgt->sgl);
	if (!mdbuf->dma_addr || !mdbuf->dma_size) {
		mdw_drv_err("can't get mem(%p) dva(0x%llx/%u)\n",
			mem, mdbuf->dma_addr, mdbuf->dma_size);
		goto unmap_dbuf;
	}
	mem->device_va = mdbuf->dma_addr;
	mem->priv = mdbuf;

	mdw_mem_debug("map mem(%p/0x%llx)\n", mem, mdbuf->dma_addr);

	goto out;

unmap_dbuf:
	dma_buf_unmap_attachment(mdbuf->m.attach,
		mdbuf->m.sgt, DMA_BIDIRECTIONAL);
detach_dbuf:
	dma_buf_detach(mdbuf->dbuf, mdbuf->m.attach);
free_mdbuf:
	kfree(mdbuf);
put_dbuf:
	dma_buf_put(dbuf);
out:
	return ret;
}

int mdw_mem_dma_unmap(struct mdw_fpriv *mpriv, struct mdw_mem *mem)
{
	struct mdw_mem_dma *mdbuf = NULL;

	if (IS_ERR_OR_NULL(mem->priv))
		return -EINVAL;

	mdw_mem_debug("map mem(%p)\n", mem);
	mdbuf = (struct mdw_mem_dma *)mem->priv;

	if (IS_ERR_OR_NULL(mdbuf->m.attach) ||
		IS_ERR_OR_NULL(mdbuf->m.sgt))
		return -EINVAL;

	mem->device_va = 0;
	mem->priv = NULL;
	dma_buf_unmap_attachment(mdbuf->m.attach,
		mdbuf->m.sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(mdbuf->dbuf, mdbuf->m.attach);
	dma_buf_put(mdbuf->dbuf);
	kfree(mdbuf);

	return 0;
}
