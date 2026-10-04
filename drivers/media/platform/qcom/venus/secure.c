// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/genalloc.h>
#include <linux/iommu.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <dt-bindings/firmware/qcom,scm.h>

#include "core.h"
#include "secure.h"

struct venus_secure_buffer {
	struct list_head list;
	void *pages;
	size_t size;
	dma_addr_t iova;
	u64 owners;
	bool mapped;
	bool orphan;
};

struct venus_secure {
	struct platform_device *pdev;
	struct iommu_domain *domain;
	struct gen_pool *pool;
	struct mutex lock;
	struct list_head buffers;
};

int venus_secure_init(struct venus_core *core)
{
	struct device_node *np;
	struct venus_secure *secure;
	int ret;

	if (!IS_V5(core))
		return 0;

	np = of_get_child_by_name(core->dev->of_node, "secure-nonpixel");
	if (!np)
		return -ENODEV;

	secure = kzalloc_obj(*secure);
	if (!secure) {
		of_node_put(np);
		return -ENOMEM;
	}

	/* Keep the context alive independently of codec child removal. */
	secure->pdev = of_platform_device_create(np, NULL, NULL);
	if (!secure->pdev) {
		ret = -ENODEV;
		goto put_node;
	}

	ret = of_dma_configure(&secure->pdev->dev, np, true);
	if (ret)
		goto put_device;

	secure->domain = iommu_paging_domain_alloc(&secure->pdev->dev);
	if (IS_ERR(secure->domain)) {
		ret = PTR_ERR(secure->domain);
		goto put_device;
	}

	ret = iommu_attach_device(secure->domain, &secure->pdev->dev);
	if (ret)
		goto free_domain;

	secure->pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!secure->pool) {
		ret = -ENOMEM;
		goto detach;
	}

	ret = gen_pool_add(secure->pool, core->res->cp_nonpixel_start,
			   core->res->cp_nonpixel_size, -1);
	if (ret)
		goto destroy_pool;

	mutex_init(&secure->lock);
	INIT_LIST_HEAD(&secure->buffers);
	core->secure = secure;
	of_node_put(np);
	return 0;

destroy_pool:
	gen_pool_destroy(secure->pool);
detach:
	iommu_detach_device(secure->domain, &secure->pdev->dev);
free_domain:
	iommu_domain_free(secure->domain);
put_device:
	of_platform_device_destroy(&secure->pdev->dev, NULL);
put_node:
	of_node_put(np);
	kfree(secure);
	return ret;
}

static int venus_secure_buffer_free(struct venus_secure *secure,
				    struct venus_secure_buffer *buf)
{
	const struct qcom_scm_vmperm perm = {
		QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_RWX,
	};
	int ret;

	if (!buf->owners)
		return -EACCES;

	if (buf->mapped) {
		if (iommu_unmap(secure->domain, buf->iova, buf->size) != buf->size)
			return -EIO;
		buf->mapped = false;
	}

	ret = qcom_scm_assign_mem(virt_to_phys(buf->pages), buf->size,
				  &buf->owners, &perm, 1);
	if (ret)
		return ret;

	if (buf->iova)
		gen_pool_free(secure->pool, buf->iova, buf->size);
	free_pages_exact(buf->pages, buf->size);
	list_del(&buf->list);
	kfree(buf);
	return 0;
}

