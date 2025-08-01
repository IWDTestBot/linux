// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_of.h>

#include "dsi.h"
#include "dsi.xml.h"
#include "sfpb.xml.h"
#include "dsi_cfg.h"
#include "msm_dsc_helper.h"
#include "msm_kms.h"
#include "msm_gem.h"
#include "phy/dsi_phy.h"

#define DSI_RESET_TOGGLE_DELAY_MS 20

static int dsi_populate_dsc_params(struct msm_dsi_host *msm_host, struct drm_dsc_config *dsc);

static int dsi_get_version(const void __iomem *base, u32 *major, u32 *minor)
{
	u32 ver;

	if (!major || !minor)
		return -EINVAL;

	/*
	 * From DSI6G(v3), addition of a 6G_HW_VERSION register at offset 0
	 * makes all other registers 4-byte shifted down.
	 *
	 * In order to identify between DSI6G(v3) and beyond, and DSIv2 and
	 * older, we read the DSI_VERSION register without any shift(offset
	 * 0x1f0). In the case of DSIv2, this hast to be a non-zero value. In
	 * the case of DSI6G, this has to be zero (the offset points to a
	 * scratch register which we never touch)
	 */

	ver = readl(base + REG_DSI_VERSION);
	if (ver) {
		/* older dsi host, there is no register shift */
		ver = FIELD(ver, DSI_VERSION_MAJOR);
		if (ver <= MSM_DSI_VER_MAJOR_V2) {
			/* old versions */
			*major = ver;
			*minor = 0;
			return 0;
		} else {
			return -EINVAL;
		}
	} else {
		/*
		 * newer host, offset 0 has 6G_HW_VERSION, the rest of the
		 * registers are shifted down, read DSI_VERSION again with
		 * the shifted offset
		 */
		ver = readl(base + DSI_6G_REG_SHIFT + REG_DSI_VERSION);
		ver = FIELD(ver, DSI_VERSION_MAJOR);
		if (ver == MSM_DSI_VER_MAJOR_6G) {
			/* 6G version */
			*major = ver;
			*minor = readl(base + REG_DSI_6G_HW_VERSION);
			return 0;
		} else {
			return -EINVAL;
		}
	}
}

#define DSI_ERR_STATE_ACK			0x0000
#define DSI_ERR_STATE_TIMEOUT			0x0001
#define DSI_ERR_STATE_DLN0_PHY			0x0002
#define DSI_ERR_STATE_FIFO			0x0004
#define DSI_ERR_STATE_MDP_FIFO_UNDERFLOW	0x0008
#define DSI_ERR_STATE_INTERLEAVE_OP_CONTENTION	0x0010
#define DSI_ERR_STATE_PLL_UNLOCKED		0x0020

#define DSI_CLK_CTRL_ENABLE_CLKS	\
		(DSI_CLK_CTRL_AHBS_HCLK_ON | DSI_CLK_CTRL_AHBM_SCLK_ON | \
		DSI_CLK_CTRL_PCLK_ON | DSI_CLK_CTRL_DSICLK_ON | \
		DSI_CLK_CTRL_BYTECLK_ON | DSI_CLK_CTRL_ESCCLK_ON | \
		DSI_CLK_CTRL_FORCE_ON_DYN_AHBM_HCLK)

struct msm_dsi_host {
	struct mipi_dsi_host base;

	struct platform_device *pdev;
	struct drm_device *dev;

	int id;

	void __iomem *ctrl_base;
	phys_addr_t ctrl_size;
	struct regulator_bulk_data *supplies;

	int num_bus_clks;
	struct clk_bulk_data bus_clks[DSI_BUS_CLK_MAX];

	struct clk *byte_clk;
	struct clk *esc_clk;
	struct clk *pixel_clk;
	struct clk *byte_intf_clk;

	/*
	 * Clocks which needs to be properly parented between DISPCC and DSI PHY
	 * PLL:
	 */
	struct clk *byte_src_clk;
	struct clk *pixel_src_clk;
	struct clk *dsi_pll_byte_clk;
	struct clk *dsi_pll_pixel_clk;

	unsigned long byte_clk_rate;
	unsigned long byte_intf_clk_rate;
	unsigned long pixel_clk_rate;
	unsigned long esc_clk_rate;

	/* DSI v2 specific clocks */
	struct clk *src_clk;

	unsigned long src_clk_rate;

	const struct msm_dsi_cfg_handler *cfg_hnd;

	struct completion dma_comp;
	struct completion video_comp;
	struct mutex dev_mutex;
	struct mutex cmd_mutex;
	spinlock_t intr_lock; /* Protect interrupt ctrl register */

	u32 err_work_state;
	struct work_struct err_work;
	struct workqueue_struct *workqueue;

	/* DSI 6G TX buffer*/
	struct drm_gem_object *tx_gem_obj;
	struct drm_gpuvm *vm;

	/* DSI v2 TX buffer */
	void *tx_buf;
	dma_addr_t tx_buf_paddr;

	int tx_size;

	u8 *rx_buf;

	struct regmap *sfpb;

	struct drm_display_mode *mode;
	struct drm_dsc_config *dsc;

	/* connected device info */
	unsigned int channel;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;

	/* lane data parsed via DT */
	int dlane_swap;
	int num_data_lanes;

	/* from phy DT */
	bool cphy_mode;

	u32 dma_cmd_ctrl_restore;

	bool registered;
	bool power_on;
	bool enabled;
	int irq;
};

static inline u32 dsi_read(struct msm_dsi_host *msm_host, u32 reg)
{
	return readl(msm_host->ctrl_base + reg);
}

static inline void dsi_write(struct msm_dsi_host *msm_host, u32 reg, u32 data)
{
	writel(data, msm_host->ctrl_base + reg);
}

static const struct msm_dsi_cfg_handler *
dsi_get_config(struct msm_dsi_host *msm_host)
{
	const struct msm_dsi_cfg_handler *cfg_hnd = NULL;
	struct device *dev = &msm_host->pdev->dev;
	struct clk *ahb_clk;
	int ret;
	u32 major = 0, minor = 0;

	ahb_clk = msm_clk_get(msm_host->pdev, "iface");
	if (IS_ERR(ahb_clk)) {
		dev_err_probe(dev, PTR_ERR(ahb_clk), "%s: cannot get interface clock\n",
			      __func__);
		goto exit;
	}

	pm_runtime_get_sync(dev);

	ret = clk_prepare_enable(ahb_clk);
	if (ret) {
		dev_err_probe(dev, ret, "%s: unable to enable ahb_clk\n", __func__);
		goto runtime_put;
	}

	ret = dsi_get_version(msm_host->ctrl_base, &major, &minor);
	if (ret) {
		dev_err_probe(dev, ret, "%s: Invalid version\n", __func__);
		goto disable_clks;
	}

	cfg_hnd = msm_dsi_cfg_get(major, minor);

	DBG("%s: Version %x:%x\n", __func__, major, minor);

disable_clks:
	clk_disable_unprepare(ahb_clk);
runtime_put:
	pm_runtime_put_sync(dev);
exit:
	return cfg_hnd;
}

static inline struct msm_dsi_host *to_msm_dsi_host(struct mipi_dsi_host *host)
{
	return container_of(host, struct msm_dsi_host, base);
}

int dsi_clk_init_v2(struct msm_dsi_host *msm_host)
{
	struct platform_device *pdev = msm_host->pdev;
	int ret = 0;

	msm_host->src_clk = msm_clk_get(pdev, "src");

	if (IS_ERR(msm_host->src_clk)) {
		ret = PTR_ERR(msm_host->src_clk);
		pr_err("%s: can't find src clock. ret=%d\n",
			__func__, ret);
		msm_host->src_clk = NULL;
		return ret;
	}

	return ret;
}

int dsi_clk_init_6g_v2(struct msm_dsi_host *msm_host)
{
	struct platform_device *pdev = msm_host->pdev;
	int ret = 0;

	msm_host->byte_intf_clk = msm_clk_get(pdev, "byte_intf");
	if (IS_ERR(msm_host->byte_intf_clk)) {
		ret = PTR_ERR(msm_host->byte_intf_clk);
		pr_err("%s: can't find byte_intf clock. ret=%d\n",
			__func__, ret);
	}

	return ret;
}

int dsi_clk_init_6g_v2_9(struct msm_dsi_host *msm_host)
{
	struct device *dev = &msm_host->pdev->dev;
	int ret;

	ret = dsi_clk_init_6g_v2(msm_host);
	if (ret)
		return ret;

	msm_host->byte_src_clk = devm_clk_get(dev, "byte_src");
	if (IS_ERR(msm_host->byte_src_clk))
		return dev_err_probe(dev, PTR_ERR(msm_host->byte_src_clk),
				     "can't get byte_src clock\n");

	msm_host->dsi_pll_byte_clk = devm_clk_get(dev, "dsi_pll_byte");
	if (IS_ERR(msm_host->dsi_pll_byte_clk))
		return dev_err_probe(dev, PTR_ERR(msm_host->dsi_pll_byte_clk),
				     "can't get dsi_pll_byte clock\n");

	msm_host->pixel_src_clk = devm_clk_get(dev, "pixel_src");
	if (IS_ERR(msm_host->pixel_src_clk))
		return dev_err_probe(dev, PTR_ERR(msm_host->pixel_src_clk),
				     "can't get pixel_src clock\n");

	msm_host->dsi_pll_pixel_clk = devm_clk_get(dev, "dsi_pll_pixel");
	if (IS_ERR(msm_host->dsi_pll_pixel_clk))
		return dev_err_probe(dev, PTR_ERR(msm_host->dsi_pll_pixel_clk),
				     "can't get dsi_pll_pixel clock\n");

	return 0;
}

