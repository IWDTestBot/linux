// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas SDHI
 *
 * Copyright (C) 2015-19 Renesas Electronics Corporation
 * Copyright (C) 2016-19 Sang Engineering, Wolfram Sang
 * Copyright (C) 2016-17 Horms Solutions, Simon Horman
 * Copyright (C) 2009 Magnus Damm
 *
 * Based on "Compaq ASIC3 support":
 *
 * Copyright 2001 Compaq Computer Corporation.
 * Copyright 2004-2005 Phil Blundell
 * Copyright 2007-2008 OpenedHand Ltd.
 *
 * Authors: Phil Blundell <pb@handhelds.org>,
 *	    Samuel Ortiz <sameo@openedhand.com>
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl-state.h>
#include <linux/platform_data/tmio.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/reset.h>
#include <linux/sh_dma.h>
#include <linux/slab.h>

#include "renesas_sdhi.h"
#include "tmio_mmc.h"

#define CTL_HOST_MODE	0xe4
#define HOST_MODE_GEN2_SDR50_WMODE	BIT(0)
#define HOST_MODE_GEN2_SDR104_WMODE	BIT(0)
#define HOST_MODE_GEN3_WMODE		BIT(0)
#define HOST_MODE_GEN3_BUSWIDTH		BIT(8)

#define HOST_MODE_GEN3_16BIT	HOST_MODE_GEN3_WMODE
#define HOST_MODE_GEN3_32BIT	(HOST_MODE_GEN3_WMODE | HOST_MODE_GEN3_BUSWIDTH)
#define HOST_MODE_GEN3_64BIT	0

#define SDHI_VER_GEN2_SDR50	0x490c
#define SDHI_VER_RZ_A1		0x820b
/* very old datasheets said 0x490c for SDR104, too. They are wrong! */
#define SDHI_VER_GEN2_SDR104	0xcb0d
#define SDHI_VER_GEN3_SD	0xcc10
#define SDHI_VER_GEN3_SDMMC	0xcd10

#define SDHI_GEN3_MMC0_ADDR	0xee140000

static void renesas_sdhi_sdbuf_width(struct tmio_mmc_host *host, int width)
{
	u32 val;

	/*
	 * see also
	 *	renesas_sdhi_of_data :: dma_buswidth
	 */
	switch (sd_ctrl_read16(host, CTL_VERSION)) {
	case SDHI_VER_GEN2_SDR50:
		val = (width == 32) ? HOST_MODE_GEN2_SDR50_WMODE : 0;
		break;
	case SDHI_VER_GEN2_SDR104:
		val = (width == 32) ? 0 : HOST_MODE_GEN2_SDR104_WMODE;
		break;
	case SDHI_VER_GEN3_SD:
	case SDHI_VER_GEN3_SDMMC:
		if (width == 64)
			val = HOST_MODE_GEN3_64BIT;
		else if (width == 32)
			val = HOST_MODE_GEN3_32BIT;
		else
			val = HOST_MODE_GEN3_16BIT;
		break;
	default:
		/* nothing to do */
		return;
	}

	sd_ctrl_write16(host, CTL_HOST_MODE, val);
}

static int renesas_sdhi_clk_enable(struct tmio_mmc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct renesas_sdhi *priv = host_to_priv(host);
	int ret;

	ret = clk_prepare_enable(priv->clk_cd);
	if (ret < 0)
		return ret;

	/*
	 * The clock driver may not know what maximum frequency
	 * actually works, so it should be set with the max-frequency
	 * property which will already have been read to f_max.  If it
	 * was missing, assume the current frequency is the maximum.
	 */
	if (!mmc->f_max)
		mmc->f_max = clk_get_rate(priv->clk);

	/*
	 * Minimum frequency is the minimum input clock frequency
	 * divided by our maximum divider.
	 */
	mmc->f_min = max(clk_round_rate(priv->clk, 1) / 512, 1L);

	/* enable 16bit data access on SDBUF as default */
	renesas_sdhi_sdbuf_width(host, 16);

	return 0;
}

static unsigned int renesas_sdhi_clk_update(struct tmio_mmc_host *host,
					    unsigned int wanted_clock)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	struct clk *ref_clk = priv->clk;
	unsigned int freq, diff, best_freq = 0, diff_min = ~0;
	unsigned int new_clock, clkh_shift = 0;
	unsigned int new_upper_limit;
	int i;

	/*
	 * We simply return the current rate if a) we are not on a R-Car Gen2+
	 * SoC (may work for others, but untested) or b) if the SCC needs its
	 * clock during tuning, so we don't change the external clock setup.
	 */
	if (!(host->pdata->flags & TMIO_MMC_MIN_RCAR2) || mmc_doing_tune(host->mmc))
		return clk_get_rate(priv->clk);

	if (priv->clkh) {
		/* HS400 with 4TAP needs different clock settings */
		bool use_4tap = sdhi_has_quirk(priv, hs400_4taps);
		bool need_slow_clkh = host->mmc->ios.timing == MMC_TIMING_MMC_HS400;
		clkh_shift = use_4tap && need_slow_clkh ? 1 : 2;
		ref_clk = priv->clkh;
	}

	new_clock = wanted_clock << clkh_shift;

	/*
	 * We want the bus clock to be as close as possible to, but no
	 * greater than, new_clock.  As we can divide by 1 << i for
	 * any i in [0, 9] we want the input clock to be as close as
	 * possible, but no greater than, new_clock << i.
	 *
	 * Add an upper limit of 1/1024 rate higher to the clock rate to fix
	 * clk rate jumping to lower rate due to rounding error (eg: RZ/G2L has
	 * 3 clk sources 533.333333 MHz, 400 MHz and 266.666666 MHz. The request
	 * for 533.333333 MHz will selects a slower 400 MHz due to rounding
	 * error (533333333 Hz / 4 * 4 = 533333332 Hz < 533333333 Hz)).
	 */
	for (i = min(9, ilog2(UINT_MAX / new_clock)); i >= 0; i--) {
		freq = clk_round_rate(ref_clk, new_clock << i);
		new_upper_limit = (new_clock << i) + ((new_clock << i) >> 10);
		if (freq > new_upper_limit) {
			/* Too fast; look for a slightly slower option */
			freq = clk_round_rate(ref_clk, (new_clock << i) / 4 * 3);
			if (freq > new_upper_limit)
				continue;
		}

		diff = new_clock - (freq >> i);
		if (diff <= diff_min) {
			best_freq = freq;
			diff_min = diff;
		}
	}

	clk_set_rate(ref_clk, best_freq);

	if (priv->clkh)
		clk_set_rate(priv->clk, best_freq >> clkh_shift);

	return clk_get_rate(priv->clk);
}

