// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/iopoll.h>

#include <drm/drm_managed.h>

#include "dpu_hw_mdss.h"
#include "dpu_hwio.h"
#include "dpu_hw_catalog.h"
#include "dpu_hw_merge3d.h"
#include "dpu_kms.h"
#include "dpu_trace.h"

#define MERGE_3D_MUX  0x000
#define MERGE_3D_MODE 0x004

static void dpu_hw_merge_3d_setup_3d_mode(struct dpu_hw_merge_3d *merge_3d,
			enum dpu_3d_blend_mode mode_3d)
{
	struct dpu_hw_blk_reg_map *c;
	u32 data;


	c = &merge_3d->hw;
	if (mode_3d == BLEND_3D_NONE) {
		DPU_REG_WRITE(c, MERGE_3D_MODE, 0);
		DPU_REG_WRITE(c, MERGE_3D_MUX, 0);
	} else {
		data = BIT(0) | ((mode_3d - 1) << 1);
		DPU_REG_WRITE(c, MERGE_3D_MODE, data);
	}
}

static void _setup_merge_3d_ops(struct dpu_hw_merge_3d *c)
{
	c->ops.setup_3d_mode = dpu_hw_merge_3d_setup_3d_mode;
};

/**
 * dpu_hw_merge_3d_init() - Initializes the merge_3d driver for the passed
 * merge3d catalog entry.
 * @dev:  Corresponding device for devres management
 * @cfg:  Pingpong catalog entry for which driver object is required
 * @addr: Mapped register io address of MDP
 * Return: Error code or allocated dpu_hw_merge_3d context
 */
struct dpu_hw_merge_3d *dpu_hw_merge_3d_init(struct drm_device *dev,
					     const struct dpu_merge_3d_cfg *cfg,
					     void __iomem *addr)
{
	struct dpu_hw_merge_3d *c;

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_PINGPONG;

	c->idx = cfg->id;
	c->caps = cfg;
	_setup_merge_3d_ops(c);

	return c;
}