static int dsi_clk_init(struct msm_dsi_host *msm_host)
{
	struct platform_device *pdev = msm_host->pdev;
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	const struct msm_dsi_config *cfg = cfg_hnd->cfg;
	int i, ret = 0;

	/* get bus clocks */
	for (i = 0; i < cfg->num_bus_clks; i++)
		msm_host->bus_clks[i].id = cfg->bus_clk_names[i];
	msm_host->num_bus_clks = cfg->num_bus_clks;

	ret = devm_clk_bulk_get(&pdev->dev, msm_host->num_bus_clks, msm_host->bus_clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "Unable to get clocks\n");

	/* get link and source clocks */
	msm_host->byte_clk = msm_clk_get(pdev, "byte");
	if (IS_ERR(msm_host->byte_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(msm_host->byte_clk),
				     "%s: can't find dsi_byte clock\n",
				     __func__);

	msm_host->pixel_clk = msm_clk_get(pdev, "pixel");
	if (IS_ERR(msm_host->pixel_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(msm_host->pixel_clk),
				     "%s: can't find dsi_pixel clock\n",
				     __func__);

	msm_host->esc_clk = msm_clk_get(pdev, "core");
	if (IS_ERR(msm_host->esc_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(msm_host->esc_clk),
				     "%s: can't find dsi_esc clock\n",
				     __func__);

	if (cfg_hnd->ops->clk_init_ver)
		ret = cfg_hnd->ops->clk_init_ver(msm_host);

	return ret;
}

int msm_dsi_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_dsi *msm_dsi = platform_get_drvdata(pdev);
	struct mipi_dsi_host *host = msm_dsi->host;
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	if (!msm_host->cfg_hnd)
		return 0;

	clk_bulk_disable_unprepare(msm_host->num_bus_clks, msm_host->bus_clks);

	return 0;
}

int msm_dsi_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_dsi *msm_dsi = platform_get_drvdata(pdev);
	struct mipi_dsi_host *host = msm_dsi->host;
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	if (!msm_host->cfg_hnd)
		return 0;

	return clk_bulk_prepare_enable(msm_host->num_bus_clks, msm_host->bus_clks);
}

int dsi_link_clk_set_rate_6g(struct msm_dsi_host *msm_host)
{
	int ret;

	DBG("Set clk rates: pclk=%lu, byteclk=%lu",
	    msm_host->pixel_clk_rate, msm_host->byte_clk_rate);

	ret = dev_pm_opp_set_rate(&msm_host->pdev->dev,
				  msm_host->byte_clk_rate);
	if (ret) {
		pr_err("%s: dev_pm_opp_set_rate failed %d\n", __func__, ret);
		return ret;
	}

	ret = clk_set_rate(msm_host->pixel_clk, msm_host->pixel_clk_rate);
	if (ret) {
		pr_err("%s: Failed to set rate pixel clk, %d\n", __func__, ret);
		return ret;
	}

	if (msm_host->byte_intf_clk) {
		ret = clk_set_rate(msm_host->byte_intf_clk, msm_host->byte_intf_clk_rate);
		if (ret) {
			pr_err("%s: Failed to set rate byte intf clk, %d\n",
			       __func__, ret);
			return ret;
		}
	}

	return 0;
}

int dsi_link_clk_set_rate_6g_v2_9(struct msm_dsi_host *msm_host)
{
	struct device *dev = &msm_host->pdev->dev;
	int ret;

	/*
	 * DSI PHY PLLs have to be enabled to allow reparenting to them, so
	 * cannot use assigned-clock-parents.
	 */
	ret = clk_set_parent(msm_host->byte_src_clk, msm_host->dsi_pll_byte_clk);
	if (ret)
		dev_err(dev, "Failed to parent byte_src -> dsi_pll_byte: %d\n", ret);

	ret = clk_set_parent(msm_host->pixel_src_clk, msm_host->dsi_pll_pixel_clk);
	if (ret)
		dev_err(dev, "Failed to parent pixel_src -> dsi_pll_pixel: %d\n", ret);

	return dsi_link_clk_set_rate_6g(msm_host);
}

int dsi_link_clk_enable_6g(struct msm_dsi_host *msm_host)
{
	int ret;

	ret = clk_prepare_enable(msm_host->esc_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi esc clk\n", __func__);
		goto error;
	}

	ret = clk_prepare_enable(msm_host->byte_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi byte clk\n", __func__);
		goto byte_clk_err;
	}

	ret = clk_prepare_enable(msm_host->pixel_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi pixel clk\n", __func__);
		goto pixel_clk_err;
	}

	ret = clk_prepare_enable(msm_host->byte_intf_clk);
	if (ret) {
		pr_err("%s: Failed to enable byte intf clk\n",
			   __func__);
		goto byte_intf_clk_err;
	}

	return 0;

byte_intf_clk_err:
	clk_disable_unprepare(msm_host->pixel_clk);
pixel_clk_err:
	clk_disable_unprepare(msm_host->byte_clk);
byte_clk_err:
	clk_disable_unprepare(msm_host->esc_clk);
error:
	return ret;
}

int dsi_link_clk_set_rate_v2(struct msm_dsi_host *msm_host)
{
	int ret;

	DBG("Set clk rates: pclk=%lu, byteclk=%lu, esc_clk=%lu, dsi_src_clk=%lu",
	    msm_host->pixel_clk_rate, msm_host->byte_clk_rate,
	    msm_host->esc_clk_rate, msm_host->src_clk_rate);

	ret = clk_set_rate(msm_host->byte_clk, msm_host->byte_clk_rate);
	if (ret) {
		pr_err("%s: Failed to set rate byte clk, %d\n", __func__, ret);
		return ret;
	}

	ret = clk_set_rate(msm_host->esc_clk, msm_host->esc_clk_rate);
	if (ret) {
		pr_err("%s: Failed to set rate esc clk, %d\n", __func__, ret);
		return ret;
	}

	ret = clk_set_rate(msm_host->src_clk, msm_host->src_clk_rate);
	if (ret) {
		pr_err("%s: Failed to set rate src clk, %d\n", __func__, ret);
		return ret;
	}

	ret = clk_set_rate(msm_host->pixel_clk, msm_host->pixel_clk_rate);
	if (ret) {
		pr_err("%s: Failed to set rate pixel clk, %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

int dsi_link_clk_enable_v2(struct msm_dsi_host *msm_host)
{
	int ret;

	ret = clk_prepare_enable(msm_host->byte_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi byte clk\n", __func__);
		goto error;
	}

	ret = clk_prepare_enable(msm_host->esc_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi esc clk\n", __func__);
		goto esc_clk_err;
	}

	ret = clk_prepare_enable(msm_host->src_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi src clk\n", __func__);
		goto src_clk_err;
	}

	ret = clk_prepare_enable(msm_host->pixel_clk);
	if (ret) {
		pr_err("%s: Failed to enable dsi pixel clk\n", __func__);
		goto pixel_clk_err;
	}

	return 0;

pixel_clk_err:
	clk_disable_unprepare(msm_host->src_clk);
src_clk_err:
	clk_disable_unprepare(msm_host->esc_clk);
esc_clk_err:
	clk_disable_unprepare(msm_host->byte_clk);
error:
	return ret;
}

void dsi_link_clk_disable_6g(struct msm_dsi_host *msm_host)
{
	/* Drop the performance state vote */
	dev_pm_opp_set_rate(&msm_host->pdev->dev, 0);
	clk_disable_unprepare(msm_host->esc_clk);
	clk_disable_unprepare(msm_host->pixel_clk);
	clk_disable_unprepare(msm_host->byte_intf_clk);
	clk_disable_unprepare(msm_host->byte_clk);
}

void dsi_link_clk_disable_v2(struct msm_dsi_host *msm_host)
{
	clk_disable_unprepare(msm_host->pixel_clk);
	clk_disable_unprepare(msm_host->src_clk);
	clk_disable_unprepare(msm_host->esc_clk);
	clk_disable_unprepare(msm_host->byte_clk);
}

/**
 * dsi_adjust_pclk_for_compression() - Adjust the pclk rate for compression case
 * @mode: The selected mode for the DSI output
 * @dsc: DRM DSC configuration for this DSI output
 *
 * Adjust the pclk rate by calculating a new hdisplay proportional to
 * the compression ratio such that:
 *     new_hdisplay = old_hdisplay * compressed_bpp / uncompressed_bpp
 *
 * Porches do not need to be adjusted:
 * - For VIDEO mode they are not compressed by DSC and are passed as is.
 * - For CMD mode there are no actual porches. Instead these fields
 *   currently represent the overhead to the image data transfer. As such, they
 *   are calculated for the final mode parameters (after the compression) and
 *   are not to be adjusted too.
 *
 *  FIXME: Reconsider this if/when CMD mode handling is rewritten to use
 *  transfer time and data overhead as a starting point of the calculations.
 */
static unsigned long dsi_adjust_pclk_for_compression(const struct drm_display_mode *mode,
		const struct drm_dsc_config *dsc)
{
	int new_hdisplay = DIV_ROUND_UP(mode->hdisplay * drm_dsc_get_bpp_int(dsc),
			dsc->bits_per_component * 3);

	int new_htotal = mode->htotal - mode->hdisplay + new_hdisplay;

	return mult_frac(mode->clock * 1000u, new_htotal, mode->htotal);
}

static unsigned long dsi_get_pclk_rate(const struct drm_display_mode *mode,
		const struct drm_dsc_config *dsc, bool is_bonded_dsi)
{
	unsigned long pclk_rate;

	pclk_rate = mode->clock * 1000u;

	if (dsc)
		pclk_rate = dsi_adjust_pclk_for_compression(mode, dsc);

	/*
	 * For bonded DSI mode, the current DRM mode has the complete width of the
	 * panel. Since, the complete panel is driven by two DSI controllers,
	 * the clock rates have to be split between the two dsi controllers.
	 * Adjust the byte and pixel clock rates for each dsi host accordingly.
	 */
	if (is_bonded_dsi)
		pclk_rate /= 2;

	return pclk_rate;
}

unsigned long dsi_byte_clk_get_rate(struct mipi_dsi_host *host, bool is_bonded_dsi,
				    const struct drm_display_mode *mode)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	u8 lanes = msm_host->lanes;
	u32 bpp = mipi_dsi_pixel_format_to_bpp(msm_host->format);
	unsigned long pclk_rate = dsi_get_pclk_rate(mode, msm_host->dsc, is_bonded_dsi);
	unsigned long pclk_bpp;

	if (lanes == 0) {
		pr_err("%s: forcing mdss_dsi lanes to 1\n", __func__);
		lanes = 1;
	}

	/* CPHY "byte_clk" is in units of 16 bits */
	if (msm_host->cphy_mode)
		pclk_bpp = mult_frac(pclk_rate, bpp, 16 * lanes);
	else
		pclk_bpp = mult_frac(pclk_rate, bpp, 8 * lanes);

	return pclk_bpp;
}

static void dsi_calc_pclk(struct msm_dsi_host *msm_host, bool is_bonded_dsi)
{
	msm_host->pixel_clk_rate = dsi_get_pclk_rate(msm_host->mode, msm_host->dsc, is_bonded_dsi);
	msm_host->byte_clk_rate = dsi_byte_clk_get_rate(&msm_host->base, is_bonded_dsi,
							msm_host->mode);

	DBG("pclk=%lu, bclk=%lu", msm_host->pixel_clk_rate,
				msm_host->byte_clk_rate);
}

int dsi_calc_clk_rate_6g(struct msm_dsi_host *msm_host, bool is_bonded_dsi)
{
	if (!msm_host->mode) {
		pr_err("%s: mode not set\n", __func__);
		return -EINVAL;
	}

	dsi_calc_pclk(msm_host, is_bonded_dsi);
	msm_host->esc_clk_rate = clk_get_rate(msm_host->esc_clk);
	return 0;
}

int dsi_calc_clk_rate_v2(struct msm_dsi_host *msm_host, bool is_bonded_dsi)
{
	u32 bpp = mipi_dsi_pixel_format_to_bpp(msm_host->format);
	unsigned int esc_mhz, esc_div;
	unsigned long byte_mhz;

	dsi_calc_pclk(msm_host, is_bonded_dsi);

	msm_host->src_clk_rate = mult_frac(msm_host->pixel_clk_rate, bpp, 8);

	/*
	 * esc clock is byte clock followed by a 4 bit divider,
	 * we need to find an escape clock frequency within the
	 * mipi DSI spec range within the maximum divider limit
	 * We iterate here between an escape clock frequencey
	 * between 20 Mhz to 5 Mhz and pick up the first one
	 * that can be supported by our divider
	 */

	byte_mhz = msm_host->byte_clk_rate / 1000000;

	for (esc_mhz = 20; esc_mhz >= 5; esc_mhz--) {
		esc_div = DIV_ROUND_UP(byte_mhz, esc_mhz);

		/*
		 * TODO: Ideally, we shouldn't know what sort of divider
		 * is available in mmss_cc, we're just assuming that
		 * it'll always be a 4 bit divider. Need to come up with
		 * a better way here.
		 */
		if (esc_div >= 1 && esc_div <= 16)
			break;
	}

	if (esc_mhz < 5)
		return -EINVAL;

	msm_host->esc_clk_rate = msm_host->byte_clk_rate / esc_div;

	DBG("esc=%lu, src=%lu", msm_host->esc_clk_rate,
		msm_host->src_clk_rate);

	return 0;
}

static void dsi_intr_ctrl(struct msm_dsi_host *msm_host, u32 mask, int enable)
{
	u32 intr;
	unsigned long flags;

	spin_lock_irqsave(&msm_host->intr_lock, flags);
	intr = dsi_read(msm_host, REG_DSI_INTR_CTRL);

	if (enable)
		intr |= mask;
	else
		intr &= ~mask;

	DBG("intr=%x enable=%d", intr, enable);

	dsi_write(msm_host, REG_DSI_INTR_CTRL, intr);
	spin_unlock_irqrestore(&msm_host->intr_lock, flags);
}

static inline enum dsi_traffic_mode dsi_get_traffic_mode(const u32 mode_flags)
{
	if (mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
		return BURST_MODE;
	else if (mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		return NON_BURST_SYNCH_PULSE;

	return NON_BURST_SYNCH_EVENT;
}

static inline enum dsi_vid_dst_format
dsi_get_vid_fmt(const enum mipi_dsi_pixel_format mipi_fmt)
{
	switch (mipi_fmt) {
	case MIPI_DSI_FMT_RGB888:	return VID_DST_FORMAT_RGB888;
	case MIPI_DSI_FMT_RGB666:	return VID_DST_FORMAT_RGB666_LOOSE;
	case MIPI_DSI_FMT_RGB666_PACKED:	return VID_DST_FORMAT_RGB666;
	case MIPI_DSI_FMT_RGB565:	return VID_DST_FORMAT_RGB565;
	default:			return VID_DST_FORMAT_RGB888;
	}
}

static inline enum dsi_cmd_dst_format
dsi_get_cmd_fmt(const enum mipi_dsi_pixel_format mipi_fmt)
{
	switch (mipi_fmt) {
	case MIPI_DSI_FMT_RGB888:	return CMD_DST_FORMAT_RGB888;
	case MIPI_DSI_FMT_RGB666_PACKED:
	case MIPI_DSI_FMT_RGB666:	return CMD_DST_FORMAT_RGB666;
	case MIPI_DSI_FMT_RGB565:	return CMD_DST_FORMAT_RGB565;
	default:			return CMD_DST_FORMAT_RGB888;
	}
}

static void dsi_ctrl_disable(struct msm_dsi_host *msm_host)
{
	dsi_write(msm_host, REG_DSI_CTRL, 0);
}

bool msm_dsi_host_is_wide_bus_enabled(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	return msm_host->dsc &&
		(msm_host->cfg_hnd->major == MSM_DSI_VER_MAJOR_6G &&
		 msm_host->cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V2_5_0);
}

static void dsi_ctrl_enable(struct msm_dsi_host *msm_host,
			struct msm_dsi_phy_shared_timings *phy_shared_timings, struct msm_dsi_phy *phy)
{
	u32 flags = msm_host->mode_flags;
	enum mipi_dsi_pixel_format mipi_fmt = msm_host->format;
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	u32 data = 0, lane_ctrl = 0;

	if (flags & MIPI_DSI_MODE_VIDEO) {
		if (flags & MIPI_DSI_MODE_VIDEO_HSE)
			data |= DSI_VID_CFG0_PULSE_MODE_HSA_HE;
		if (flags & MIPI_DSI_MODE_VIDEO_NO_HFP)
			data |= DSI_VID_CFG0_HFP_POWER_STOP;
		if (flags & MIPI_DSI_MODE_VIDEO_NO_HBP)
			data |= DSI_VID_CFG0_HBP_POWER_STOP;
		if (flags & MIPI_DSI_MODE_VIDEO_NO_HSA)
			data |= DSI_VID_CFG0_HSA_POWER_STOP;
		/* Always set low power stop mode for BLLP
		 * to let command engine send packets
		 */
		data |= DSI_VID_CFG0_EOF_BLLP_POWER_STOP |
			DSI_VID_CFG0_BLLP_POWER_STOP;
		data |= DSI_VID_CFG0_TRAFFIC_MODE(dsi_get_traffic_mode(flags));
		data |= DSI_VID_CFG0_DST_FORMAT(dsi_get_vid_fmt(mipi_fmt));
		data |= DSI_VID_CFG0_VIRT_CHANNEL(msm_host->channel);
		if (msm_dsi_host_is_wide_bus_enabled(&msm_host->base))
			data |= DSI_VID_CFG0_DATABUS_WIDEN;
		dsi_write(msm_host, REG_DSI_VID_CFG0, data);

		/* Do not swap RGB colors */
		data = DSI_VID_CFG1_RGB_SWAP(SWAP_RGB);
		dsi_write(msm_host, REG_DSI_VID_CFG1, 0);
	} else {
		/* Do not swap RGB colors */
		data = DSI_CMD_CFG0_RGB_SWAP(SWAP_RGB);
		data |= DSI_CMD_CFG0_DST_FORMAT(dsi_get_cmd_fmt(mipi_fmt));
		dsi_write(msm_host, REG_DSI_CMD_CFG0, data);

		data = DSI_CMD_CFG1_WR_MEM_START(MIPI_DCS_WRITE_MEMORY_START) |
			DSI_CMD_CFG1_WR_MEM_CONTINUE(
					MIPI_DCS_WRITE_MEMORY_CONTINUE);
		/* Always insert DCS command */
		data |= DSI_CMD_CFG1_INSERT_DCS_COMMAND;
		dsi_write(msm_host, REG_DSI_CMD_CFG1, data);

		if (cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) {
			data = dsi_read(msm_host, REG_DSI_CMD_MODE_MDP_CTRL2);

			if (cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_3)
				data |= DSI_CMD_MODE_MDP_CTRL2_BURST_MODE;

			if (msm_dsi_host_is_wide_bus_enabled(&msm_host->base))
				data |= DSI_CMD_MODE_MDP_CTRL2_DATABUS_WIDEN;

			dsi_write(msm_host, REG_DSI_CMD_MODE_MDP_CTRL2, data);
		}
	}

	dsi_write(msm_host, REG_DSI_CMD_DMA_CTRL,
			DSI_CMD_DMA_CTRL_FROM_FRAME_BUFFER |
			DSI_CMD_DMA_CTRL_LOW_POWER);

	data = 0;
	/* Always assume dedicated TE pin */
	data |= DSI_TRIG_CTRL_TE;
	data |= DSI_TRIG_CTRL_MDP_TRIGGER(TRIGGER_NONE);
	data |= DSI_TRIG_CTRL_DMA_TRIGGER(TRIGGER_SW);
	data |= DSI_TRIG_CTRL_STREAM(msm_host->channel);
	if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
		(cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_2))
		data |= DSI_TRIG_CTRL_BLOCK_DMA_WITHIN_FRAME;
	dsi_write(msm_host, REG_DSI_TRIG_CTRL, data);

	data = DSI_CLKOUT_TIMING_CTRL_T_CLK_POST(phy_shared_timings->clk_post) |
		DSI_CLKOUT_TIMING_CTRL_T_CLK_PRE(phy_shared_timings->clk_pre);
	dsi_write(msm_host, REG_DSI_CLKOUT_TIMING_CTRL, data);

	if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
	    (cfg_hnd->minor > MSM_DSI_6G_VER_MINOR_V1_0) &&
	    phy_shared_timings->clk_pre_inc_by_2)
		dsi_write(msm_host, REG_DSI_T_CLK_PRE_EXTEND,
			  DSI_T_CLK_PRE_EXTEND_INC_BY_2_BYTECLK);

	data = 0;
	if (!(flags & MIPI_DSI_MODE_NO_EOT_PACKET))
		data |= DSI_EOT_PACKET_CTRL_TX_EOT_APPEND;
	dsi_write(msm_host, REG_DSI_EOT_PACKET_CTRL, data);

	/* allow only ack-err-status to generate interrupt */
	dsi_write(msm_host, REG_DSI_ERR_INT_MASK0, 0x13ff3fe0);

	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_ERROR, 1);

	dsi_write(msm_host, REG_DSI_CLK_CTRL, DSI_CLK_CTRL_ENABLE_CLKS);

	data = DSI_CTRL_CLK_EN;

	DBG("lane number=%d", msm_host->lanes);
	data |= ((DSI_CTRL_LANE0 << msm_host->lanes) - DSI_CTRL_LANE0);

	dsi_write(msm_host, REG_DSI_LANE_SWAP_CTRL,
		  DSI_LANE_SWAP_CTRL_DLN_SWAP_SEL(msm_host->dlane_swap));

	if (!(flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)) {
		lane_ctrl = dsi_read(msm_host, REG_DSI_LANE_CTRL);

		if (msm_dsi_phy_set_continuous_clock(phy, true))
			lane_ctrl &= ~DSI_LANE_CTRL_HS_REQ_SEL_PHY;

		dsi_write(msm_host, REG_DSI_LANE_CTRL,
			lane_ctrl | DSI_LANE_CTRL_CLKLN_HS_FORCE_REQUEST);
	}

	data |= DSI_CTRL_ENABLE;

	dsi_write(msm_host, REG_DSI_CTRL, data);

	if (msm_host->cphy_mode)
		dsi_write(msm_host, REG_DSI_CPHY_MODE_CTRL, BIT(0));
}

static void dsi_update_dsc_timing(struct msm_dsi_host *msm_host, bool is_cmd_mode)
{
	struct drm_dsc_config *dsc = msm_host->dsc;
	u32 reg, reg_ctrl, reg_ctrl2;
	u32 slice_per_intf, total_bytes_per_intf;
	u32 pkt_per_line;
	u32 eol_byte_num;
	u32 bytes_per_pkt;

	/* first calculate dsc parameters and then program
	 * compress mode registers
	 */
	slice_per_intf = dsc->slice_count;

	total_bytes_per_intf = dsc->slice_chunk_size * slice_per_intf;
	bytes_per_pkt = dsc->slice_chunk_size; /* * slice_per_pkt; */

	eol_byte_num = total_bytes_per_intf % 3;

	/*
	 * Typically, pkt_per_line = slice_per_intf * slice_per_pkt.
	 *
	 * Since the current driver only supports slice_per_pkt = 1,
	 * pkt_per_line will be equal to slice per intf for now.
	 */
	pkt_per_line = slice_per_intf;

	if (is_cmd_mode) /* packet data type */
		reg = DSI_COMMAND_COMPRESSION_MODE_CTRL_STREAM0_DATATYPE(MIPI_DSI_DCS_LONG_WRITE);
	else
		reg = DSI_VIDEO_COMPRESSION_MODE_CTRL_DATATYPE(MIPI_DSI_COMPRESSED_PIXEL_STREAM);

	/* DSI_VIDEO_COMPRESSION_MODE & DSI_COMMAND_COMPRESSION_MODE
	 * registers have similar offsets, so for below common code use
	 * DSI_VIDEO_COMPRESSION_MODE_XXXX for setting bits
	 *
	 * pkt_per_line is log2 encoded, >>1 works for supported values (1,2,4)
	 */
	if (pkt_per_line > 4)
		drm_warn_once(msm_host->dev, "pkt_per_line too big");
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_PKT_PER_LINE(pkt_per_line >> 1);
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_EOL_BYTE_NUM(eol_byte_num);
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_EN;

	if (is_cmd_mode) {
		reg_ctrl = dsi_read(msm_host, REG_DSI_COMMAND_COMPRESSION_MODE_CTRL);
		reg_ctrl2 = dsi_read(msm_host, REG_DSI_COMMAND_COMPRESSION_MODE_CTRL2);

		reg_ctrl &= ~0xffff;
		reg_ctrl |= reg;

		reg_ctrl2 &= ~DSI_COMMAND_COMPRESSION_MODE_CTRL2_STREAM0_SLICE_WIDTH__MASK;
		reg_ctrl2 |= DSI_COMMAND_COMPRESSION_MODE_CTRL2_STREAM0_SLICE_WIDTH(dsc->slice_chunk_size);

		dsi_write(msm_host, REG_DSI_COMMAND_COMPRESSION_MODE_CTRL, reg_ctrl);
		dsi_write(msm_host, REG_DSI_COMMAND_COMPRESSION_MODE_CTRL2, reg_ctrl2);
	} else {
		reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_WC(bytes_per_pkt);
		dsi_write(msm_host, REG_DSI_VIDEO_COMPRESSION_MODE_CTRL, reg);
	}
}

static void dsi_timing_setup(struct msm_dsi_host *msm_host, bool is_bonded_dsi)
{
	struct drm_display_mode *mode = msm_host->mode;
	u32 hs_start = 0, vs_start = 0; /* take sync start as 0 */
	u32 h_total = mode->htotal;
	u32 v_total = mode->vtotal;
	u32 hs_end = mode->hsync_end - mode->hsync_start;
	u32 vs_end = mode->vsync_end - mode->vsync_start;
	u32 ha_start = h_total - mode->hsync_start;
	u32 ha_end = ha_start + mode->hdisplay;
	u32 va_start = v_total - mode->vsync_start;
	u32 va_end = va_start + mode->vdisplay;
	u32 hdisplay = mode->hdisplay;
	u32 wc;
	int ret;
	bool wide_bus_enabled = msm_dsi_host_is_wide_bus_enabled(&msm_host->base);

	DBG("");

	/*
	 * For bonded DSI mode, the current DRM mode has
	 * the complete width of the panel. Since, the complete
	 * panel is driven by two DSI controllers, the horizontal
	 * timings have to be split between the two dsi controllers.
	 * Adjust the DSI host timing values accordingly.
	 */
	if (is_bonded_dsi) {
		h_total /= 2;
		hs_end /= 2;
		ha_start /= 2;
		ha_end /= 2;
		hdisplay /= 2;
	}

	if (msm_host->dsc) {
		struct drm_dsc_config *dsc = msm_host->dsc;
		u32 bytes_per_pclk;

		/* update dsc params with timing params */
		if (!dsc || !mode->hdisplay || !mode->vdisplay) {
			pr_err("DSI: invalid input: pic_width: %d pic_height: %d\n",
			       mode->hdisplay, mode->vdisplay);
			return;
		}

		dsc->pic_width = mode->hdisplay;
		dsc->pic_height = mode->vdisplay;
		DBG("Mode %dx%d\n", dsc->pic_width, dsc->pic_height);

		/* we do the calculations for dsc parameters here so that
		 * panel can use these parameters
		 */
		ret = dsi_populate_dsc_params(msm_host, dsc);
		if (ret)
			return;

		/*
		 * DPU sends 3 bytes per pclk cycle to DSI. If widebus is
		 * enabled, bus width is extended to 6 bytes.
		 *
		 * Calculate the number of pclks needed to transmit one line of
		 * the compressed data.

		 * The back/font porch and pulse width are kept intact. For
		 * VIDEO mode they represent timing parameters rather than
		 * actual data transfer, see the documentation for
		 * dsi_adjust_pclk_for_compression(). For CMD mode they are
		 * unused anyway.
		 */
		h_total -= hdisplay;
		if (wide_bus_enabled && !(msm_host->mode_flags & MIPI_DSI_MODE_VIDEO))
			bytes_per_pclk = 6;
		else
			bytes_per_pclk = 3;

		hdisplay = DIV_ROUND_UP(msm_dsc_get_bytes_per_line(msm_host->dsc), bytes_per_pclk);

		h_total += hdisplay;
		ha_end = ha_start + hdisplay;
	}

	if (msm_host->mode_flags & MIPI_DSI_MODE_VIDEO) {
		if (msm_host->dsc)
			dsi_update_dsc_timing(msm_host, false);

		dsi_write(msm_host, REG_DSI_ACTIVE_H,
			DSI_ACTIVE_H_START(ha_start) |
			DSI_ACTIVE_H_END(ha_end));
		dsi_write(msm_host, REG_DSI_ACTIVE_V,
			DSI_ACTIVE_V_START(va_start) |
			DSI_ACTIVE_V_END(va_end));
		dsi_write(msm_host, REG_DSI_TOTAL,
			DSI_TOTAL_H_TOTAL(h_total - 1) |
			DSI_TOTAL_V_TOTAL(v_total - 1));

		dsi_write(msm_host, REG_DSI_ACTIVE_HSYNC,
			DSI_ACTIVE_HSYNC_START(hs_start) |
			DSI_ACTIVE_HSYNC_END(hs_end));
		dsi_write(msm_host, REG_DSI_ACTIVE_VSYNC_HPOS, 0);
		dsi_write(msm_host, REG_DSI_ACTIVE_VSYNC_VPOS,
			DSI_ACTIVE_VSYNC_VPOS_START(vs_start) |
			DSI_ACTIVE_VSYNC_VPOS_END(vs_end));
	} else {		/* command mode */
		if (msm_host->dsc)
			dsi_update_dsc_timing(msm_host, true);

		/* image data and 1 byte write_memory_start cmd */
		if (!msm_host->dsc)
			wc = hdisplay * mipi_dsi_pixel_format_to_bpp(msm_host->format) / 8 + 1;
		else
			/*
			 * When DSC is enabled, WC = slice_chunk_size * slice_per_pkt + 1.
			 * Currently, the driver only supports default value of slice_per_pkt = 1
			 *
			 * TODO: Expand mipi_dsi_device struct to hold slice_per_pkt info
			 *       and adjust DSC math to account for slice_per_pkt.
			 */
			wc = msm_host->dsc->slice_chunk_size + 1;

		dsi_write(msm_host, REG_DSI_CMD_MDP_STREAM0_CTRL,
			DSI_CMD_MDP_STREAM0_CTRL_WORD_COUNT(wc) |
			DSI_CMD_MDP_STREAM0_CTRL_VIRTUAL_CHANNEL(
					msm_host->channel) |
			DSI_CMD_MDP_STREAM0_CTRL_DATA_TYPE(
					MIPI_DSI_DCS_LONG_WRITE));

		dsi_write(msm_host, REG_DSI_CMD_MDP_STREAM0_TOTAL,
			DSI_CMD_MDP_STREAM0_TOTAL_H_TOTAL(hdisplay) |
			DSI_CMD_MDP_STREAM0_TOTAL_V_TOTAL(mode->vdisplay));
	}
}

static void dsi_sw_reset(struct msm_dsi_host *msm_host)
{
	u32 ctrl;

	ctrl = dsi_read(msm_host, REG_DSI_CTRL);

	if (ctrl & DSI_CTRL_ENABLE) {
		dsi_write(msm_host, REG_DSI_CTRL, ctrl & ~DSI_CTRL_ENABLE);
		/*
		 * dsi controller need to be disabled before
		 * clocks turned on
		 */
		wmb();
	}

	dsi_write(msm_host, REG_DSI_CLK_CTRL, DSI_CLK_CTRL_ENABLE_CLKS);
	wmb(); /* clocks need to be enabled before reset */

	/* dsi controller can only be reset while clocks are running */
	dsi_write(msm_host, REG_DSI_RESET, 1);
	msleep(DSI_RESET_TOGGLE_DELAY_MS); /* make sure reset happen */
	dsi_write(msm_host, REG_DSI_RESET, 0);
	wmb(); /* controller out of reset */

	if (ctrl & DSI_CTRL_ENABLE) {
		dsi_write(msm_host, REG_DSI_CTRL, ctrl);
		wmb();	/* make sure dsi controller enabled again */
	}
}

static void dsi_op_mode_config(struct msm_dsi_host *msm_host,
					bool video_mode, bool enable)
{
	u32 dsi_ctrl;

	dsi_ctrl = dsi_read(msm_host, REG_DSI_CTRL);

	if (!enable) {
		dsi_ctrl &= ~(DSI_CTRL_ENABLE | DSI_CTRL_VID_MODE_EN |
				DSI_CTRL_CMD_MODE_EN);
		dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_CMD_MDP_DONE |
					DSI_IRQ_MASK_VIDEO_DONE, 0);
	} else {
		if (video_mode) {
			dsi_ctrl |= DSI_CTRL_VID_MODE_EN;
		} else {		/* command mode */
			dsi_ctrl |= DSI_CTRL_CMD_MODE_EN;
			dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_CMD_MDP_DONE, 1);
		}
		dsi_ctrl |= DSI_CTRL_ENABLE;
	}

	dsi_write(msm_host, REG_DSI_CTRL, dsi_ctrl);
}