static void renesas_sdhi_set_clock(struct tmio_mmc_host *host,
				   unsigned int new_clock)
{
	unsigned int clk_margin;
	u32 clk = 0, clock;

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	if (new_clock == 0) {
		host->mmc->actual_clock = 0;
		goto out;
	}

	host->mmc->actual_clock = renesas_sdhi_clk_update(host, new_clock);
	clock = host->mmc->actual_clock / 512;

	/*
	 * Add a margin of 1/1024 rate higher to the clock rate in order
	 * to avoid clk variable setting a value of 0 due to the margin
	 * provided for actual_clock in renesas_sdhi_clk_update().
	 */
	clk_margin = new_clock >> 10;
	for (clk = 0x80000080; new_clock + clk_margin >= (clock << 1); clk >>= 1)
		clock <<= 1;

	/* 1/1 clock is option */
	if ((host->pdata->flags & TMIO_MMC_CLK_ACTUAL) && ((clk >> 22) & 0x1)) {
		if (!(host->mmc->ios.timing == MMC_TIMING_MMC_HS400))
			clk |= 0xff;
		else
			clk &= ~0xff;
	}

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, clk & CLK_CTL_DIV_MASK);
	if (!(host->pdata->flags & TMIO_MMC_MIN_RCAR2))
		usleep_range(10000, 11000);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

out:
	/* HW engineers overrode docs: no sleep needed on R-Car2+ */
	if (!(host->pdata->flags & TMIO_MMC_MIN_RCAR2))
		usleep_range(10000, 11000);
}

static void renesas_sdhi_clk_disable(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	clk_disable_unprepare(priv->clk_cd);
}

static int renesas_sdhi_card_busy(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);

	return !(sd_ctrl_read16_and_16_as_32(host, CTL_STATUS) &
		 TMIO_STAT_DAT0);
}

static int renesas_sdhi_start_signal_voltage_switch(struct mmc_host *mmc,
						    struct mmc_ios *ios)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);
	struct pinctrl_state *pin_state;
	int ret;

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_330:
		pin_state = priv->pins_default;
		break;
	case MMC_SIGNAL_VOLTAGE_180:
		pin_state = priv->pins_uhs;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * If anything is missing, assume signal voltage is fixed at
	 * 3.3V and succeed/fail accordingly.
	 */
	if (IS_ERR(priv->pinctrl) || IS_ERR(pin_state))
		return ios->signal_voltage ==
			MMC_SIGNAL_VOLTAGE_330 ? 0 : -EINVAL;

	ret = mmc_regulator_set_vqmmc(host->mmc, ios);
	if (ret < 0)
		return ret;

	return pinctrl_select_state(priv->pinctrl, pin_state);
}

/* SCC registers */
#define SH_MOBILE_SDHI_SCC_DTCNTL	0x000
#define SH_MOBILE_SDHI_SCC_TAPSET	0x002
#define SH_MOBILE_SDHI_SCC_DT2FF	0x004
#define SH_MOBILE_SDHI_SCC_CKSEL	0x006
#define SH_MOBILE_SDHI_SCC_RVSCNTL	0x008
#define SH_MOBILE_SDHI_SCC_RVSREQ	0x00A
#define SH_MOBILE_SDHI_SCC_SMPCMP       0x00C
#define SH_MOBILE_SDHI_SCC_TMPPORT2	0x00E
#define SH_MOBILE_SDHI_SCC_TMPPORT3	0x014
#define SH_MOBILE_SDHI_SCC_TMPPORT4	0x016
#define SH_MOBILE_SDHI_SCC_TMPPORT5	0x018
#define SH_MOBILE_SDHI_SCC_TMPPORT6	0x01A
#define SH_MOBILE_SDHI_SCC_TMPPORT7	0x01C

#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN		BIT(0)
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT	16
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_MASK	0xff

#define SH_MOBILE_SDHI_SCC_CKSEL_DTSEL		BIT(0)

#define SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN	BIT(0)

#define SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPDOWN	BIT(0)
#define SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPUP	BIT(1)
#define SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR	BIT(2)

#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQDOWN	BIT(8)
#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQUP	BIT(24)
#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_ERR	(BIT(8) | BIT(24))

