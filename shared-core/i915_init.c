/*
 * Copyright (c) 2007 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Copyright © 2002, 2003 David Dawes <dawes@xfree86.org>
 *                   2004 Sylvain Meyer
 *
 * GPL/BSD dual license
 */
#include "drmP.h"
#include "drm.h"
#include "drm_sarea.h"
#include "i915_drm.h"
#include "i915_drv.h"
#include "intel_bios.h"
#include "intel_drv.h"

/**
 * i915_probe_agp - get AGP bootup configuration
 * @pdev: PCI device
 * @aperture_size: returns AGP aperture configured size
 * @preallocated_size: returns size of BIOS preallocated AGP space
 *
 * Since Intel integrated graphics are UMA, the BIOS has to set aside
 * some RAM for the framebuffer at early boot.  This code figures out
 * how much was set aside so we can use it for our own purposes.
 */
int i915_probe_agp(struct pci_dev *pdev, unsigned long *aperture_size,
		   unsigned long *preallocated_size)
{
	struct pci_dev *bridge_dev;
	u16 tmp = 0;
	unsigned long overhead;

	bridge_dev = pci_get_bus_and_slot(0, PCI_DEVFN(0,0));
	if (!bridge_dev) {
		DRM_ERROR("bridge device not found\n");
		return -1;
	}

	/* Get the fb aperture size and "stolen" memory amount. */
	pci_read_config_word(bridge_dev, INTEL_GMCH_CTRL, &tmp);
	pci_dev_put(bridge_dev);

	*aperture_size = 1024 * 1024;
	*preallocated_size = 1024 * 1024;

	switch (pdev->device) {
	case PCI_DEVICE_ID_INTEL_82830_CGC:
	case PCI_DEVICE_ID_INTEL_82845G_IG:
	case PCI_DEVICE_ID_INTEL_82855GM_IG:
	case PCI_DEVICE_ID_INTEL_82865_IG:
		if ((tmp & INTEL_GMCH_MEM_MASK) == INTEL_GMCH_MEM_64M)
			*aperture_size *= 64;
		else
			*aperture_size *= 128;
		break;
	default:
		/* 9xx supports large sizes, just look at the length */
		*aperture_size = pci_resource_len(pdev, 2);
		break;
	}

	/*
	 * Some of the preallocated space is taken by the GTT
	 * and popup.  GTT is 1K per MB of aperture size, and popup is 4K.
	 */
	overhead = (*aperture_size / 1024) + 4096;
	switch (tmp & INTEL_855_GMCH_GMS_MASK) {
	case INTEL_855_GMCH_GMS_STOLEN_1M:
		break; /* 1M already */
	case INTEL_855_GMCH_GMS_STOLEN_4M:
		*preallocated_size *= 4;
		break;
	case INTEL_855_GMCH_GMS_STOLEN_8M:
		*preallocated_size *= 8;
		break;
	case INTEL_855_GMCH_GMS_STOLEN_16M:
		*preallocated_size *= 16;
		break;
	case INTEL_855_GMCH_GMS_STOLEN_32M:
		*preallocated_size *= 32;
		break;
	case INTEL_915G_GMCH_GMS_STOLEN_48M:
		*preallocated_size *= 48;
		break;
	case INTEL_915G_GMCH_GMS_STOLEN_64M:
		*preallocated_size *= 64;
		break;
	case INTEL_855_GMCH_GMS_DISABLED:
		DRM_ERROR("video memory is disabled\n");
		return -1;
	default:
		DRM_ERROR("unexpected GMCH_GMS value: 0x%02x\n",
			tmp & INTEL_855_GMCH_GMS_MASK);
		return -1;
	}
	*preallocated_size -= overhead;

	return 0;
}