static void dsi_set_tx_power_mode(int mode, struct msm_dsi_host *msm_host)
{
	u32 data;

	data = dsi_read(msm_host, REG_DSI_CMD_DMA_CTRL);

	if (mode == 0)
		data &= ~DSI_CMD_DMA_CTRL_LOW_POWER;
	else
		data |= DSI_CMD_DMA_CTRL_LOW_POWER;

	dsi_write(msm_host, REG_DSI_CMD_DMA_CTRL, data);
}

static void dsi_wait4video_done(struct msm_dsi_host *msm_host)
{
	u32 ret = 0;
	struct device *dev = &msm_host->pdev->dev;

	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_VIDEO_DONE, 1);

	reinit_completion(&msm_host->video_comp);

	ret = wait_for_completion_timeout(&msm_host->video_comp,
			msecs_to_jiffies(70));

	if (ret == 0)
		DRM_DEV_ERROR(dev, "wait for video done timed out\n");

	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_VIDEO_DONE, 0);
}

static void dsi_wait4video_eng_busy(struct msm_dsi_host *msm_host)
{
	u32 data;

	if (!(msm_host->mode_flags & MIPI_DSI_MODE_VIDEO))
		return;

	data = dsi_read(msm_host, REG_DSI_STATUS0);

	/* if video mode engine is not busy, its because
	 * either timing engine was not turned on or the
	 * DSI controller has finished transmitting the video
	 * data already, so no need to wait in those cases
	 */
	if (!(data & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY))
		return;

	if (msm_host->power_on && msm_host->enabled) {
		dsi_wait4video_done(msm_host);
		/* delay 4 ms to skip BLLP */
		usleep_range(2000, 4000);
	}
}