#define SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL	BIT(4)
#define SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN	BIT(31)

/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT4 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START	BIT(0)

/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT5 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_R	BIT(8)
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_W	(0 << 8)
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK	0x3F

/* Definitions for values the SH_MOBILE_SDHI_SCC register */
#define SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE	0xa5000000
#define SH_MOBILE_SDHI_SCC_TMPPORT_CALIB_CODE_MASK	0x1f
#define SH_MOBILE_SDHI_SCC_TMPPORT_MANUAL_MODE		BIT(7)

static inline u32 sd_scc_read32(struct tmio_mmc_host *host,
				struct renesas_sdhi *priv, int addr)
{
	return readl(priv->scc_ctl + (addr << host->bus_shift));
}

static inline void sd_scc_write32(struct tmio_mmc_host *host,
				  struct renesas_sdhi *priv,
				  int addr, u32 val)
{
	writel(val, priv->scc_ctl + (addr << host->bus_shift));
}

static unsigned int renesas_sdhi_init_tuning(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv;

	priv = host_to_priv(host);

	/* Initialize SCC */
	sd_ctrl_write32_as_16_and_16(host, CTL_STATUS, 0x0);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* set sampling clock selection range */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN |
		       0x8 << SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       SH_MOBILE_SDHI_SCC_CKSEL_DTSEL |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF, priv->scc_tappos);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Read TAPNUM */
	return (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL) >>
		SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT) &
		SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_MASK;
}

static void renesas_sdhi_hs400_complete(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);
	u32 bad_taps = priv->quirks ? priv->quirks->hs400_bad_taps : 0;
	bool use_4tap = sdhi_has_quirk(priv, hs400_4taps);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Set HS400 mode */
	sd_ctrl_write16(host, CTL_SDIF_MODE, SDIF_MODE_HS400 |
			sd_ctrl_read16(host, CTL_SDIF_MODE));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF,
		       priv->scc_tappos_hs400);

	if (sdhi_has_quirk(priv, manual_tap_correction))
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
			       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
			       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2,
		       (SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN |
			SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL) |
			sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN |
		       sd_scc_read32(host, priv,
				     SH_MOBILE_SDHI_SCC_DTCNTL));

	/* Avoid bad TAP */
	if (bad_taps & BIT(priv->tap_set)) {
		u32 new_tap = (priv->tap_set + 1) % priv->tap_num;

		if (bad_taps & BIT(new_tap))
			new_tap = (priv->tap_set - 1) % priv->tap_num;

		if (bad_taps & BIT(new_tap)) {
			new_tap = priv->tap_set;
			dev_dbg(&host->pdev->dev, "Can't handle three bad tap in a row\n");
		}

		priv->tap_set = new_tap;
	}

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET,
		       priv->tap_set / (use_4tap ? 2 : 1));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       SH_MOBILE_SDHI_SCC_CKSEL_DTSEL |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	if (priv->adjust_hs400_calib_table)
		priv->needs_adjust_hs400 = true;
}

static void renesas_sdhi_disable_scc(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       ~SH_MOBILE_SDHI_SCC_CKSEL_DTSEL &
		       sd_scc_read32(host, priv,
				     SH_MOBILE_SDHI_SCC_CKSEL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       ~SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN &
		       sd_scc_read32(host, priv,
				     SH_MOBILE_SDHI_SCC_DTCNTL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
}

static u32 sd_scc_tmpport_read32(struct tmio_mmc_host *host,
				 struct renesas_sdhi *priv, u32 addr)
{
	/* read mode */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT5,
		       SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_R |
		       (SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK & addr));

	/* access start and stop */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4,
		       SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4, 0);

	return sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT7);
}

static void sd_scc_tmpport_write32(struct tmio_mmc_host *host,
				   struct renesas_sdhi *priv, u32 addr, u32 val)
{
	/* write mode */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT5,
		       SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_W |
		       (SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK & addr));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT6, val);

	/* access start and stop */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4,
		       SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4, 0);
}

static void renesas_sdhi_adjust_hs400_mode_enable(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	u32 calib_code;

	/* disable write protect */
	sd_scc_tmpport_write32(host, priv, 0x00,
			       SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE);
	/* read calibration code and adjust */
	calib_code = sd_scc_tmpport_read32(host, priv, 0x26);
	calib_code &= SH_MOBILE_SDHI_SCC_TMPPORT_CALIB_CODE_MASK;

	sd_scc_tmpport_write32(host, priv, 0x22,
			       SH_MOBILE_SDHI_SCC_TMPPORT_MANUAL_MODE |
			       priv->adjust_hs400_calib_table[calib_code]);

	/* set offset value to TMPPORT3, hardcoded to OFFSET0 (= 0x3) for now */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT3, 0x3);

	/* adjustment done, clear flag */
	priv->needs_adjust_hs400 = false;
}

static void renesas_sdhi_adjust_hs400_mode_disable(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	/* disable write protect */
	sd_scc_tmpport_write32(host, priv, 0x00,
			       SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE);
	/* disable manual calibration */
	sd_scc_tmpport_write32(host, priv, 0x22, 0);
	/* clear offset value of TMPPORT3 */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT3, 0);
}

