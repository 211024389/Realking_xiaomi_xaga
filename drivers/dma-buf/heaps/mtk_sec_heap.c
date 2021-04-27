// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF mtk_sec heap exporter
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019, 2020 Linaro Ltd.
 * Copyright (C) 2021 MediaTek Inc.
 *
 * Portions based off of Andrew Davis' SRAM heap:
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 */

#define pr_fmt(fmt) "[MTK_DMABUF_HEAP: SEC] "fmt

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "page_pool.h"
#include "deferred-free-helper.h"

#include <public/trusted_mem_api.h>

static struct dma_heap *mtk_svp_heap;
static struct dma_heap *mtk_prot_heap;

//TODO: should replace by atomic_t
static size_t sec_heap_total_memory;

struct mtk_sec_heap_buffer {
	struct dma_heap *heap;
	struct list_head attachments;
	struct mutex lock;
	unsigned long len;
	struct sg_table sg_table;
	int vmap_cnt;
	void *vaddr;
	struct deferred_freelist_item deferred_free;

	bool uncached;

	void *priv;
};

struct sec_heap_dev_info {
	struct device           *dev;
	enum dma_data_direction direction;
	unsigned long           map_attrs;
};

/* No domain concept in secure memory, set array count as 1 */
struct sec_heap_priv {
	bool                     mapped[1];
	struct sec_heap_dev_info dev_info[1];
	/* secure heap will not strore sgtable here */
	struct sg_table          *mapped_table[1];
	struct mutex             lock; /* map iova lock */
	pid_t                    pid;
	pid_t                    tid;
	char                     pid_name[TASK_COMM_LEN];
	char                     tid_name[TASK_COMM_LEN];
};

struct dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool mapped;

	bool uncached;
};