int dsi_tx_buf_alloc_6g(struct msm_dsi_host *msm_host, int size)
{
	struct drm_device *dev = msm_host->dev;
	struct msm_drm_private *priv = dev->dev_private;
	uint64_t iova;
	u8 *data;

	msm_host->vm = drm_gpuvm_get(priv->kms->vm);

	data = msm_gem_kernel_new(dev, size, MSM_BO_WC,
					msm_host->vm,
					&msm_host->tx_gem_obj, &iova);

	if (IS_ERR(data)) {
		msm_host->tx_gem_obj = NULL;
		return PTR_ERR(data);
	}

	msm_gem_object_set_name(msm_host->tx_gem_obj, "tx_gem");

	msm_host->tx_size = msm_host->tx_gem_obj->size;

	return 0;
}

int dsi_tx_buf_alloc_v2(struct msm_dsi_host *msm_host, int size)
{
	struct drm_device *dev = msm_host->dev;

	msm_host->tx_buf = dma_alloc_coherent(dev->dev, size,
					&msm_host->tx_buf_paddr, GFP_KERNEL);
	if (!msm_host->tx_buf)
		return -ENOMEM;

	msm_host->tx_size = size;

	return 0;
}

void msm_dsi_tx_buf_free(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	struct drm_device *dev = msm_host->dev;

	/*
	 * This is possible if we're tearing down before we've had a chance to
	 * fully initialize. A very real possibility if our probe is deferred,
	 * in which case we'll hit msm_dsi_host_destroy() without having run
	 * through the dsi_tx_buf_alloc().
	 */
	if (!dev)
		return;

	if (msm_host->tx_gem_obj) {
		msm_gem_kernel_put(msm_host->tx_gem_obj, msm_host->vm);
		drm_gpuvm_put(msm_host->vm);
		msm_host->tx_gem_obj = NULL;
		msm_host->vm = NULL;
	}

	if (msm_host->tx_buf)
		dma_free_coherent(dev->dev, msm_host->tx_size, msm_host->tx_buf,
			msm_host->tx_buf_paddr);
}