static void renesas_sdhi_reset_hs400_mode(struct tmio_mmc_host *host,
					  struct renesas_sdhi *priv)
{
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Reset HS400 mode */
	sd_ctrl_write16(host, CTL_SDIF_MODE, ~SDIF_MODE_HS400 &
			sd_ctrl_read16(host, CTL_SDIF_MODE));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF, priv->scc_tappos);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2,
		       ~(SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN |
			 SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL) &
			sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2));

	if (sdhi_has_quirk(priv, hs400_calib_table) || sdhi_has_quirk(priv, hs400_bad_taps))
		renesas_sdhi_adjust_hs400_mode_disable(host);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
}

static int renesas_sdhi_prepare_hs400_tuning(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);

	renesas_sdhi_reset_hs400_mode(host, host_to_priv(host));
	return 0;
}

static void renesas_sdhi_scc_reset(struct tmio_mmc_host *host, struct renesas_sdhi *priv)
{
	renesas_sdhi_disable_scc(host->mmc);
	renesas_sdhi_reset_hs400_mode(host, priv);
	priv->needs_adjust_hs400 = false;

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));
}

/* only populated for TMIO_MMC_MIN_RCAR2 */
static void renesas_sdhi_reset(struct tmio_mmc_host *host, bool preserve)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	int ret;
	u16 val;

	if (!preserve) {
		if (priv->rstc) {
			u32 sd_status;
			/*
			 * HW reset might have toggled the regulator state in
			 * HW which regulator core might be unaware of so save
			 * and restore the regulator state during HW reset.
			 */
			if (priv->rdev)
				sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);

			reset_control_reset(priv->rstc);
			/* Unknown why but without polling reset status, it will hang */
			read_poll_timeout(reset_control_status, ret, ret == 0, 1, 100,
					  false, priv->rstc);
			/* At least SDHI_VER_GEN2_SDR50 needs manual release of reset */
			sd_ctrl_write16(host, CTL_RESET_SD, 0x0001);
			if (priv->rdev)
				sd_ctrl_write32(host, CTL_SD_STATUS, sd_status);

			priv->needs_adjust_hs400 = false;
			renesas_sdhi_set_clock(host, host->clk_cache);

			/* Ensure default value for this driver. */
			renesas_sdhi_sdbuf_width(host, 16);
		} else if (priv->scc_ctl) {
			renesas_sdhi_scc_reset(host, priv);
		}
	}

	if (sd_ctrl_read16(host, CTL_VERSION) >= SDHI_VER_GEN3_SD) {
		val = sd_ctrl_read16(host, CTL_SD_MEM_CARD_OPT);
		val |= CARD_OPT_EXTOP;
		sd_ctrl_write16(host, CTL_SD_MEM_CARD_OPT, val);
	}
}

static unsigned int renesas_sdhi_gen3_get_cycles(struct tmio_mmc_host *host)
{
	u16 num, val = sd_ctrl_read16(host, CTL_SD_MEM_CARD_OPT);

	num = (val & CARD_OPT_TOP_MASK) >> CARD_OPT_TOP_SHIFT;
	return 1 << ((val & CARD_OPT_EXTOP ? 14 : 13) + num);

}

#define SH_MOBILE_SDHI_MIN_TAP_ROW 3

static int renesas_sdhi_select_tuning(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	unsigned int tap_start = 0, tap_end = 0, tap_cnt = 0, rs, re, i;
	unsigned int taps_size = priv->tap_num * 2, min_tap_row;
	unsigned long *bitmap;

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);

	/*
	 * When tuning CMD19 is issued twice for each tap, merge the
	 * result requiring the tap to be good in both runs before
	 * considering it for tuning selection.
	 */
	for (i = 0; i < taps_size; i++) {
		int offset = priv->tap_num * (i < priv->tap_num ? 1 : -1);

		if (!test_bit(i, priv->taps))
			clear_bit(i + offset, priv->taps);

		if (!test_bit(i, priv->smpcmp))
			clear_bit(i + offset, priv->smpcmp);
	}

	/*
	 * If all TAP are OK, the sampling clock position is selected by
	 * identifying the change point of data.
	 */
	if (bitmap_full(priv->taps, taps_size)) {
		bitmap = priv->smpcmp;
		min_tap_row = 1;
	} else {
		bitmap = priv->taps;
		min_tap_row = SH_MOBILE_SDHI_MIN_TAP_ROW;
	}

	/*
	 * Find the longest consecutive run of successful probes. If that
	 * is at least SH_MOBILE_SDHI_MIN_TAP_ROW probes long then use the
	 * center index as the tap, otherwise bail out.
	 */
	for_each_set_bitrange(rs, re, bitmap, taps_size) {
		if (re - rs > tap_cnt) {
			tap_end = re;
			tap_start = rs;
			tap_cnt = tap_end - tap_start;
		}
	}

	if (tap_cnt >= min_tap_row)
		priv->tap_set = (tap_start + tap_end) / 2 % priv->tap_num;
	else
		return -EIO;

	/* Set SCC */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET, priv->tap_set);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       (priv->card_is_sdio ? 0 : SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN) |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	return 0;
}