static int i915_init_hwstatus(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_memrange_node *free_space;
	int ret = 0;

	/* Program Hardware Status Page */
	if (!IS_G33(dev)) {
		dev_priv->status_page_dmah = 
			drm_pci_alloc(dev, PAGE_SIZE, PAGE_SIZE, 0xffffffff);

		if (!dev_priv->status_page_dmah) {
			DRM_ERROR("Can not allocate hardware status page\n");
			ret = -ENOMEM;
			goto out;
		}
		dev_priv->hws_vaddr = dev_priv->status_page_dmah->vaddr;
		dev_priv->dma_status_page = dev_priv->status_page_dmah->busaddr;

		I915_WRITE(HWS_PGA, dev_priv->dma_status_page);
	} else {
		free_space = drm_memrange_search_free(&dev_priv->vram,
						      PAGE_SIZE,
						      PAGE_SIZE, 0);
		if (!free_space) {
			DRM_ERROR("No free vram available, aborting\n");
			ret = -ENOMEM;
			goto out;
		}

		dev_priv->hws = drm_memrange_get_block(free_space, PAGE_SIZE,
						       PAGE_SIZE);
		if (!dev_priv->hws) {
			DRM_ERROR("Unable to allocate or pin hw status page\n");
			ret = -EINVAL;
			goto out;
		}

		dev_priv->hws_agpoffset = dev_priv->hws->start;
		dev_priv->hws_map.offset = dev->agp->base +
			dev_priv->hws->start;
		dev_priv->hws_map.size = PAGE_SIZE;
		dev_priv->hws_map.type= 0;
		dev_priv->hws_map.flags= 0;
		dev_priv->hws_map.mtrr = 0;

		drm_core_ioremap(&dev_priv->hws_map, dev);
		if (dev_priv->hws_map.handle == NULL) {
			dev_priv->hws_agpoffset = 0;
			DRM_ERROR("can not ioremap virtual addr for"
					"G33 hw status page\n");
			ret = -ENOMEM;
			goto out_free;
		}
		dev_priv->hws_vaddr = dev_priv->hws_map.handle;
		I915_WRITE(HWS_PGA, dev_priv->hws_agpoffset);
	}

	memset(dev_priv->hws_vaddr, 0, PAGE_SIZE);

	DRM_DEBUG("Enabled hardware status page\n");

	return 0;

out_free:
	/* free hws */
out:
	return ret;
}

static void i915_cleanup_hwstatus(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	if (!IS_G33(dev)) {
		if (dev_priv->status_page_dmah)
			drm_pci_free(dev, dev_priv->status_page_dmah);
	} else {
		if (dev_priv->hws_map.handle)
			drm_core_ioremapfree(&dev_priv->hws_map, dev);
		if (dev_priv->hws)
			drm_memrange_put_block(dev_priv->hws);
	}
	I915_WRITE(HWS_PGA, 0x1ffff000);
}

static int i915_load_modeset_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	unsigned long agp_size, prealloc_size;
	int ret = 0;

	i915_probe_agp(dev->pdev, &agp_size, &prealloc_size);

	/* Basic memrange allocator for stolen space (aka vram) */
	drm_memrange_init(&dev_priv->vram, 0, prealloc_size);
	/* Let GEM Manage from end of prealloc space to end of aperture */
	i915_gem_do_init(dev, prealloc_size, agp_size);

	ret = i915_gem_init_ringbuffer(dev);
	if (ret)
		goto out;

	ret = i915_init_hwstatus(dev);
	if (ret)
		goto destroy_ringbuffer;

	/* Allow hardware batchbuffers unless told otherwise.
	 */
	dev_priv->allow_batchbuffer = 1;
	dev_priv->max_validate_buffers = I915_MAX_VALIDATE_BUFFERS;
	mutex_init(&dev_priv->cmdbuf_mutex);

	dev_priv->wq = create_singlethread_workqueue("i915");
	if (dev_priv->wq == 0) {
		DRM_DEBUG("Error\n");
		ret = -EINVAL;
		goto destroy_hws;
	}

	ret = intel_init_bios(dev);
	if (ret) {
		DRM_ERROR("failed to find VBIOS tables\n");
		ret = -ENODEV;
		goto destroy_wq;
	}

	intel_modeset_init(dev);
	drm_helper_initial_config(dev, false);

	dev->devname = kstrdup(DRIVER_NAME, GFP_KERNEL);
	if (!dev->devname) {
		ret = -ENOMEM;
		goto modeset_cleanup;
	}

	ret = drm_irq_install(dev);
	if (ret) {
		kfree(dev->devname);
		goto modeset_cleanup;
	}
	return 0;

modeset_cleanup:
	intel_modeset_cleanup(dev);
destroy_wq:
	destroy_workqueue(dev_priv->wq);
destroy_hws:
	i915_cleanup_hwstatus(dev);
destroy_ringbuffer:
	i915_gem_cleanup_ringbuffer(dev);
out:
	return ret;
}