void *dsi_tx_buf_get_6g(struct msm_dsi_host *msm_host)
{
	return msm_gem_get_vaddr(msm_host->tx_gem_obj);
}

void *dsi_tx_buf_get_v2(struct msm_dsi_host *msm_host)
{
	return msm_host->tx_buf;
}

void dsi_tx_buf_put_6g(struct msm_dsi_host *msm_host)
{
	msm_gem_put_vaddr(msm_host->tx_gem_obj);
}

/*
 * prepare cmd buffer to be txed
 */
static int dsi_cmd_dma_add(struct msm_dsi_host *msm_host,
			   const struct mipi_dsi_msg *msg)
{
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	struct mipi_dsi_packet packet;
	int len;
	int ret;
	u8 *data;

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret) {
		pr_err("%s: create packet failed, %d\n", __func__, ret);
		return ret;
	}
	len = (packet.size + 3) & (~0x3);

	if (len > msm_host->tx_size) {
		pr_err("%s: packet size is too big\n", __func__);
		return -EINVAL;
	}

	data = cfg_hnd->ops->tx_buf_get(msm_host);
	if (IS_ERR(data)) {
		ret = PTR_ERR(data);
		pr_err("%s: get vaddr failed, %d\n", __func__, ret);
		return ret;
	}

	/* MSM specific command format in memory */
	data[0] = packet.header[1];
	data[1] = packet.header[2];
	data[2] = packet.header[0];
	data[3] = BIT(7); /* Last packet */
	if (mipi_dsi_packet_format_is_long(msg->type))
		data[3] |= BIT(6);
	if (msg->rx_buf && msg->rx_len)
		data[3] |= BIT(5);

	/* Long packet */
	if (packet.payload && packet.payload_length)
		memcpy(data + 4, packet.payload, packet.payload_length);

	/* Append 0xff to the end */
	if (packet.size < len)
		memset(data + packet.size, 0xff, len - packet.size);

	if (cfg_hnd->ops->tx_buf_put)
		cfg_hnd->ops->tx_buf_put(msm_host);

	return len;
}

/*
 * dsi_short_read1_resp: 1 parameter
 */
static int dsi_short_read1_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	u8 *data = msg->rx_buf;

	if (data && (msg->rx_len >= 1)) {
		*data = buf[1]; /* strip out dcs type */
		return 1;
	}

	pr_err("%s: read data does not match with rx_buf len %zu\n",
		__func__, msg->rx_len);
	return -EINVAL;
}

/*
 * dsi_short_read2_resp: 2 parameter
 */
static int dsi_short_read2_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	u8 *data = msg->rx_buf;

	if (data && (msg->rx_len >= 2)) {
		data[0] = buf[1]; /* strip out dcs type */
		data[1] = buf[2];
		return 2;
	}

	pr_err("%s: read data does not match with rx_buf len %zu\n",
		__func__, msg->rx_len);
	return -EINVAL;
}

static int dsi_long_read_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	/* strip out 4 byte dcs header */
	if (msg->rx_buf && msg->rx_len)
		memcpy(msg->rx_buf, buf + 4, msg->rx_len);

	return msg->rx_len;
}

int dsi_dma_base_get_6g(struct msm_dsi_host *msm_host, uint64_t *dma_base)
{
	struct drm_device *dev = msm_host->dev;
	struct msm_drm_private *priv = dev->dev_private;

	if (!dma_base)
		return -EINVAL;

	return msm_gem_get_and_pin_iova(msm_host->tx_gem_obj,
				priv->kms->vm, dma_base);
}

int dsi_dma_base_get_v2(struct msm_dsi_host *msm_host, uint64_t *dma_base)
{
	if (!dma_base)
		return -EINVAL;

	*dma_base = msm_host->tx_buf_paddr;
	return 0;
}

static int dsi_cmd_dma_tx(struct msm_dsi_host *msm_host, int len)
{
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int ret;
	uint64_t dma_base;
	bool triggered;

	ret = cfg_hnd->ops->dma_base_get(msm_host, &dma_base);
	if (ret) {
		pr_err("%s: failed to get iova: %d\n", __func__, ret);
		return ret;
	}

	reinit_completion(&msm_host->dma_comp);

	dsi_wait4video_eng_busy(msm_host);

	triggered = msm_dsi_manager_cmd_xfer_trigger(
						msm_host->id, dma_base, len);
	if (triggered) {
		ret = wait_for_completion_timeout(&msm_host->dma_comp,
					msecs_to_jiffies(200));
		DBG("ret=%d", ret);
		if (ret == 0)
			ret = -ETIMEDOUT;
		else
			ret = len;
	} else {
		ret = len;
	}

	return ret;
}

static int dsi_cmd_dma_rx(struct msm_dsi_host *msm_host,
			u8 *buf, int rx_byte, int pkt_size)
{
	u32 *temp, data;
	int i, j = 0, cnt;
	u32 read_cnt;
	u8 reg[16];
	int repeated_bytes = 0;
	int buf_offset = buf - msm_host->rx_buf;

	temp = (u32 *)reg;
	cnt = (rx_byte + 3) >> 2;
	if (cnt > 4)
		cnt = 4; /* 4 x 32 bits registers only */

	if (rx_byte == 4)
		read_cnt = 4;
	else
		read_cnt = pkt_size + 6;

	/*
	 * In case of multiple reads from the panel, after the first read, there
	 * is possibility that there are some bytes in the payload repeating in
	 * the RDBK_DATA registers. Since we read all the parameters from the
	 * panel right from the first byte for every pass. We need to skip the
	 * repeating bytes and then append the new parameters to the rx buffer.
	 */
	if (read_cnt > 16) {
		int bytes_shifted;
		/* Any data more than 16 bytes will be shifted out.
		 * The temp read buffer should already contain these bytes.
		 * The remaining bytes in read buffer are the repeated bytes.
		 */
		bytes_shifted = read_cnt - 16;
		repeated_bytes = buf_offset - bytes_shifted;
	}

	for (i = cnt - 1; i >= 0; i--) {
		data = dsi_read(msm_host, REG_DSI_RDBK_DATA(i));
		*temp++ = ntohl(data); /* to host byte order */
		DBG("data = 0x%x and ntohl(data) = 0x%x", data, ntohl(data));
	}

	for (i = repeated_bytes; i < 16; i++)
		buf[j++] = reg[i];

	return j;
}

static int dsi_cmds2buf_tx(struct msm_dsi_host *msm_host,
				const struct mipi_dsi_msg *msg)
{
	int len, ret;
	int bllp_len = msm_host->mode->hdisplay *
			mipi_dsi_pixel_format_to_bpp(msm_host->format) / 8;

	len = dsi_cmd_dma_add(msm_host, msg);
	if (len < 0) {
		pr_err("%s: failed to add cmd type = 0x%x\n",
			__func__,  msg->type);
		return len;
	}

	/*
	 * for video mode, do not send cmds more than
	 * one pixel line, since it only transmit it
	 * during BLLP.
	 *
	 * TODO: if the command is sent in LP mode, the bit rate is only
	 * half of esc clk rate. In this case, if the video is already
	 * actively streaming, we need to check more carefully if the
	 * command can be fit into one BLLP.
	 */
	if ((msm_host->mode_flags & MIPI_DSI_MODE_VIDEO) && (len > bllp_len)) {
		pr_err("%s: cmd cannot fit into BLLP period, len=%d\n",
			__func__, len);
		return -EINVAL;
	}

	ret = dsi_cmd_dma_tx(msm_host, len);
	if (ret < 0) {
		pr_err("%s: cmd dma tx failed, type=0x%x, data0=0x%x, len=%d, ret=%d\n",
			__func__, msg->type, (*(u8 *)(msg->tx_buf)), len, ret);
		return ret;
	} else if (ret < len) {
		pr_err("%s: cmd dma tx failed, type=0x%x, data0=0x%x, ret=%d len=%d\n",
			__func__, msg->type, (*(u8 *)(msg->tx_buf)), ret, len);
		return -EIO;
	}

	return len;
}