static int renesas_sdhi_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);
	int i, ret;

	priv->tap_num = renesas_sdhi_init_tuning(host);
	if (!priv->tap_num)
		return 0; /* Tuning is not supported */

	if (priv->tap_num * 2 >= sizeof(priv->taps) * BITS_PER_BYTE) {
		dev_err(&host->pdev->dev,
			"Too many taps, please update 'taps' in tmio_mmc_host!\n");
		return -EINVAL;
	}

	bitmap_zero(priv->taps, priv->tap_num * 2);
	bitmap_zero(priv->smpcmp, priv->tap_num * 2);

	/* Issue CMD19 twice for each tap */
	for (i = 0; i < 2 * priv->tap_num; i++) {
		int cmd_error = 0;

		/* Set sampling clock position */
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET, i % priv->tap_num);

		if (mmc_send_tuning(mmc, opcode, &cmd_error) == 0)
			set_bit(i, priv->taps);

		if (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_SMPCMP) == 0)
			set_bit(i, priv->smpcmp);

		if (cmd_error)
			mmc_send_abort_tuning(mmc, opcode);
	}

	ret = renesas_sdhi_select_tuning(host);
	if (ret < 0)
		renesas_sdhi_scc_reset(host, priv);
	return ret;
}

static bool renesas_sdhi_manual_correction(struct tmio_mmc_host *host, bool use_4tap)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	unsigned int new_tap = priv->tap_set, error_tap = priv->tap_set;
	u32 val;

	val = sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ);
	if (!val)
		return false;

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);

	/* Change TAP position according to correction status */
	if (sdhi_has_quirk(priv, manual_tap_correction) &&
	    host->mmc->ios.timing == MMC_TIMING_MMC_HS400) {
		u32 bad_taps = priv->quirks ? priv->quirks->hs400_bad_taps : 0;
		/*
		 * With HS400, the DAT signal is based on DS, not CLK.
		 * Therefore, use only CMD status.
		 */
		u32 smpcmp = sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_SMPCMP) &
					   SH_MOBILE_SDHI_SCC_SMPCMP_CMD_ERR;
		if (!smpcmp) {
			return false;	/* no error in CMD signal */
		} else if (smpcmp == SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQUP) {
			new_tap++;
			error_tap--;
		} else if (smpcmp == SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQDOWN) {
			new_tap--;
			error_tap++;
		} else {
			return true;	/* need retune */
		}

		/*
		 * When new_tap is a bad tap, we cannot change. Then, we compare
		 * with the HS200 tuning result. When smpcmp[error_tap] is OK,
		 * we can at least retune.
		 */
		if (bad_taps & BIT(new_tap % priv->tap_num))
			return test_bit(error_tap % priv->tap_num, priv->smpcmp);
	} else {
		if (!priv->card_is_sdio &&
		    !(val & SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR)) {
			u32 smpcmp = sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_SMPCMP);

			/* DAT1 is unmatched because of an SDIO irq */
			if (smpcmp & (BIT(17) | BIT(1)))
				return false;
		}
		if (val & SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR)
			return true;    /* need retune */
		else if (val & SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPUP)
			new_tap++;
		else if (val & SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPDOWN)
			new_tap--;
		else
			return false;
	}

	priv->tap_set = (new_tap % priv->tap_num);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET,
		       priv->tap_set / (use_4tap ? 2 : 1));

	return false;
}

static bool renesas_sdhi_auto_correction(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	/* Check SCC error */
	if (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ) &
	    SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR) {
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);
		return true;
	}

	return false;
}

static bool renesas_sdhi_check_scc_error(struct tmio_mmc_host *host,
					 struct mmc_request *mrq)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	bool use_4tap = sdhi_has_quirk(priv, hs400_4taps);
	bool ret = false;

	/*
	 * Skip checking SCC errors when running on 4 taps in HS400 mode as
	 * any retuning would still result in the same 4 taps being used.
	 */
	if (!(host->mmc->ios.timing == MMC_TIMING_UHS_SDR104) &&
	    !(host->mmc->ios.timing == MMC_TIMING_MMC_HS200) &&
	    !(host->mmc->ios.timing == MMC_TIMING_MMC_HS400 && !use_4tap))
		return false;

	if (mmc_doing_tune(host->mmc))
		return false;

	/* mrq can be NULL to check SCC error on SDIO irq without any request */
	if (mrq) {
		if (((mrq->cmd->error == -ETIMEDOUT) ||
		     (mrq->data && mrq->data->error == -ETIMEDOUT)) &&
		    ((host->mmc->caps & MMC_CAP_NONREMOVABLE) ||
		     (host->ops.get_cd && host->ops.get_cd(host->mmc))))
			ret |= true;
	}

	if (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL) &
	    SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN)
		ret |= renesas_sdhi_auto_correction(host);
	else
		ret |= renesas_sdhi_manual_correction(host, use_4tap);

	return ret;
}

static void renesas_sdhi_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);

	/*
	 * This controller cannot do auto-retune with SDIO irqs, so we
	 * then need to enforce manual correction. However, when tuning,
	 * mmc->card is not populated yet, so we don't know if the card
	 * is SDIO. init_card provides this information earlier, so we
	 * keep a copy of it.
	 */
	priv->card_is_sdio = mmc_card_sdio(card);
}

static void renesas_sdhi_sdio_irq(struct tmio_mmc_host *host)
{
	/* This controller requires retune when an SDIO irq occurs */
	if (renesas_sdhi_check_scc_error(host, NULL))
		mmc_retune_needed(host->mmc);
}

static int renesas_sdhi_wait_idle(struct tmio_mmc_host *host, u32 bit)
{
	int timeout = 1000;
	/* CBSY is set when busy, SCLKDIVEN is cleared when busy */
	u32 wait_state = (bit == TMIO_STAT_CMD_BUSY ? TMIO_STAT_CMD_BUSY : 0);

	while (--timeout && (sd_ctrl_read16_and_16_as_32(host, CTL_STATUS)
			      & bit) == wait_state)
		udelay(1);

	if (!timeout) {
		dev_warn(&host->pdev->dev, "timeout waiting for SD bus idle\n");
		return -EBUSY;
	}

	return 0;
}

