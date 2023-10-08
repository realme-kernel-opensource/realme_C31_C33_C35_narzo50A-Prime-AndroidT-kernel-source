
/*****************************************************************************
 *
 * Copyright (c) Imagination Technologies Ltd.
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 ("GPL")in which case the provisions of
 * GPL are applicable instead of those above.
 *
 * If you wish to allow use of your version of this file only under the terms
 * of GPL, and not to allow others to use your version of this file under the
 * terms of the MIT license, indicate your decision by deleting the provisions
 * above and replace them with the notice and other provisions required by GPL
 * as set out in the file called "GPLHEADER" included in this distribution. If
 * you do not delete the provisions above, a recipient may use your version of
 * this file under the terms of either the MIT license or GPL.
 *
 * This License is also included in this distribution in the file called
 * "MIT_COPYING".
 *
 *****************************************************************************/

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/gfp.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>

#include "sprd_vdsp_mem_core.h"
#include "sprd_vdsp_mem_core_priv.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "sprd-vdsp: [mem_heap]: %d %s: "\
        fmt, current->pid, __func__

static int trace_physical_pages = 0;

struct buffer_data {
	struct sg_table *sgt;
	enum sprd_vdsp_mem_attr mattr;	/* memory attributes */
};

static int anonymous_heap_import(struct device *device, struct heap *heap,
				 size_t size, enum sprd_vdsp_mem_attr attr,
				 uint64_t buf_hnd, struct buffer *buffer)
{
	struct buffer_data *data;
	unsigned long cpu_addr = (unsigned long)buf_hnd;
	struct sg_table *sgt;
	struct page **pages;
	struct scatterlist *sgl;
	int num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	int ret;
	int i;

	/* Check alignment */
	if (cpu_addr & (PAGE_SIZE - 1)) {
		pr_err("wrong alignment of %#lx address!\n", cpu_addr);
		return -EFAULT;
	}

	pages = kmalloc_array(num_pages, sizeof(struct page *),
			      GFP_KERNEL | __GFP_ZERO);
	if (!pages) {
		pr_err("failed to allocate memory for pages\n");
		return -ENOMEM;
	}

	down_read(&current->mm->mmap_sem);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	ret = get_user_pages(cpu_addr, num_pages, FOLL_WRITE, pages, NULL);
#else
	pr_err("get_user_pages not supported for this kernel version\n");
	ret = -1;
#endif
	up_read(&current->mm->mmap_sem);
	if (ret != num_pages) {
		pr_err
		    ("Error: failed to get_user_pages count:%d for %#lx address\n",
		     num_pages, cpu_addr);
		pr_err("Error: get_user_pages ret: %d\n", ret);
		ret = -EFAULT;
		goto get_user_pages_failed;
	}

	sgt = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto alloc_sgt_failed;
	}

	ret = sg_alloc_table(sgt, num_pages, GFP_KERNEL);
	if (ret) {
		pr_err("failed to allocate sgt with num_pages\n");
		goto alloc_sgt_pages_failed;
	}

	data = kzalloc(sizeof(struct buffer_data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto alloc_priv_failed;
	}

	for_each_sg(sgt->sgl, sgl, sgt->nents, i) {
		struct page *page = pages[i];

		sg_set_page(sgl, page, PAGE_SIZE, 0);

		/* Sanity check if physical address is
		 * accessible from the device PoV */
		if (~dma_get_mask(device) & sg_phys(sgl)) {
			pr_err("%0xllX physical address is out of dma_mask,"
			       " and probably won't be accessible by the core!\n",
			       sg_phys(sgl));
			ret = -EFAULT;
			goto dma_mask_check_failed;
		}

		if (trace_physical_pages) {
			pr_debug("phys %#llx length %d\n",
				 (unsigned long long)sg_phys(sgl), sgl->length);
		}
	}

	data->sgt = sgt;
	data->mattr = attr;
	buffer->priv = data;

	ret = dma_map_sg(buffer->device, sgt->sgl, sgt->orig_nents,
			 DMA_BIDIRECTIONAL);
	if (ret <= 0) {
		pr_err("%s dma_map_sg failed!\n", __func__);
		goto dma_mask_check_failed;
	}

	kfree(pages);
	return 0;

dma_mask_check_failed:
	kfree(data);
alloc_priv_failed:
	sg_free_table(sgt);
alloc_sgt_pages_failed:
	kfree(sgt);
get_user_pages_failed:
	for (i = 0; i < num_pages; i++)
		if (pages[i])
			put_page(pages[i]);
alloc_sgt_failed:
	kfree(pages);
	return ret;
}