static void dsi_err_worker(struct work_struct *work)
{
	struct msm_dsi_host *msm_host =
		container_of(work, struct msm_dsi_host, err_work);
	u32 status = msm_host->err_work_state;

	pr_err_ratelimited("%s: status=%x\n", __func__, status);
	if (status & DSI_ERR_STATE_MDP_FIFO_UNDERFLOW)
		dsi_sw_reset(msm_host);

	/* It is safe to clear here because error irq is disabled. */
	msm_host->err_work_state = 0;

	/* enable dsi error interrupt */
	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_ERROR, 1);
}

static void dsi_ack_err_status(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_ACK_ERR_STATUS);

	if (status) {
		dsi_write(msm_host, REG_DSI_ACK_ERR_STATUS, status);
		/* Writing of an extra 0 needed to clear error bits */
		dsi_write(msm_host, REG_DSI_ACK_ERR_STATUS, 0);
		msm_host->err_work_state |= DSI_ERR_STATE_ACK;
	}
}

static void dsi_timeout_status(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_TIMEOUT_STATUS);

	if (status) {
		dsi_write(msm_host, REG_DSI_TIMEOUT_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_TIMEOUT;
	}
}

static void dsi_dln0_phy_err(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_DLN0_PHY_ERR);

	if (status & (DSI_DLN0_PHY_ERR_DLN0_ERR_ESC |
			DSI_DLN0_PHY_ERR_DLN0_ERR_SYNC_ESC |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTROL |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTENTION_LP0 |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTENTION_LP1)) {
		dsi_write(msm_host, REG_DSI_DLN0_PHY_ERR, status);
		msm_host->err_work_state |= DSI_ERR_STATE_DLN0_PHY;
	}
}

static void dsi_fifo_status(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_FIFO_STATUS);

	/* fifo underflow, overflow */
	if (status) {
		dsi_write(msm_host, REG_DSI_FIFO_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_FIFO;
		if (status & DSI_FIFO_STATUS_CMD_MDP_FIFO_UNDERFLOW)
			msm_host->err_work_state |=
					DSI_ERR_STATE_MDP_FIFO_UNDERFLOW;
	}
}

static void dsi_status(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_STATUS0);

	if (status & DSI_STATUS0_INTERLEAVE_OP_CONTENTION) {
		dsi_write(msm_host, REG_DSI_STATUS0, status);
		msm_host->err_work_state |=
			DSI_ERR_STATE_INTERLEAVE_OP_CONTENTION;
	}
}

static void dsi_clk_status(struct msm_dsi_host *msm_host)
{
	u32 status;

	status = dsi_read(msm_host, REG_DSI_CLK_STATUS);

	if (status & DSI_CLK_STATUS_PLL_UNLOCKED) {
		dsi_write(msm_host, REG_DSI_CLK_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_PLL_UNLOCKED;
	}
}

static void dsi_error(struct msm_dsi_host *msm_host)
{
	/* disable dsi error interrupt */
	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_ERROR, 0);

	dsi_clk_status(msm_host);
	dsi_fifo_status(msm_host);
	dsi_ack_err_status(msm_host);
	dsi_timeout_status(msm_host);
	dsi_status(msm_host);
	dsi_dln0_phy_err(msm_host);

	queue_work(msm_host->workqueue, &msm_host->err_work);
}

static irqreturn_t dsi_host_irq(int irq, void *ptr)
{
	struct msm_dsi_host *msm_host = ptr;
	u32 isr;
	unsigned long flags;

	if (!msm_host->ctrl_base)
		return IRQ_HANDLED;

	spin_lock_irqsave(&msm_host->intr_lock, flags);
	isr = dsi_read(msm_host, REG_DSI_INTR_CTRL);
	dsi_write(msm_host, REG_DSI_INTR_CTRL, isr);
	spin_unlock_irqrestore(&msm_host->intr_lock, flags);

	DBG("isr=0x%x, id=%d", isr, msm_host->id);

	if (isr & DSI_IRQ_ERROR)
		dsi_error(msm_host);

	if (isr & DSI_IRQ_VIDEO_DONE)
		complete(&msm_host->video_comp);

	if (isr & DSI_IRQ_CMD_DMA_DONE)
		complete(&msm_host->dma_comp);

	return IRQ_HANDLED;
}

static int dsi_host_attach(struct mipi_dsi_host *host,
					struct mipi_dsi_device *dsi)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	int ret;

	if (dsi->lanes > msm_host->num_data_lanes)
		return -EINVAL;

	msm_host->channel = dsi->channel;
	msm_host->lanes = dsi->lanes;
	msm_host->format = dsi->format;
	msm_host->mode_flags = dsi->mode_flags;
	if (dsi->dsc)
		msm_host->dsc = dsi->dsc;

	ret = dsi_dev_attach(msm_host->pdev);
	if (ret)
		return ret;

	DBG("id=%d", msm_host->id);

	return 0;
}

static int dsi_host_detach(struct mipi_dsi_host *host,
					struct mipi_dsi_device *dsi)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	dsi_dev_detach(msm_host->pdev);

	DBG("id=%d", msm_host->id);

	return 0;
}

static ssize_t dsi_host_transfer(struct mipi_dsi_host *host,
					const struct mipi_dsi_msg *msg)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	int ret;

	if (!msg || !msm_host->power_on)
		return -EINVAL;

	mutex_lock(&msm_host->cmd_mutex);
	ret = msm_dsi_manager_cmd_xfer(msm_host->id, msg);
	mutex_unlock(&msm_host->cmd_mutex);

	return ret;
}

static const struct mipi_dsi_host_ops dsi_host_ops = {
	.attach = dsi_host_attach,
	.detach = dsi_host_detach,
	.transfer = dsi_host_transfer,
};

/*
 * List of supported physical to logical lane mappings.
 * For example, the 2nd entry represents the following mapping:
 *
 * "3012": Logic 3->Phys 0; Logic 0->Phys 1; Logic 1->Phys 2; Logic 2->Phys 3;
 */
static const int supported_data_lane_swaps[][4] = {
	{ 0, 1, 2, 3 },
	{ 3, 0, 1, 2 },
	{ 2, 3, 0, 1 },
	{ 1, 2, 3, 0 },
	{ 0, 3, 2, 1 },
	{ 1, 0, 3, 2 },
	{ 2, 1, 0, 3 },
	{ 3, 2, 1, 0 },
};

static int dsi_host_parse_lane_data(struct msm_dsi_host *msm_host,
				    struct device_node *ep)
{
	struct device *dev = &msm_host->pdev->dev;
	struct property *prop;
	u32 lane_map[4];
	int ret, i, len, num_lanes;

	prop = of_find_property(ep, "data-lanes", &len);
	if (!prop) {
		DRM_DEV_DEBUG(dev,
			"failed to find data lane mapping, using default\n");
		/* Set the number of date lanes to 4 by default. */
		msm_host->num_data_lanes = 4;
		return 0;
	}

	num_lanes = drm_of_get_data_lanes_count(ep, 1, 4);
	if (num_lanes < 0) {
		DRM_DEV_ERROR(dev, "bad number of data lanes\n");
		return num_lanes;
	}

	msm_host->num_data_lanes = num_lanes;

	ret = of_property_read_u32_array(ep, "data-lanes", lane_map,
					 num_lanes);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to read lane data\n");
		return ret;
	}

	/*
	 * compare DT specified physical-logical lane mappings with the ones
	 * supported by hardware
	 */
	for (i = 0; i < ARRAY_SIZE(supported_data_lane_swaps); i++) {
		const int *swap = supported_data_lane_swaps[i];
		int j;

		/*
		 * the data-lanes array we get from DT has a logical->physical
		 * mapping. The "data lane swap" register field represents
		 * supported configurations in a physical->logical mapping.
		 * Translate the DT mapping to what we understand and find a
		 * configuration that works.
		 */
		for (j = 0; j < num_lanes; j++) {
			if (lane_map[j] < 0 || lane_map[j] > 3)
				DRM_DEV_ERROR(dev, "bad physical lane entry %u\n",
					lane_map[j]);

			if (swap[lane_map[j]] != j)
				break;
		}

		if (j == num_lanes) {
			msm_host->dlane_swap = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int dsi_populate_dsc_params(struct msm_dsi_host *msm_host, struct drm_dsc_config *dsc)
{
	int ret;

	if (dsc->bits_per_pixel & 0xf) {
		DRM_DEV_ERROR(&msm_host->pdev->dev, "DSI does not support fractional bits_per_pixel\n");
		return -EINVAL;
	}

	switch (dsc->bits_per_component) {
	case 8:
	case 10:
	case 12:
		/*
		 * Only 8, 10, and 12 bpc are supported for DSC 1.1 block.
		 * If additional bpc values need to be supported, update
		 * this quard with the appropriate DSC version verification.
		 */
		break;
	default:
		DRM_DEV_ERROR(&msm_host->pdev->dev,
			      "Unsupported bits_per_component value: %d\n",
			      dsc->bits_per_component);
		return -EOPNOTSUPP;
	}

	dsc->simple_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);

	/* DPU supports only pre-SCR panels */
	ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
	if (ret) {
		DRM_DEV_ERROR(&msm_host->pdev->dev, "could not find DSC RC parameters\n");
		return ret;
	}

	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
	dsc->line_buf_depth = dsc->bits_per_component + 1;

	return drm_dsc_compute_rc_parameters(dsc);
}

static int dsi_host_parse_dt(struct msm_dsi_host *msm_host)
{
	struct msm_dsi *msm_dsi = platform_get_drvdata(msm_host->pdev);
	struct device *dev = &msm_host->pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *endpoint;
	const char *te_source;
	int ret = 0;

	/*
	 * Get the endpoint of the output port of the DSI host. In our case,
	 * this is mapped to port number with reg = 1. Don't return an error if
	 * the remote endpoint isn't defined. It's possible that there is
	 * nothing connected to the dsi output.
	 */
	endpoint = of_graph_get_endpoint_by_regs(np, 1, -1);
	if (!endpoint) {
		DRM_DEV_DEBUG(dev, "%s: no endpoint\n", __func__);
		return 0;
	}

	ret = dsi_host_parse_lane_data(msm_host, endpoint);
	if (ret) {
		DRM_DEV_ERROR(dev, "%s: invalid lane configuration %d\n",
			__func__, ret);
		ret = -EINVAL;
		goto err;
	}

	ret = of_property_read_string(endpoint, "qcom,te-source", &te_source);
	if (ret && ret != -EINVAL) {
		DRM_DEV_ERROR(dev, "%s: invalid TE source configuration %d\n",
			__func__, ret);
		goto err;
	}
	if (!ret) {
		msm_dsi->te_source = devm_kstrdup(dev, te_source, GFP_KERNEL);
		if (!msm_dsi->te_source) {
			DRM_DEV_ERROR(dev, "%s: failed to allocate te_source\n",
				__func__);
			ret = -ENOMEM;
			goto err;
		}
	}
	ret = 0;

	if (of_property_present(np, "syscon-sfpb")) {
		msm_host->sfpb = syscon_regmap_lookup_by_phandle(np,
					"syscon-sfpb");
		if (IS_ERR(msm_host->sfpb)) {
			DRM_DEV_ERROR(dev, "%s: failed to get sfpb regmap\n",
				__func__);
			ret = PTR_ERR(msm_host->sfpb);
		}
	}

err:
	of_node_put(endpoint);

	return ret;
}

static int dsi_host_get_id(struct msm_dsi_host *msm_host)
{
	struct platform_device *pdev = msm_host->pdev;
	const struct msm_dsi_config *cfg = msm_host->cfg_hnd->cfg;
	struct resource *res;
	int i, j;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dsi_ctrl");
	if (!res)
		return -EINVAL;

	for (i = 0; i < VARIANTS_MAX; i++)
		for (j = 0; j < DSI_MAX; j++)
			if (cfg->io_start[i][j] == res->start)
				return j;

	return -EINVAL;
}

int msm_dsi_host_init(struct msm_dsi *msm_dsi)
{
	struct msm_dsi_host *msm_host = NULL;
	struct platform_device *pdev = msm_dsi->pdev;
	const struct msm_dsi_config *cfg;
	int ret;

	msm_host = devm_kzalloc(&pdev->dev, sizeof(*msm_host), GFP_KERNEL);
	if (!msm_host)
		return -ENOMEM;

	msm_host->pdev = pdev;
	msm_dsi->host = &msm_host->base;

	ret = dsi_host_parse_dt(msm_host);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "%s: failed to parse dt\n",
				     __func__);

	msm_host->ctrl_base = msm_ioremap_size(pdev, "dsi_ctrl", &msm_host->ctrl_size);
	if (IS_ERR(msm_host->ctrl_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(msm_host->ctrl_base),
				     "%s: unable to map Dsi ctrl base\n", __func__);

	pm_runtime_enable(&pdev->dev);

	msm_host->cfg_hnd = dsi_get_config(msm_host);
	if (!msm_host->cfg_hnd)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "%s: get config failed\n", __func__);
	cfg = msm_host->cfg_hnd->cfg;

	msm_host->id = dsi_host_get_id(msm_host);
	if (msm_host->id < 0)
		return dev_err_probe(&pdev->dev, msm_host->id,
				     "%s: unable to identify DSI host index\n",
				     __func__);

	/* fixup base address by io offset */
	msm_host->ctrl_base += cfg->io_offset;

	ret = devm_regulator_bulk_get_const(&pdev->dev, cfg->num_regulators,
					    cfg->regulator_data,
					    &msm_host->supplies);
	if (ret)
		return ret;

	ret = dsi_clk_init(msm_host);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "%s: unable to initialize dsi clks\n", __func__);

	msm_host->rx_buf = devm_kzalloc(&pdev->dev, SZ_4K, GFP_KERNEL);
	if (!msm_host->rx_buf)
		return -ENOMEM;

	ret = devm_pm_opp_set_clkname(&pdev->dev, "byte");
	if (ret)
		return ret;
	/* OPP table is optional */
	ret = devm_pm_opp_of_add_table(&pdev->dev);
	if (ret && ret != -ENODEV)
		return dev_err_probe(&pdev->dev, ret, "invalid OPP table in device tree\n");

	msm_host->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!msm_host->irq)
		return dev_err_probe(&pdev->dev, -EINVAL, "failed to get irq\n");

	/* do not autoenable, will be enabled later */
	ret = devm_request_irq(&pdev->dev, msm_host->irq, dsi_host_irq,
			IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN,
			"dsi_isr", msm_host);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to request IRQ%u\n",
				     msm_host->irq);

	init_completion(&msm_host->dma_comp);
	init_completion(&msm_host->video_comp);
	mutex_init(&msm_host->dev_mutex);
	mutex_init(&msm_host->cmd_mutex);
	spin_lock_init(&msm_host->intr_lock);

	/* setup workqueue */
	msm_host->workqueue = alloc_ordered_workqueue("dsi_drm_work", 0);
	if (!msm_host->workqueue)
		return -ENOMEM;

	INIT_WORK(&msm_host->err_work, dsi_err_worker);

	msm_dsi->id = msm_host->id;

	DBG("Dsi Host %d initialized", msm_host->id);
	return 0;
}