static int renesas_sdhi_write16_hook(struct tmio_mmc_host *host, int addr)
{
	u32 bit = TMIO_STAT_SCLKDIVEN;

	switch (addr) {
	case CTL_SD_CMD:
	case CTL_STOP_INTERNAL_ACTION:
	case CTL_XFER_BLK_COUNT:
	case CTL_SD_XFER_LEN:
	case CTL_SD_MEM_CARD_OPT:
	case CTL_TRANSACTION_CTL:
	case CTL_DMA_ENABLE:
	case CTL_HOST_MODE:
		if (host->pdata->flags & TMIO_MMC_HAVE_CBSY)
			bit = TMIO_STAT_CMD_BUSY;
		fallthrough;
	case CTL_SD_CARD_CLK_CTL:
		return renesas_sdhi_wait_idle(host, bit);
	}

	return 0;
}

static int renesas_sdhi_multi_io_quirk(struct mmc_card *card,
				       unsigned int direction, int blk_size)
{
	/*
	 * In Renesas controllers, when performing a
	 * multiple block read of one or two blocks,
	 * depending on the timing with which the
	 * response register is read, the response
	 * value may not be read properly.
	 * Use single block read for this HW bug
	 */
	if ((direction == MMC_DATA_READ) &&
	    blk_size == 2)
		return 1;

	return blk_size;
}

static void renesas_sdhi_fixup_request(struct tmio_mmc_host *host, struct mmc_request *mrq)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	if (priv->needs_adjust_hs400 && mrq->cmd->opcode == MMC_SEND_STATUS)
		renesas_sdhi_adjust_hs400_mode_enable(host);
}
static void renesas_sdhi_enable_dma(struct tmio_mmc_host *host, bool enable)
{
	/* Iff regs are 8 byte apart, sdbuf is 64 bit. Otherwise always 32. */
	int width = (host->bus_shift == 2) ? 64 : 32;

	sd_ctrl_write16(host, CTL_DMA_ENABLE, enable ? DMA_ENABLE_DMASDRW : 0);
	renesas_sdhi_sdbuf_width(host, enable ? width : 16);
}

static const unsigned int renesas_sdhi_vqmmc_voltages[] = {
	3300000, 1800000
};

static int renesas_sdhi_regulator_disable(struct regulator_dev *rdev)
{
	struct tmio_mmc_host *host = rdev_get_drvdata(rdev);
	u32 sd_status;

	sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);
	sd_status &= ~SD_STATUS_PWEN;
	sd_ctrl_write32(host, CTL_SD_STATUS, sd_status);

	return 0;
}

static int renesas_sdhi_regulator_enable(struct regulator_dev *rdev)
{
	struct tmio_mmc_host *host = rdev_get_drvdata(rdev);
	u32 sd_status;

	sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);
	sd_status |= SD_STATUS_PWEN;
	sd_ctrl_write32(host, CTL_SD_STATUS, sd_status);

	return 0;
}

static int renesas_sdhi_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct tmio_mmc_host *host = rdev_get_drvdata(rdev);
	u32 sd_status;

	sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);

	return (sd_status & SD_STATUS_PWEN) ? 1 : 0;
}

static int renesas_sdhi_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct tmio_mmc_host *host = rdev_get_drvdata(rdev);
	u32 sd_status;

	sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);

	return (sd_status & SD_STATUS_IOVS) ? 1800000 : 3300000;
}

static int renesas_sdhi_regulator_set_voltage(struct regulator_dev *rdev,
					      int min_uV, int max_uV,
					      unsigned int *selector)
{
	struct tmio_mmc_host *host = rdev_get_drvdata(rdev);
	u32 sd_status;

	sd_status = sd_ctrl_read32(host, CTL_SD_STATUS);
	if (min_uV >= 1700000 && max_uV <= 1950000) {
		sd_status |= SD_STATUS_IOVS;
		*selector = 1;
	} else {
		sd_status &= ~SD_STATUS_IOVS;
		*selector = 0;
	}
	sd_ctrl_write32(host, CTL_SD_STATUS, sd_status);

	return 0;
}

static int renesas_sdhi_regulator_list_voltage(struct regulator_dev *rdev,
					       unsigned int selector)
{
	if (selector >= ARRAY_SIZE(renesas_sdhi_vqmmc_voltages))
		return -EINVAL;

	return renesas_sdhi_vqmmc_voltages[selector];
}

static const struct regulator_ops renesas_sdhi_regulator_voltage_ops = {
	.enable = renesas_sdhi_regulator_enable,
	.disable = renesas_sdhi_regulator_disable,
	.is_enabled = renesas_sdhi_regulator_is_enabled,
	.list_voltage = renesas_sdhi_regulator_list_voltage,
	.get_voltage = renesas_sdhi_regulator_get_voltage,
	.set_voltage = renesas_sdhi_regulator_set_voltage,
};

static const struct regulator_desc renesas_sdhi_vqmmc_regulator = {
	.name = "sdhi-vqmmc-regulator",
	.of_match = of_match_ptr("vqmmc-regulator"),
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &renesas_sdhi_regulator_voltage_ops,
	.volt_table = renesas_sdhi_vqmmc_voltages,
	.n_voltages = ARRAY_SIZE(renesas_sdhi_vqmmc_voltages),
};