static void anonymous_heap_free(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *data = buffer->priv;
	struct sg_table *sgt = data->sgt;
	struct scatterlist *sgl;
	bool dirty = false;

	dma_unmap_sg(buffer->device, sgt->sgl, sgt->orig_nents,
		     DMA_BIDIRECTIONAL);
	if (buffer->kptr) {
		dirty = true;
		vunmap(buffer->kptr);
		buffer->kptr = NULL;
	}

	sgl = sgt->sgl;
	while (sgl) {
		struct page *page = sg_page(sgl);

		if (page) {
			if (dirty)
				set_page_dirty(page);
			put_page(page);
		}
		sgl = sg_next(sgl);
	}

	sg_free_table(sgt);
	kfree(sgt);
	kfree(data);
}

static int anonymous_heap_map_km(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *buffer_data = buffer->priv;
	struct sg_table *sgt = buffer_data->sgt;
	struct scatterlist *sgl = sgt->sgl;
	unsigned int num_pages = sg_nents(sgl);
	struct page **pages;
	pgprot_t prot;
	int i;

	if (buffer->kptr) {
		pr_warn("called for already mapped buffer %d\n", buffer->id);
		return 0;
	}

	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		pr_err("failed to allocate memory for pages\n");
		return -ENOMEM;
	}

	prot = PAGE_KERNEL;
	prot = pgprot_noncached(prot);

	i = 0;
	while (sgl) {
		pages[i++] = sg_page(sgl);
		sgl = sg_next(sgl);
	}

	buffer->kptr = vmap(pages, num_pages, VM_MAP, prot);
	kfree(pages);
	if (!buffer->kptr) {
		pr_err("vmap failed!\n");
		return -EFAULT;
	}

	return 0;
}

static int anonymous_heap_unmap_km(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *data = buffer->priv;
	struct sg_table *sgt = data->sgt;
	struct scatterlist *sgl;

	if (!buffer->kptr) {
		pr_warn("called for unmapped buffer %d\n", buffer->id);
		return 0;
	}

	vunmap(buffer->kptr);
	buffer->kptr = NULL;

	sgl = sgt->sgl;
	while (sgl) {
		struct page *page = sg_page(sgl);

		if (page) {
			set_page_dirty(page);
		}
		sgl = sg_next(sgl);
	}

	return 0;
}

static int anonymous_get_sg_table(struct heap *heap, struct buffer *buffer,
				  struct sg_table **sg_table)
{
	struct buffer_data *data = buffer->priv;

	*sg_table = data->sgt;
	return 0;
}

static void anonymous_sync_cpu_to_dev(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *buffer_data = buffer->priv;
	struct sg_table *sgt = buffer_data->sgt;

	pr_debug("%s:%d buffer %d (0x%p)\n", __func__, __LINE__,
		 buffer->id, buffer);
	if (!(buffer_data->mattr & SPRD_VDSP_MEM_ATTR_UNCACHED)) {
		dma_sync_sg_for_device(buffer->device,
				       sgt->sgl, sgt->orig_nents,
				       DMA_TO_DEVICE);
		dma_sync_sg_for_cpu(buffer->device, sgt->sgl, sgt->orig_nents,
				    DMA_FROM_DEVICE);
	}
}

static void anonymous_sync_dev_to_cpu(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *buffer_data = buffer->priv;
	struct sg_table *sgt = buffer_data->sgt;

	pr_debug("%s:%d buffer %d (0x%p)\n", __func__, __LINE__,
		 buffer->id, buffer);

	if (!(buffer_data->mattr & SPRD_VDSP_MEM_ATTR_UNCACHED)) {
		dma_sync_sg_for_cpu(buffer->device,
				    sgt->sgl, sgt->orig_nents, DMA_FROM_DEVICE);
	}
}

static void anonymous_heap_destroy(struct heap *heap)
{
	return;
}

static struct heap_ops anonymous_heap_ops = {
	.alloc = NULL,
	.import = anonymous_heap_import,
	.free = anonymous_heap_free,
	.map_um = NULL,
	.unmap_um = NULL,
	.map_km = anonymous_heap_map_km,
	.unmap_km = anonymous_heap_unmap_km,
	.get_sg_table = anonymous_get_sg_table,
	.get_page_array = NULL,
	.sync_cpu_to_dev = anonymous_sync_cpu_to_dev,
	.sync_dev_to_cpu = anonymous_sync_dev_to_cpu,
	.destroy = anonymous_heap_destroy,
};

int sprd_vdsp_mem_anonymous_init(const struct heap_config *heap_cfg,
				 struct heap *heap)
{

	heap->ops = &anonymous_heap_ops;

	return 0;
}