void msm_dsi_host_destroy(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	DBG("");
	if (msm_host->workqueue) {
		destroy_workqueue(msm_host->workqueue);
		msm_host->workqueue = NULL;
	}

	mutex_destroy(&msm_host->cmd_mutex);
	mutex_destroy(&msm_host->dev_mutex);

	pm_runtime_disable(&msm_host->pdev->dev);
}

int msm_dsi_host_modeset_init(struct mipi_dsi_host *host,
					struct drm_device *dev)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int ret;

	msm_host->dev = dev;

	ret = cfg_hnd->ops->tx_buf_alloc(msm_host, SZ_4K);
	if (ret) {
		pr_err("%s: alloc tx gem obj failed, %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

int msm_dsi_host_register(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	int ret;

	/* Register mipi dsi host */
	if (!msm_host->registered) {
		host->dev = &msm_host->pdev->dev;
		host->ops = &dsi_host_ops;
		ret = mipi_dsi_host_register(host);
		if (ret)
			return ret;

		msm_host->registered = true;
	}

	return 0;
}

void msm_dsi_host_unregister(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	if (msm_host->registered) {
		mipi_dsi_host_unregister(host);
		host->dev = NULL;
		host->ops = NULL;
		msm_host->registered = false;
	}
}

int msm_dsi_host_xfer_prepare(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;

	/* TODO: make sure dsi_cmd_mdp is idle.
	 * Since DSI6G v1.2.0, we can set DSI_TRIG_CTRL.BLOCK_DMA_WITHIN_FRAME
	 * to ask H/W to wait until cmd mdp is idle. S/W wait is not needed.
	 * How to handle the old versions? Wait for mdp cmd done?
	 */

	/*
	 * mdss interrupt is generated in mdp core clock domain
	 * mdp clock need to be enabled to receive dsi interrupt
	 */
	pm_runtime_get_sync(&msm_host->pdev->dev);
	cfg_hnd->ops->link_clk_set_rate(msm_host);
	cfg_hnd->ops->link_clk_enable(msm_host);

	/* TODO: vote for bus bandwidth */

	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM))
		dsi_set_tx_power_mode(0, msm_host);

	msm_host->dma_cmd_ctrl_restore = dsi_read(msm_host, REG_DSI_CTRL);
	dsi_write(msm_host, REG_DSI_CTRL,
		msm_host->dma_cmd_ctrl_restore |
		DSI_CTRL_CMD_MODE_EN |
		DSI_CTRL_ENABLE);
	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_CMD_DMA_DONE, 1);

	return 0;
}

void msm_dsi_host_xfer_restore(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;

	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_CMD_DMA_DONE, 0);
	dsi_write(msm_host, REG_DSI_CTRL, msm_host->dma_cmd_ctrl_restore);

	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM))
		dsi_set_tx_power_mode(1, msm_host);

	/* TODO: unvote for bus bandwidth */

	cfg_hnd->ops->link_clk_disable(msm_host);
	pm_runtime_put(&msm_host->pdev->dev);
}

int msm_dsi_host_cmd_tx(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	return dsi_cmds2buf_tx(msm_host, msg);
}

int msm_dsi_host_cmd_rx(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int data_byte, rx_byte, dlen, end;
	int short_response, diff, pkt_size, ret = 0;
	char cmd;
	int rlen = msg->rx_len;
	u8 *buf;

	if (rlen <= 2) {
		short_response = 1;
		pkt_size = rlen;
		rx_byte = 4;
	} else {
		short_response = 0;
		data_byte = 10;	/* first read */
		if (rlen < data_byte)
			pkt_size = rlen;
		else
			pkt_size = data_byte;
		rx_byte = data_byte + 6; /* 4 header + 2 crc */
	}

	buf = msm_host->rx_buf;
	end = 0;
	while (!end) {
		u8 tx[2] = {pkt_size & 0xff, pkt_size >> 8};
		struct mipi_dsi_msg max_pkt_size_msg = {
			.channel = msg->channel,
			.type = MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE,
			.tx_len = 2,
			.tx_buf = tx,
		};

		DBG("rlen=%d pkt_size=%d rx_byte=%d",
			rlen, pkt_size, rx_byte);

		ret = dsi_cmds2buf_tx(msm_host, &max_pkt_size_msg);
		if (ret < 2) {
			pr_err("%s: Set max pkt size failed, %d\n",
				__func__, ret);
			return -EINVAL;
		}

		if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
			(cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_1)) {
			/* Clear the RDBK_DATA registers */
			dsi_write(msm_host, REG_DSI_RDBK_DATA_CTRL,
					DSI_RDBK_DATA_CTRL_CLR);
			wmb(); /* make sure the RDBK registers are cleared */
			dsi_write(msm_host, REG_DSI_RDBK_DATA_CTRL, 0);
			wmb(); /* release cleared status before transfer */
		}

		ret = dsi_cmds2buf_tx(msm_host, msg);
		if (ret < 0) {
			pr_err("%s: Read cmd Tx failed, %d\n", __func__, ret);
			return ret;
		} else if (ret < msg->tx_len) {
			pr_err("%s: Read cmd Tx failed, too short: %d\n", __func__, ret);
			return -ECOMM;
		}

		/*
		 * once cmd_dma_done interrupt received,
		 * return data from client is ready and stored
		 * at RDBK_DATA register already
		 * since rx fifo is 16 bytes, dcs header is kept at first loop,
		 * after that dcs header lost during shift into registers
		 */
		dlen = dsi_cmd_dma_rx(msm_host, buf, rx_byte, pkt_size);

		if (dlen <= 0)
			return 0;

		if (short_response)
			break;

		if (rlen <= data_byte) {
			diff = data_byte - rlen;
			end = 1;
		} else {
			diff = 0;
			rlen -= data_byte;
		}

		if (!end) {
			dlen -= 2; /* 2 crc */
			dlen -= diff;
			buf += dlen;	/* next start position */
			data_byte = 14;	/* NOT first read */
			if (rlen < data_byte)
				pkt_size += rlen;
			else
				pkt_size += data_byte;
			DBG("buf=%p dlen=%d diff=%d", buf, dlen, diff);
		}
	}

	/*
	 * For single Long read, if the requested rlen < 10,
	 * we need to shift the start position of rx
	 * data buffer to skip the bytes which are not
	 * updated.
	 */
	if (pkt_size < 10 && !short_response)
		buf = msm_host->rx_buf + (10 - rlen);
	else
		buf = msm_host->rx_buf;

	cmd = buf[0];
	switch (cmd) {
	case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
		pr_err("%s: rx ACK_ERR_PACLAGE\n", __func__);
		ret = 0;
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		ret = dsi_short_read1_resp(buf, msg);
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		ret = dsi_short_read2_resp(buf, msg);
		break;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		ret = dsi_long_read_resp(buf, msg);
		break;
	default:
		pr_warn("%s:Invalid response cmd\n", __func__);
		ret = 0;
	}

	return ret;
}