/**
 * i915_driver_load - setup chip and create an initial config
 * @dev: DRM device
 * @flags: startup flags
 *
 * The driver load routine has to do several things:
 *   - drive output discovery via intel_modeset_init()
 *   - initialize the memory manager
 *   - allocate initial config memory
 *   - setup the DRM framebuffer with the allocated memory
 */
int i915_driver_load(struct drm_device *dev, unsigned long flags)
{
	struct drm_i915_private *dev_priv;
	int ret = 0;

	dev_priv = drm_alloc(sizeof(struct drm_i915_private), DRM_MEM_DRIVER);
	if (dev_priv == NULL)
		return -ENOMEM;

	memset(dev_priv, 0, sizeof(struct drm_i915_private));
	dev->dev_private = (void *)dev_priv;
	dev_priv->dev = dev;

	/* i915 has 4 more counters */
	dev->counters += 4;
	dev->types[6] = _DRM_STAT_IRQ;
	dev->types[7] = _DRM_STAT_PRIMARY;
	dev->types[8] = _DRM_STAT_SECONDARY;
	dev->types[9] = _DRM_STAT_DMA;

	if (IS_MOBILE(dev) || IS_I9XX(dev))
		dev_priv->cursor_needs_physical = true;
	else
		dev_priv->cursor_needs_physical = false;

	if (IS_I965G(dev) || IS_G33(dev))
		dev_priv->cursor_needs_physical = false;

	if (IS_I9XX(dev)) {
		pci_read_config_dword(dev->pdev, 0x5C, &dev_priv->stolen_base);
		DRM_DEBUG("stolen base %p\n", (void*)dev_priv->stolen_base);
	}

	if (IS_I9XX(dev)) {
		dev_priv->mmiobase = drm_get_resource_start(dev, 0);
		dev_priv->mmiolen = drm_get_resource_len(dev, 0);
		dev->mode_config.fb_base =
			drm_get_resource_start(dev, 2) & 0xff000000;
	} else if (drm_get_resource_start(dev, 1)) {
		dev_priv->mmiobase = drm_get_resource_start(dev, 1);
		dev_priv->mmiolen = drm_get_resource_len(dev, 1);
		dev->mode_config.fb_base =
			drm_get_resource_start(dev, 0) & 0xff000000;
	} else {
		DRM_ERROR("Unable to find MMIO registers\n");
		ret = -ENODEV;
		goto free_priv;
	}

	DRM_DEBUG("fb_base: 0x%08lx\n", dev->mode_config.fb_base);

	ret = drm_addmap(dev, dev_priv->mmiobase, dev_priv->mmiolen,
			 _DRM_REGISTERS, _DRM_KERNEL|_DRM_READ_ONLY|_DRM_DRIVER,
			 &dev_priv->mmio_map);
	if (ret != 0) {
		DRM_ERROR("Cannot add mapping for MMIO registers\n");
		goto free_priv;
	}

	INIT_LIST_HEAD(&dev_priv->mm.active_list);
	INIT_LIST_HEAD(&dev_priv->mm.flushing_list);
	INIT_LIST_HEAD(&dev_priv->mm.inactive_list);
	INIT_LIST_HEAD(&dev_priv->mm.request_list);
	dev_priv->mm.retire_timer.function = i915_gem_retire_timeout;
	dev_priv->mm.retire_timer.data = (unsigned long) dev;
	init_timer_deferrable (&dev_priv->mm.retire_timer);
	INIT_WORK(&dev_priv->mm.retire_task,
		  i915_gem_retire_handler);
	INIT_WORK(&dev_priv->user_interrupt_task,
		  i915_user_interrupt_handler);
	dev_priv->mm.next_gem_seqno = 1;

#ifdef __linux__
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
        intel_init_chipset_flush_compat(dev);
#endif
#endif

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		ret = i915_load_modeset_init(dev);
		if (ret < 0) {
			DRM_ERROR("failed to init modeset\n");
			goto out_rmmap;
		}
	}
	return 0;

out_rmmap:
	drm_rmmap(dev, dev_priv->mmio_map);
free_priv:
	drm_free(dev_priv, sizeof(struct drm_i915_private), DRM_MEM_DRIVER);
	return ret;
}

int i915_driver_unload(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	I915_WRITE(PRB0_CTL, 0);

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		drm_irq_uninstall(dev);
		intel_modeset_cleanup(dev);
		destroy_workqueue(dev_priv->wq);
	}