int renesas_sdhi_probe(struct platform_device *pdev,
		       const struct tmio_mmc_dma_ops *dma_ops,
		       const struct renesas_sdhi_of_data *of_data,
		       const struct renesas_sdhi_quirks *quirks)
{
	struct tmio_mmc_data *mmd = pdev->dev.platform_data;
	struct tmio_mmc_data *mmc_data;
	struct regulator_config rcfg = { .dev = &pdev->dev, };
	struct regulator_dev *rdev;
	struct renesas_sdhi_dma *dma_priv;
	struct device *dev = &pdev->dev;
	struct tmio_mmc_host *host;
	struct renesas_sdhi *priv;
	int num_irqs, irq, ret, i;
	struct resource *res;
	u16 ver;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct renesas_sdhi),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->quirks = quirks;
	mmc_data = &priv->mmc_data;
	dma_priv = &priv->dma_priv;

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->clk), "cannot get clock");

	priv->clkh = devm_clk_get_optional(&pdev->dev, "clkh");
	if (IS_ERR(priv->clkh))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->clkh), "cannot get clkh");

	/*
	 * Some controllers provide a 2nd clock just to run the internal card
	 * detection logic. Unfortunately, the existing driver architecture does
	 * not support a separation of clocks for runtime PM usage. When
	 * native hotplug is used, the tmio driver assumes that the core
	 * must continue to run for card detect to stay active, so we cannot
	 * disable it.
	 * Additionally, it is prohibited to supply a clock to the core but not
	 * to the card detect circuit. That leaves us with if separate clocks
	 * are presented, we must treat them both as virtually 1 clock.
	 */
	priv->clk_cd = devm_clk_get_optional(&pdev->dev, "cd");
	if (IS_ERR(priv->clk_cd))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->clk_cd), "cannot get cd clock");

	priv->rstc = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(priv->rstc))
		return PTR_ERR(priv->rstc);

	priv->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (!IS_ERR(priv->pinctrl)) {
		priv->pins_default = pinctrl_lookup_state(priv->pinctrl,
						PINCTRL_STATE_DEFAULT);
		priv->pins_uhs = pinctrl_lookup_state(priv->pinctrl,
						"state_uhs");
	}

	host = tmio_mmc_host_alloc(pdev, mmc_data);
	if (IS_ERR(host))
		return PTR_ERR(host);

	priv->host = host;

	if (of_data) {
		mmc_data->flags |= of_data->tmio_flags;
		mmc_data->ocr_mask = of_data->tmio_ocr_mask;
		mmc_data->capabilities |= of_data->capabilities;
		mmc_data->capabilities2 |= of_data->capabilities2;
		mmc_data->dma_rx_offset = of_data->dma_rx_offset;
		mmc_data->max_blk_count = of_data->max_blk_count;
		mmc_data->max_segs = of_data->max_segs;
		dma_priv->dma_buswidth = of_data->dma_buswidth;
		host->bus_shift = of_data->bus_shift;
		/* Fallback for old DTs */
		if (!priv->clkh && of_data->sdhi_flags & SDHI_FLAG_NEED_CLKH_FALLBACK)
			priv->clkh = clk_get_parent(clk_get_parent(priv->clk));

	}

	host->write16_hook = renesas_sdhi_write16_hook;
	host->clk_enable = renesas_sdhi_clk_enable;
	host->clk_disable = renesas_sdhi_clk_disable;
	host->set_clock = renesas_sdhi_set_clock;
	host->multi_io_quirk = renesas_sdhi_multi_io_quirk;
	host->dma_ops = dma_ops;

	if (sdhi_has_quirk(priv, hs400_disabled))
		host->mmc->caps2 &= ~(MMC_CAP2_HS400 | MMC_CAP2_HS400_ES);

	/* For some SoC, we disable internal WP. GPIO may override this */
	if (mmc_host_can_gpio_ro(host->mmc))
		mmc_data->capabilities2 &= ~MMC_CAP2_NO_WRITE_PROTECT;

	/* SDR speeds are only available on Gen2+ */
	if (mmc_data->flags & TMIO_MMC_MIN_RCAR2) {
		/* card_busy caused issues on r8a73a4 (pre-Gen2) CD-less SDHI */
		host->ops.card_busy = renesas_sdhi_card_busy;
		host->ops.start_signal_voltage_switch =
			renesas_sdhi_start_signal_voltage_switch;
		host->sdcard_irq_setbit_mask = TMIO_STAT_ALWAYS_SET_27;
		host->sdcard_irq_mask_all = TMIO_MASK_ALL_RCAR2;
		host->reset = renesas_sdhi_reset;
	} else {
		host->sdcard_irq_mask_all = TMIO_MASK_ALL;
	}

	/* Orginally registers were 16 bit apart, could be 32 or 64 nowadays */
	if (!host->bus_shift && resource_size(res) > 0x100) /* old way to determine the shift */
		host->bus_shift = 1;

	if (mmd)
		*mmc_data = *mmd;

	dma_priv->filter = shdma_chan_filter;
	dma_priv->enable = renesas_sdhi_enable_dma;

	mmc_data->capabilities |= MMC_CAP_MMC_HIGHSPEED;

	/*
	 * All SDHI blocks support 2-byte and larger block sizes in 4-bit
	 * bus width mode.
	 */
	mmc_data->flags |= TMIO_MMC_BLKSZ_2BYTES;

	/*
	 * All SDHI blocks support SDIO IRQ signalling.
	 */
	mmc_data->flags |= TMIO_MMC_SDIO_IRQ;

	/* All SDHI have CMD12 control bit */
	mmc_data->flags |= TMIO_MMC_HAVE_CMD12_CTRL;

	/* All SDHI have SDIO status bits which must be 1 */
	mmc_data->flags |= TMIO_MMC_SDIO_STATUS_SETBITS;

	/* All SDHI support HW busy detection */
	mmc_data->flags |= TMIO_MMC_USE_BUSY_TIMEOUT;

	dev_pm_domain_start(&pdev->dev);

	ret = renesas_sdhi_clk_enable(host);
	if (ret)
		return ret;

	rcfg.of_node = of_get_available_child_by_name(dev->of_node, "vqmmc-regulator");
	if (rcfg.of_node) {
		rcfg.driver_data = priv->host;
		rdev = devm_regulator_register(dev, &renesas_sdhi_vqmmc_regulator, &rcfg);
		of_node_put(rcfg.of_node);
		if (IS_ERR(rdev)) {
			dev_err(dev, "regulator register failed err=%ld", PTR_ERR(rdev));
			ret = PTR_ERR(rdev);
			goto edisclk;
		}
		priv->rdev = rdev;
	}

	ver = sd_ctrl_read16(host, CTL_VERSION);
	/* GEN2_SDR104 is first known SDHI to use 32bit block count */
	if (ver < SDHI_VER_GEN2_SDR104 && mmc_data->max_blk_count > U16_MAX)
		mmc_data->max_blk_count = U16_MAX;

	/* One Gen2 SDHI incarnation does NOT have a CBSY bit */
	if (ver == SDHI_VER_GEN2_SDR50)
		mmc_data->flags &= ~TMIO_MMC_HAVE_CBSY;

	if (ver == SDHI_VER_GEN3_SDMMC && sdhi_has_quirk(priv, hs400_calib_table)) {
		host->fixup_request = renesas_sdhi_fixup_request;
		priv->adjust_hs400_calib_table = *(
			res->start == SDHI_GEN3_MMC0_ADDR ?
			quirks->hs400_calib_table :
			quirks->hs400_calib_table + 1);
	}

	/* these have an EXTOP bit */
	if (ver >= SDHI_VER_GEN3_SD)
		host->get_timeout_cycles = renesas_sdhi_gen3_get_cycles;

	/* Check for SCC so we can reset it if needed */
	if (of_data && of_data->scc_offset && ver >= SDHI_VER_GEN2_SDR104)
		priv->scc_ctl = host->ctl + of_data->scc_offset;

	/* Enable tuning iff we have an SCC and a supported mode */
	if (priv->scc_ctl && (host->mmc->caps & MMC_CAP_UHS_SDR104 ||
	    host->mmc->caps2 & MMC_CAP2_HSX00_1_8V)) {
		const struct renesas_sdhi_scc *taps = of_data->taps;
		bool use_4tap = sdhi_has_quirk(priv, hs400_4taps);
		bool hit = false;

		for (i = 0; i < of_data->taps_num; i++) {
			if (taps[i].clk_rate == 0 ||
			    taps[i].clk_rate == host->mmc->f_max) {
				priv->scc_tappos = taps->tap;
				priv->scc_tappos_hs400 = use_4tap ?
							 taps->tap_hs400_4tap :
							 taps->tap;
				hit = true;
				break;
			}
		}

		if (!hit)
			dev_warn(&host->pdev->dev, "Unknown clock rate for tuning\n");

		host->check_retune = renesas_sdhi_check_scc_error;
		host->sdio_irq = renesas_sdhi_sdio_irq;
		host->ops.init_card = renesas_sdhi_init_card;
		host->ops.execute_tuning = renesas_sdhi_execute_tuning;
		host->ops.prepare_hs400_tuning = renesas_sdhi_prepare_hs400_tuning;
		host->ops.hs400_downgrade = renesas_sdhi_disable_scc;
		host->ops.hs400_complete = renesas_sdhi_hs400_complete;
	}

	sd_ctrl_write32_as_16_and_16(host, CTL_IRQ_MASK, host->sdcard_irq_mask_all);

	/* There must be at least one IRQ source */
	num_irqs = platform_irq_count(pdev);
	if (num_irqs <= 0) {
		ret = num_irqs ?: -ENOENT;
		goto edisclk;
	}

	for (i = 0; i < num_irqs; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0) {
			ret = irq;
			goto edisclk;
		}

		ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_irq, 0,
				       dev_name(&pdev->dev), host);
		if (ret)
			goto edisclk;
	}

	ret = tmio_mmc_host_probe(host);
	if (ret < 0)
		goto edisclk;

	dev_info(&pdev->dev, "%s base at %pa, max clock rate %u MHz\n",
		 mmc_hostname(host->mmc), &res->start, host->mmc->f_max / 1000000);

	return ret;

edisclk:
	renesas_sdhi_clk_disable(host);
	return ret;
}
EXPORT_SYMBOL_GPL(renesas_sdhi_probe);

void renesas_sdhi_remove(struct platform_device *pdev)
{
	struct tmio_mmc_host *host = platform_get_drvdata(pdev);

	tmio_mmc_host_remove(host);
	renesas_sdhi_clk_disable(host);
}
EXPORT_SYMBOL_GPL(renesas_sdhi_remove);

MODULE_DESCRIPTION("Renesas SDHI core driver");
MODULE_LICENSE("GPL v2");