static struct sg_table *dup_sg_table_sec(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sgtable_sg(table, sg, i) {
		/* also copy dma_address */
		memcpy(new_sg, sg, sizeof(*sg));
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static void tmem_free(enum TRUSTED_MEM_REQ_TYPE tmem_type,
		      struct dma_buf *dmabuf)
{
	struct mtk_sec_heap_buffer *buffer = dmabuf->priv;
	struct sec_heap_priv *buf_info = buffer->priv;
	u32 sec_handle = 0;

	pr_debug("[%s][%d] %s: enter priv 0x%lx\n",
		 dmabuf->exp_name, tmem_type,
		 __func__, dmabuf->priv);

	sec_heap_total_memory -= buffer->len;

	sec_handle = sg_dma_address(buffer->sg_table.sgl);

	trusted_mem_api_unref(tmem_type, sec_handle,
			      (uint8_t *)dma_heap_get_name(buffer->heap), 0);

	mutex_lock(&dmabuf->lock);
	kfree(buf_info);
	kfree(buffer);

	pr_debug("%s: [%s][%d] exit, total %zu\n", __func__,
		 dmabuf->exp_name, tmem_type, sec_heap_total_memory);

	mutex_unlock(&dmabuf->lock);
}

static inline void svp_free(struct dma_buf *dmabuf)
{
	enum TRUSTED_MEM_REQ_TYPE tmem_type = TRUSTED_MEM_REQ_SVP_REGION;

	tmem_free(tmem_type, dmabuf);
}

static int mtk_sec_heap_attach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct mtk_sec_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct sg_table *table;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table_sec(&buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;
	a->uncached = buffer->uncached;
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void mtk_sec_heap_detach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct mtk_sec_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *mtk_sec_heap_map_dma_buf(struct dma_buf_attachment *attachment,
						 enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *table = a->table;

	return table;
}

static void mtk_sec_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
				       struct sg_table *table,
				       enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *sgt = a->table;

	/* set dma_address as 0 to clear secure handle*/
	sg_dma_address(sgt->sgl) = 0;
}

static const struct dma_buf_ops svp_heap_buf_ops = {
	.attach = mtk_sec_heap_attach,
	.detach = mtk_sec_heap_detach,
	.map_dma_buf = mtk_sec_heap_map_dma_buf,
	.unmap_dma_buf = mtk_sec_heap_unmap_dma_buf,
	.release = svp_free,
};

static inline void prot_free(struct dma_buf *dmabuf)
{
	enum TRUSTED_MEM_REQ_TYPE tmem_type = TRUSTED_MEM_REQ_PROT;

	tmem_free(tmem_type, dmabuf);
}

static const struct dma_buf_ops prot_heap_buf_ops = {
	.attach = mtk_sec_heap_attach,
	.detach = mtk_sec_heap_detach,
	.map_dma_buf = mtk_sec_heap_map_dma_buf,
	.unmap_dma_buf = mtk_sec_heap_unmap_dma_buf,
	.release = prot_free,
};

static struct dma_buf *tmem_allocate(enum TRUSTED_MEM_REQ_TYPE tmem_type,
				     const struct dma_buf_ops *heap_buf_ops,
				     struct dma_heap *heap,
				     unsigned long len,
				     unsigned long fd_flags,
				     unsigned long heap_flags)
{
	struct mtk_sec_heap_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	struct sg_table *table;
	int ret = -ENOMEM;
	struct sec_heap_priv *info;
	struct task_struct *task = current->group_leader;
	u32 sec_handle = 0;
	u32 refcount = 0;/* tmem refcount */

	pr_debug("[%s][%d] %s: enter: size 0x%lx\n",
		 dma_heap_get_name(heap), tmem_type, __func__, len);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!buffer || !info || !table) {
		pr_info("%s#%d Error. Allocate mem failed.\n",
			__func__, __LINE__);
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->heap = heap;
	buffer->len = len;
	buffer->uncached = true;/* all secure memory set as uncached buffer */

	ret = trusted_mem_api_alloc(tmem_type, 0, len, &refcount,
				    &sec_handle,
				    (uint8_t *)dma_heap_get_name(heap),
				    0);

	if (ret == -ENOMEM) {
		pr_info("%s security out of memory, heap:%s\n",
			__func__, dma_heap_get_name(heap));
	}

	if (sec_handle <= 0) {
		pr_info("%s alloc security memory failed, total size %zu\n",
			__func__, sec_heap_total_memory);
		//TODO: should dump used memory here
		ret = -ENOMEM;
		goto free_buffer_struct;
	}

	table = &buffer->sg_table;

	/* secure memory doesn't have page struct
	 * alloc one node to record secure handle
	 */
	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret) {
		//free buffer
		pr_info("%s#%d Error. Allocate mem failed.\n",
			__func__, __LINE__);
		goto free_buffer;
	}
	sg_set_page(table->sgl, 0, 0, 0);

	/* store seucre handle */
	sg_dma_address(table->sgl) = (dma_addr_t)sec_handle;

	/* create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		pr_info("%s dma_buf_export fail\n", __func__);
		goto free_buffer;
	}

	/* add debug info */
	buffer->priv = info;
	mutex_init(&info->lock);
	/* add alloc pid & tid info*/
	get_task_comm(info->pid_name, task);
	get_task_comm(info->tid_name, current);
	info->pid = task_pid_nr(task);
	info->tid = task_pid_nr(current);

	sec_heap_total_memory += len;
	pr_debug("[%s][%d] %s: priv 0x%lx, size 0x%lx\n",
		 dma_heap_get_name(heap), tmem_type, __func__,
		 sg_dma_address(buffer->sg_table.sgl),
		 buffer->len);
	return dmabuf;
free_buffer:
	//free secure handle
	trusted_mem_api_unref(tmem_type, sec_handle,
			      (uint8_t *)dma_heap_get_name(buffer->heap), 0);
free_buffer_struct:
	sg_free_table(table);
	kfree(info);
	kfree(buffer);
	return ERR_PTR(ret);
}

static inline struct dma_buf *svp_allocate(struct dma_heap *heap,
					   unsigned long len,
					   unsigned long fd_flags,
					   unsigned long heap_flags)
{
	enum TRUSTED_MEM_REQ_TYPE tmem_type = TRUSTED_MEM_REQ_SVP_REGION;

	return tmem_allocate(tmem_type, &svp_heap_buf_ops,
			     heap, len, fd_flags, heap_flags);
}

static const struct dma_heap_ops svp_heap_ops = {
	.allocate = svp_allocate,
};

static inline struct dma_buf *prot_allocate(struct dma_heap *heap,
					    unsigned long len,
					    unsigned long fd_flags,
					    unsigned long heap_flags)
{
	enum TRUSTED_MEM_REQ_TYPE tmem_type = TRUSTED_MEM_REQ_PROT;

	return tmem_allocate(tmem_type, &prot_heap_buf_ops,
			     heap, len, fd_flags, heap_flags);
}

static const struct dma_heap_ops prot_heap_ops = {
	.allocate = prot_allocate,
};

static int mtk_sec_heap_create(void)
{
	struct dma_heap_export_info exp_info;

	/* No need pagepool for secure heap */

	exp_info.name = "mtk_svp_region-uncached";
	exp_info.ops = &svp_heap_ops;
	exp_info.priv = NULL;

	mtk_svp_heap = dma_heap_add(&exp_info);
	if (IS_ERR(mtk_svp_heap))
		return PTR_ERR(mtk_svp_heap);
	pr_info("%s add heap[%s] success\n", __func__, exp_info.name);

	exp_info.name = "mtk_prot_region-uncached";
	exp_info.ops = &prot_heap_ops;
	exp_info.priv = NULL;

	mtk_prot_heap = dma_heap_add(&exp_info);
	if (IS_ERR(mtk_prot_heap))
		return PTR_ERR(mtk_prot_heap);
	pr_info("%s add heap[%s] done\n", __func__, exp_info.name);

	return 0;
}
module_init(mtk_sec_heap_create);
MODULE_LICENSE("GPL v2");