#if 0
	if (dev_priv->ring.virtual_start) {
		drm_core_ioremapfree(&dev_priv->ring.map, dev);
	}
#endif

#ifdef DRI2
	if (dev_priv->sarea_kmap.virtual) {
		drm_bo_kunmap(&dev_priv->sarea_kmap);
		dev_priv->sarea_kmap.virtual = NULL;
		dev->sigdata.lock = NULL;
	}

	if (dev_priv->sarea_bo) {
		mutex_lock(&dev->struct_mutex);
		drm_bo_usage_deref_locked(&dev_priv->sarea_bo);
		mutex_unlock(&dev->struct_mutex);
		dev_priv->sarea_bo = NULL;
	}
#endif
	i915_cleanup_hwstatus(dev);

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		mutex_lock(&dev->struct_mutex);
		i915_gem_cleanup_ringbuffer(dev);
		mutex_unlock(&dev->struct_mutex);
		drm_memrange_takedown(&dev_priv->vram);
		i915_gem_lastclose(dev);
	}

#ifdef __linux__
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
        intel_init_chipset_flush_compat(dev);
#endif
#endif

        DRM_DEBUG("%p\n", dev_priv->mmio_map);
        drm_rmmap(dev, dev_priv->mmio_map);

	drm_free(dev_priv, sizeof(*dev_priv), DRM_MEM_DRIVER);

	dev->dev_private = NULL;
	return 0;
}

int i915_master_create(struct drm_device *dev, struct drm_master *master)
{
	struct drm_i915_master_private *master_priv;
	unsigned long sareapage;
	int ret;

	master_priv = drm_calloc(1, sizeof(*master_priv), DRM_MEM_DRIVER);
	if (!master_priv)
		return -ENOMEM;

	/* prebuild the SAREA */
	sareapage = max(SAREA_MAX, PAGE_SIZE);
	ret = drm_addmap(dev, 0, sareapage, _DRM_SHM, _DRM_CONTAINS_LOCK|_DRM_DRIVER,
			 &master_priv->sarea);
	if (ret) {
		DRM_ERROR("SAREA setup failed\n");
		return ret;
	}
	master_priv->sarea_priv = master_priv->sarea->handle + sizeof(struct drm_sarea);
	master_priv->sarea_priv->pf_current_page = 0;

	master->driver_priv = master_priv;
	return 0;
}

void i915_master_destroy(struct drm_device *dev, struct drm_master *master)
{
	struct drm_i915_master_private *master_priv = master->driver_priv;

	if (!master_priv)
		return;

	if (master_priv->sarea)
		drm_rmmap(dev, master_priv->sarea);
		
	drm_free(master_priv, sizeof(*master_priv), DRM_MEM_DRIVER);

	master->driver_priv = NULL;
}

void i915_driver_preclose(struct drm_device * dev, struct drm_file *file_priv)
{
        struct drm_i915_private *dev_priv = dev->dev_private;
	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		i915_mem_release(dev, file_priv, dev_priv->agp_heap);
}

void i915_driver_lastclose(struct drm_device * dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return;

#ifdef I915_HAVE_BUFFER
	if (dev_priv->val_bufs) {
		vfree(dev_priv->val_bufs);
		dev_priv->val_bufs = NULL;
	}
#endif

	i915_gem_lastclose(dev);

	if (dev_priv->agp_heap)
		i915_mem_takedown(&(dev_priv->agp_heap));

#if defined(DRI2)
	if (dev_priv->sarea_kmap.virtual) {
		drm_bo_kunmap(&dev_priv->sarea_kmap);
		dev_priv->sarea_kmap.virtual = NULL;
		dev->control->master->lock.hw_lock = NULL;
		dev->sigdata.lock = NULL;
	}

	if (dev_priv->sarea_bo) {
		mutex_lock(&dev->struct_mutex);
		drm_bo_usage_deref_locked(&dev_priv->sarea_bo);
		mutex_unlock(&dev->struct_mutex);
		dev_priv->sarea_bo = NULL;
	}
#endif
	
	i915_dma_cleanup(dev);
}

int i915_driver_firstopen(struct drm_device *dev)
{
	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return 0;
#if defined(I915_HAVE_BUFFER) && defined(I915_TTM)
	drm_bo_driver_init(dev);
#endif
	return 0;
}