struct venus_secure_buffer *venus_secure_alloc(struct venus_core *core,
					      size_t size, u32 alignment,
					      dma_addr_t *iova)
{
	const struct qcom_scm_vmperm perm = {
		QCOM_SCM_VMID_CP_NON_PIXEL, QCOM_SCM_PERM_RW,
	};
	struct venus_secure *secure = core->secure;
	struct venus_secure_buffer *buf;
	struct genpool_data_align align = { .align = max_t(u32, PAGE_SIZE, alignment) };
	dma_addr_t dma;
	int ret;

	if (!secure)
		return ERR_PTR(-EOPNOTSUPP);
	if (!is_power_of_2(align.align) ||
	    alignment > core->res->cp_nonpixel_size)
		return ERR_PTR(-EINVAL);
	if (size > core->res->cp_nonpixel_size)
		return ERR_PTR(-E2BIG);
	if (size > SIZE_MAX - PAGE_SIZE + 1)
		return ERR_PTR(-EOVERFLOW);

	buf = kzalloc_obj(*buf);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	buf->size = PAGE_ALIGN(size);
	buf->pages = alloc_pages_exact(buf->size, GFP_KERNEL | __GFP_ZERO);
	if (!buf->pages) {
		ret = -ENOMEM;
		goto free_buf;
	}

	/* Flush the initial contents before removing HLOS access. */
	dma = dma_map_single(core->dev, buf->pages, buf->size, DMA_TO_DEVICE);
	if (dma_mapping_error(core->dev, dma)) {
		ret = -EIO;
		goto free_pages;
	}
	dma_unmap_single(core->dev, dma, buf->size, DMA_TO_DEVICE);

	mutex_lock(&secure->lock);
	buf->owners = BIT_ULL(QCOM_SCM_VMID_HLOS);
	list_add_tail(&buf->list, &secure->buffers);
	ret = qcom_scm_assign_mem(virt_to_phys(buf->pages), buf->size,
				  &buf->owners, &perm, 1);
	if (ret) {
		/* Do not reuse pages after an uncertain ownership transition. */
		dev_err(core->dev, "secure buffer assignment failed: %d; retaining pages\n",
			ret);
		buf->owners = 0;
		mutex_unlock(&secure->lock);
		return ERR_PTR(ret);
	}

	buf->iova = gen_pool_alloc_algo(secure->pool, buf->size,
				 gen_pool_first_fit_align, &align);
	if (!buf->iova) {
		ret = -ENOMEM;
		goto reclaim;
	}

	ret = iommu_map(secure->domain, buf->iova, virt_to_phys(buf->pages),
			buf->size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret)
		goto reclaim;

	buf->mapped = true;
	*iova = buf->iova;
	mutex_unlock(&secure->lock);
	return buf;

reclaim:
	buf->orphan = true;
	if (venus_secure_buffer_free(secure, buf))
		dev_err(core->dev, "secure allocation unwind failed; retaining pages\n");
	mutex_unlock(&secure->lock);
	return ERR_PTR(ret);
free_pages:
	free_pages_exact(buf->pages, buf->size);
free_buf:
	kfree(buf);
	return ERR_PTR(ret);
}

int venus_secure_free(struct venus_core *core, struct venus_secure_buffer *buf,
		      bool released)
{
	struct venus_secure *secure = core->secure;
	int ret = -EBUSY;

	mutex_lock(&secure->lock);
	buf->orphan = true;
	if (released)
		ret = venus_secure_buffer_free(secure, buf);
	if (ret)
		dev_err(core->dev, "secure buffer release failed: %d; retaining pages\n",
			ret);
	mutex_unlock(&secure->lock);
	return ret;
}

void venus_secure_reclaim(struct venus_core *core)
{
	struct venus_secure *secure = core->secure;
	struct venus_secure_buffer *buf, *next;

	if (!secure)
		return;

	mutex_lock(&secure->lock);
	list_for_each_entry_safe(buf, next, &secure->buffers, list) {
		if (buf->orphan && venus_secure_buffer_free(secure, buf))
			dev_err(core->dev, "secure buffer reclaim failed; retaining pages\n");
	}
	mutex_unlock(&secure->lock);
}

void venus_secure_deinit(struct venus_core *core, bool quiesced)
{
	struct venus_secure *secure = core->secure;
	struct venus_secure_buffer *buf, *next;

	if (!secure)
		return;

	/* A live firmware may still walk these tables or access its buffers. */
	if (!quiesced) {
		dev_err(core->dev, "Venus shutdown failed; retaining secure domain\n");
		core->secure = NULL;
		return;
	}

	list_for_each_entry_safe(buf, next, &secure->buffers, list) {
		if (venus_secure_buffer_free(secure, buf))
			dev_err(core->dev, "secure buffer reclaim failed; retaining pages\n");
	}

	iommu_detach_device(secure->domain, &secure->pdev->dev);
	iommu_domain_free(secure->domain);
	/* Unreclaimed buffers and their IOVAs must remain reserved. */
	of_platform_device_destroy(&secure->pdev->dev, NULL);
	if (list_empty(&secure->buffers)) {
		gen_pool_destroy(secure->pool);
		kfree(secure);
	}
	core->secure = NULL;
}
