/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SOF_INTEL_LNL_H
#define __SOF_INTEL_LNL_H

#define LNL_DSP_REG_HFDSC		0x160200 /* DSP core0 status */
#define LNL_DSP_REG_HFDEC		0x160204 /* DSP core0 error */

int sof_lnl_set_ops(struct snd_sof_dev *sdev, struct snd_sof_dsp_ops *dsp_ops);

bool lnl_dsp_check_sdw_irq(struct snd_sof_dev *sdev);
int lnl_dsp_disable_interrupts(struct snd_sof_dev *sdev);
bool lnl_sdw_check_wakeen_irq(struct snd_sof_dev *sdev);

#endif /* __SOF_INTEL_LNL_H */