void msm_dsi_host_cmd_xfer_commit(struct mipi_dsi_host *host, u32 dma_base,
				  u32 len)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	dsi_write(msm_host, REG_DSI_DMA_BASE, dma_base);
	dsi_write(msm_host, REG_DSI_DMA_LEN, len);
	dsi_write(msm_host, REG_DSI_TRIG_DMA, 1);

	/* Make sure trigger happens */
	wmb();
}

void msm_dsi_host_set_phy_mode(struct mipi_dsi_host *host,
	struct msm_dsi_phy *src_phy)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	msm_host->cphy_mode = src_phy->cphy_mode;
}

void msm_dsi_host_reset_phy(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	DBG("");
	dsi_write(msm_host, REG_DSI_PHY_RESET, DSI_PHY_RESET_RESET);
	/* Make sure fully reset */
	wmb();
	udelay(1000);
	dsi_write(msm_host, REG_DSI_PHY_RESET, 0);
	udelay(100);
}

void msm_dsi_host_get_phy_clk_req(struct mipi_dsi_host *host,
			struct msm_dsi_phy_clk_request *clk_req,
			bool is_bonded_dsi)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int ret;

	ret = cfg_hnd->ops->calc_clk_rate(msm_host, is_bonded_dsi);
	if (ret) {
		pr_err("%s: unable to calc clk rate, %d\n", __func__, ret);
		return;
	}

	/* CPHY transmits 16 bits over 7 clock cycles
	 * "byte_clk" is in units of 16-bits (see dsi_calc_pclk),
	 * so multiply by 7 to get the "bitclk rate"
	 */
	if (msm_host->cphy_mode)
		clk_req->bitclk_rate = msm_host->byte_clk_rate * 7;
	else
		clk_req->bitclk_rate = msm_host->byte_clk_rate * 8;
	clk_req->escclk_rate = msm_host->esc_clk_rate;
}

void msm_dsi_host_enable_irq(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	enable_irq(msm_host->irq);
}

void msm_dsi_host_disable_irq(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	disable_irq(msm_host->irq);
}

int msm_dsi_host_enable(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	dsi_op_mode_config(msm_host,
		!!(msm_host->mode_flags & MIPI_DSI_MODE_VIDEO), true);

	/* TODO: clock should be turned off for command mode,
	 * and only turned on before MDP START.
	 * This part of code should be enabled once mdp driver support it.
	 */
	/* if (msm_panel->mode == MSM_DSI_CMD_MODE) {
	 *	dsi_link_clk_disable(msm_host);
	 *	pm_runtime_put(&msm_host->pdev->dev);
	 * }
	 */
	msm_host->enabled = true;
	return 0;
}

int msm_dsi_host_disable(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	msm_host->enabled = false;
	dsi_op_mode_config(msm_host,
		!!(msm_host->mode_flags & MIPI_DSI_MODE_VIDEO), false);

	/* Since we have disabled INTF, the video engine won't stop so that
	 * the cmd engine will be blocked.
	 * Reset to disable video engine so that we can send off cmd.
	 */
	dsi_sw_reset(msm_host);

	return 0;
}

static void msm_dsi_sfpb_config(struct msm_dsi_host *msm_host, bool enable)
{
	enum sfpb_ahb_arb_master_port_en en;

	if (!msm_host->sfpb)
		return;

	en = enable ? SFPB_MASTER_PORT_ENABLE : SFPB_MASTER_PORT_DISABLE;

	regmap_update_bits(msm_host->sfpb, REG_SFPB_GPREG,
			SFPB_GPREG_MASTER_PORT_EN__MASK,
			SFPB_GPREG_MASTER_PORT_EN(en));
}

int msm_dsi_host_power_on(struct mipi_dsi_host *host,
			struct msm_dsi_phy_shared_timings *phy_shared_timings,
			bool is_bonded_dsi, struct msm_dsi_phy *phy)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int ret = 0;

	mutex_lock(&msm_host->dev_mutex);
	if (msm_host->power_on) {
		DBG("dsi host already on");
		goto unlock_ret;
	}

	msm_host->byte_intf_clk_rate = msm_host->byte_clk_rate;
	if (phy_shared_timings->byte_intf_clk_div_2)
		msm_host->byte_intf_clk_rate /= 2;

	msm_dsi_sfpb_config(msm_host, true);

	ret = regulator_bulk_enable(msm_host->cfg_hnd->cfg->num_regulators,
				    msm_host->supplies);
	if (ret) {
		pr_err("%s:Failed to enable vregs.ret=%d\n",
			__func__, ret);
		goto unlock_ret;
	}

	pm_runtime_get_sync(&msm_host->pdev->dev);
	ret = cfg_hnd->ops->link_clk_set_rate(msm_host);
	if (!ret)
		ret = cfg_hnd->ops->link_clk_enable(msm_host);
	if (ret) {
		pr_err("%s: failed to enable link clocks. ret=%d\n",
		       __func__, ret);
		goto fail_disable_reg;
	}

	ret = pinctrl_pm_select_default_state(&msm_host->pdev->dev);
	if (ret) {
		pr_err("%s: failed to set pinctrl default state, %d\n",
			__func__, ret);
		goto fail_disable_clk;
	}

	dsi_timing_setup(msm_host, is_bonded_dsi);
	dsi_sw_reset(msm_host);
	dsi_ctrl_enable(msm_host, phy_shared_timings, phy);

	msm_host->power_on = true;
	mutex_unlock(&msm_host->dev_mutex);

	return 0;

fail_disable_clk:
	cfg_hnd->ops->link_clk_disable(msm_host);
	pm_runtime_put(&msm_host->pdev->dev);
fail_disable_reg:
	regulator_bulk_disable(msm_host->cfg_hnd->cfg->num_regulators,
			       msm_host->supplies);
unlock_ret:
	mutex_unlock(&msm_host->dev_mutex);
	return ret;
}

int msm_dsi_host_power_off(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;

	mutex_lock(&msm_host->dev_mutex);
	if (!msm_host->power_on) {
		DBG("dsi host already off");
		goto unlock_ret;
	}

	dsi_ctrl_disable(msm_host);

	pinctrl_pm_select_sleep_state(&msm_host->pdev->dev);

	cfg_hnd->ops->link_clk_disable(msm_host);
	pm_runtime_put(&msm_host->pdev->dev);

	regulator_bulk_disable(msm_host->cfg_hnd->cfg->num_regulators,
			       msm_host->supplies);

	msm_dsi_sfpb_config(msm_host, false);

	DBG("-");

	msm_host->power_on = false;

unlock_ret:
	mutex_unlock(&msm_host->dev_mutex);
	return 0;
}

int msm_dsi_host_set_display_mode(struct mipi_dsi_host *host,
				  const struct drm_display_mode *mode)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	if (msm_host->mode) {
		drm_mode_destroy(msm_host->dev, msm_host->mode);
		msm_host->mode = NULL;
	}

	msm_host->mode = drm_mode_duplicate(msm_host->dev, mode);
	if (!msm_host->mode) {
		pr_err("%s: cannot duplicate mode\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

enum drm_mode_status msm_dsi_host_check_dsc(struct mipi_dsi_host *host,
					    const struct drm_display_mode *mode)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	struct drm_dsc_config *dsc = msm_host->dsc;
	int pic_width = mode->hdisplay;
	int pic_height = mode->vdisplay;

	if (!msm_host->dsc)
		return MODE_OK;

	if (pic_width % dsc->slice_width) {
		pr_err("DSI: pic_width %d has to be multiple of slice %d\n",
		       pic_width, dsc->slice_width);
		return MODE_H_ILLEGAL;
	}

	if (pic_height % dsc->slice_height) {
		pr_err("DSI: pic_height %d has to be multiple of slice %d\n",
		       pic_height, dsc->slice_height);
		return MODE_V_ILLEGAL;
	}

	return MODE_OK;
}

unsigned long msm_dsi_host_get_mode_flags(struct mipi_dsi_host *host)
{
	return to_msm_dsi_host(host)->mode_flags;
}

void msm_dsi_host_snapshot(struct msm_disp_state *disp_state, struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	pm_runtime_get_sync(&msm_host->pdev->dev);

	msm_disp_snapshot_add_block(disp_state, msm_host->ctrl_size,
			msm_host->ctrl_base, "dsi%d_ctrl", msm_host->id);

	pm_runtime_put_sync(&msm_host->pdev->dev);
}

static void msm_dsi_host_video_test_pattern_setup(struct msm_dsi_host *msm_host)
{
	u32 reg;

	reg = dsi_read(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL);

	dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_VIDEO_INIT_VAL, 0xff);
	/* draw checkered rectangle pattern */
	dsi_write(msm_host, REG_DSI_TPG_MAIN_CONTROL,
			DSI_TPG_MAIN_CONTROL_CHECKERED_RECTANGLE_PATTERN);
	/* use 24-bit RGB test pttern */
	dsi_write(msm_host, REG_DSI_TPG_VIDEO_CONFIG,
			DSI_TPG_VIDEO_CONFIG_BPP(VIDEO_CONFIG_24BPP) |
			DSI_TPG_VIDEO_CONFIG_RGB);

	reg |= DSI_TEST_PATTERN_GEN_CTRL_VIDEO_PATTERN_SEL(VID_MDSS_GENERAL_PATTERN);
	dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL, reg);

	DBG("Video test pattern setup done\n");
}

static void msm_dsi_host_cmd_test_pattern_setup(struct msm_dsi_host *msm_host)
{
	u32 reg;

	reg = dsi_read(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL);

	/* initial value for test pattern */
	dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_CMD_MDP_INIT_VAL0, 0xff);

	reg |= DSI_TEST_PATTERN_GEN_CTRL_CMD_MDP_STREAM0_PATTERN_SEL(CMD_MDP_MDSS_GENERAL_PATTERN);

	dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL, reg);
	/* draw checkered rectangle pattern */
	dsi_write(msm_host, REG_DSI_TPG_MAIN_CONTROL2,
			DSI_TPG_MAIN_CONTROL2_CMD_MDP0_CHECKERED_RECTANGLE_PATTERN);

	DBG("Cmd test pattern setup done\n");
}

void msm_dsi_host_test_pattern_en(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);
	bool is_video_mode = !!(msm_host->mode_flags & MIPI_DSI_MODE_VIDEO);
	u32 reg;

	if (is_video_mode)
		msm_dsi_host_video_test_pattern_setup(msm_host);
	else
		msm_dsi_host_cmd_test_pattern_setup(msm_host);

	reg = dsi_read(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL);
	/* enable the test pattern generator */
	dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_CTRL, (reg | DSI_TEST_PATTERN_GEN_CTRL_EN));

	/* for command mode need to trigger one frame from tpg */
	if (!is_video_mode)
		dsi_write(msm_host, REG_DSI_TEST_PATTERN_GEN_CMD_STREAM0_TRIGGER,
				DSI_TEST_PATTERN_GEN_CMD_STREAM0_TRIGGER_SW_TRIGGER);
}

struct drm_dsc_config *msm_dsi_host_get_dsc_config(struct mipi_dsi_host *host)
{
	struct msm_dsi_host *msm_host = to_msm_dsi_host(host);

	return msm_host->dsc;
}
