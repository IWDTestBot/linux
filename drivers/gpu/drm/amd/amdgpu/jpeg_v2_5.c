/*
 * Copyright 2019 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "amdgpu.h"
#include "amdgpu_jpeg.h"
#include "soc15.h"
#include "soc15d.h"
#include "jpeg_v2_0.h"
#include "jpeg_v2_5.h"

#include "vcn/vcn_2_5_offset.h"
#include "vcn/vcn_2_5_sh_mask.h"
#include "ivsrcid/vcn/irqsrcs_vcn_2_0.h"

#define mmUVD_JPEG_PITCH_INTERNAL_OFFSET			0x401f

#define JPEG25_MAX_HW_INSTANCES_ARCTURUS			2

static const struct amdgpu_hwip_reg_entry jpeg_reg_list_2_5[] = {
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JPEG_POWER_STATUS),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JPEG_INT_STAT),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JRBC_RB_RPTR),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JRBC_RB_WPTR),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JRBC_RB_CNTL),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JRBC_RB_SIZE),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JRBC_STATUS),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmJPEG_DEC_ADDR_MODE),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmJPEG_DEC_GFX10_ADDR_CONFIG),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmJPEG_DEC_Y_GFX10_TILING_SURFACE),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmJPEG_DEC_UV_GFX10_TILING_SURFACE),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JPEG_PITCH),
	SOC15_REG_ENTRY_STR(JPEG, 0, mmUVD_JPEG_UV_PITCH),
};

static void jpeg_v2_5_set_dec_ring_funcs(struct amdgpu_device *adev);
static void jpeg_v2_5_set_irq_funcs(struct amdgpu_device *adev);
static int jpeg_v2_5_set_powergating_state(struct amdgpu_ip_block *ip_block,
				enum amd_powergating_state state);
static void jpeg_v2_5_set_ras_funcs(struct amdgpu_device *adev);

static int amdgpu_ih_clientid_jpeg[] = {
	SOC15_IH_CLIENTID_VCN,
	SOC15_IH_CLIENTID_VCN1
};

/**
 * jpeg_v2_5_early_init - set function pointers
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Set ring and irq function pointers
 */
static int jpeg_v2_5_early_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	u32 harvest;
	int i;

	adev->jpeg.num_jpeg_rings = 1;
	adev->jpeg.num_jpeg_inst = JPEG25_MAX_HW_INSTANCES_ARCTURUS;
	for (i = 0; i < adev->jpeg.num_jpeg_inst; i++) {
		harvest = RREG32_SOC15(JPEG, i, mmCC_UVD_HARVESTING);
		if (harvest & CC_UVD_HARVESTING__UVD_DISABLE_MASK)
			adev->jpeg.harvest_config |= 1 << i;
	}
	if (adev->jpeg.harvest_config == (AMDGPU_JPEG_HARVEST_JPEG0 |
					 AMDGPU_JPEG_HARVEST_JPEG1))
		return -ENOENT;

	jpeg_v2_5_set_dec_ring_funcs(adev);
	jpeg_v2_5_set_irq_funcs(adev);
	jpeg_v2_5_set_ras_funcs(adev);

	return 0;
}

/**
 * jpeg_v2_5_sw_init - sw init for JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Load firmware and sw initialization
 */
static int jpeg_v2_5_sw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_ring *ring;
	int i, r;
	struct amdgpu_device *adev = ip_block->adev;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		/* JPEG TRAP */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
				VCN_2_0__SRCID__JPEG_DECODE, &adev->jpeg.inst[i].irq);
		if (r)
			return r;

		/* JPEG DJPEG POISON EVENT */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
			VCN_2_6__SRCID_DJPEG0_POISON, &adev->jpeg.inst[i].ras_poison_irq);
		if (r)
			return r;

		/* JPEG EJPEG POISON EVENT */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
			VCN_2_6__SRCID_EJPEG0_POISON, &adev->jpeg.inst[i].ras_poison_irq);
		if (r)
			return r;
	}

	r = amdgpu_jpeg_sw_init(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_resume(adev);
	if (r)
		return r;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ring = adev->jpeg.inst[i].ring_dec;
		ring->use_doorbell = true;
		if (amdgpu_ip_version(adev, UVD_HWIP, 0) == IP_VERSION(2, 5, 0))
			ring->vm_hub = AMDGPU_MMHUB1(0);
		else
			ring->vm_hub = AMDGPU_MMHUB0(0);
		ring->doorbell_index = (adev->doorbell_index.vcn.vcn_ring0_1 << 1) + 1 + 8 * i;
		sprintf(ring->name, "jpeg_dec_%d", i);
		r = amdgpu_ring_init(adev, ring, 512, &adev->jpeg.inst[i].irq,
				     0, AMDGPU_RING_PRIO_DEFAULT, NULL);
		if (r)
			return r;

		adev->jpeg.internal.jpeg_pitch[0] = mmUVD_JPEG_PITCH_INTERNAL_OFFSET;
		adev->jpeg.inst[i].external.jpeg_pitch[0] = SOC15_REG_OFFSET(JPEG, i, mmUVD_JPEG_PITCH);
	}

	r = amdgpu_jpeg_ras_sw_init(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_reg_dump_init(adev, jpeg_reg_list_2_5, ARRAY_SIZE(jpeg_reg_list_2_5));
	if (r)
		return r;

	adev->jpeg.supported_reset =
		amdgpu_get_soft_full_reset_mask(adev->jpeg.inst[0].ring_dec);
	if (!amdgpu_sriov_vf(adev))
		adev->jpeg.supported_reset |= AMDGPU_RESET_TYPE_PER_QUEUE;
	r = amdgpu_jpeg_sysfs_reset_mask_init(adev);

	return r;
}

/**
 * jpeg_v2_5_sw_fini - sw fini for JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * JPEG suspend and free up sw allocation
 */
static int jpeg_v2_5_sw_fini(struct amdgpu_ip_block *ip_block)
{
	int r;
	struct amdgpu_device *adev = ip_block->adev;

	r = amdgpu_jpeg_suspend(adev);
	if (r)
		return r;

	amdgpu_jpeg_sysfs_reset_mask_fini(adev);

	r = amdgpu_jpeg_sw_fini(adev);

	return r;
}

/**
 * jpeg_v2_5_hw_init - start and test JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 */
static int jpeg_v2_5_hw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	struct amdgpu_ring *ring;
	int i, r;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ring = adev->jpeg.inst[i].ring_dec;
		adev->nbio.funcs->vcn_doorbell_range(adev, ring->use_doorbell,
			(adev->doorbell_index.vcn.vcn_ring0_1 << 1) + 8 * i, i);

		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

/**
 * jpeg_v2_5_hw_fini - stop the hardware block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Stop the JPEG block, mark ring as not ready any more
 */
static int jpeg_v2_5_hw_fini(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i;

	cancel_delayed_work_sync(&adev->jpeg.idle_work);

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		if (adev->jpeg.cur_state != AMD_PG_STATE_GATE &&
		      RREG32_SOC15(JPEG, i, mmUVD_JRBC_STATUS))
			jpeg_v2_5_set_powergating_state(ip_block, AMD_PG_STATE_GATE);

		if (amdgpu_ras_is_supported(adev, AMDGPU_RAS_BLOCK__JPEG))
			amdgpu_irq_put(adev, &adev->jpeg.inst[i].ras_poison_irq, 0);
	}

	return 0;
}

/**
 * jpeg_v2_5_suspend - suspend JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * HW fini and suspend JPEG block
 */
static int jpeg_v2_5_suspend(struct amdgpu_ip_block *ip_block)
{
	int r;

	r = jpeg_v2_5_hw_fini(ip_block);
	if (r)
		return r;

	r = amdgpu_jpeg_suspend(ip_block->adev);

	return r;
}

/**
 * jpeg_v2_5_resume - resume JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Resume firmware and hw init JPEG block
 */
static int jpeg_v2_5_resume(struct amdgpu_ip_block *ip_block)
{
	int r;

	r = amdgpu_jpeg_resume(ip_block->adev);
	if (r)
		return r;

	r = jpeg_v2_5_hw_init(ip_block);

	return r;
}

static void jpeg_v2_5_disable_clock_gating(struct amdgpu_device *adev, int inst)
{
	uint32_t data;

	data = RREG32_SOC15(JPEG, inst, mmJPEG_CGC_CTRL);
	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG)
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	else
		data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15(JPEG, inst, mmJPEG_CGC_CTRL, data);

	data = RREG32_SOC15(JPEG, inst, mmJPEG_CGC_GATE);
	data &= ~(JPEG_CGC_GATE__JPEG_DEC_MASK
		| JPEG_CGC_GATE__JPEG2_DEC_MASK
		| JPEG_CGC_GATE__JMCIF_MASK
		| JPEG_CGC_GATE__JRBBM_MASK);
	WREG32_SOC15(JPEG, inst, mmJPEG_CGC_GATE, data);

	data = RREG32_SOC15(JPEG, inst, mmJPEG_CGC_CTRL);
	data &= ~(JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK
		| JPEG_CGC_CTRL__JPEG2_DEC_MODE_MASK
		| JPEG_CGC_CTRL__JMCIF_MODE_MASK
		| JPEG_CGC_CTRL__JRBBM_MODE_MASK);
	WREG32_SOC15(JPEG, inst, mmJPEG_CGC_CTRL, data);
}

static void jpeg_v2_5_enable_clock_gating(struct amdgpu_device *adev, int inst)
{
	uint32_t data;

	data = RREG32_SOC15(JPEG, inst, mmJPEG_CGC_GATE);
	data |= (JPEG_CGC_GATE__JPEG_DEC_MASK
		|JPEG_CGC_GATE__JPEG2_DEC_MASK
		|JPEG_CGC_GATE__JPEG_ENC_MASK
		|JPEG_CGC_GATE__JMCIF_MASK
		|JPEG_CGC_GATE__JRBBM_MASK);
	WREG32_SOC15(JPEG, inst, mmJPEG_CGC_GATE, data);
}

static void jpeg_v2_5_start_inst(struct amdgpu_device *adev, int i)
{
	struct amdgpu_ring *ring = adev->jpeg.inst[i].ring_dec;
	/* disable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, i, mmUVD_JPEG_POWER_STATUS), 0,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);

	/* JPEG disable CGC */
	jpeg_v2_5_disable_clock_gating(adev, i);

	/* MJPEG global tiling registers */
	WREG32_SOC15(JPEG, i, mmJPEG_DEC_GFX8_ADDR_CONFIG,
		adev->gfx.config.gb_addr_config);
	WREG32_SOC15(JPEG, i, mmJPEG_DEC_GFX10_ADDR_CONFIG,
		adev->gfx.config.gb_addr_config);

	/* enable JMI channel */
	WREG32_P(SOC15_REG_OFFSET(JPEG, i, mmUVD_JMI_CNTL), 0,
		~UVD_JMI_CNTL__SOFT_RESET_MASK);

	/* enable System Interrupt for JRBC */
	WREG32_P(SOC15_REG_OFFSET(JPEG, i, mmJPEG_SYS_INT_EN),
		JPEG_SYS_INT_EN__DJRBC_MASK,
		~JPEG_SYS_INT_EN__DJRBC_MASK);

	WREG32_SOC15(JPEG, i, mmUVD_LMI_JRBC_RB_VMID, 0);
	WREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_CNTL, (0x00000001L | 0x00000002L));
	WREG32_SOC15(JPEG, i, mmUVD_LMI_JRBC_RB_64BIT_BAR_LOW,
		lower_32_bits(ring->gpu_addr));
	WREG32_SOC15(JPEG, i, mmUVD_LMI_JRBC_RB_64BIT_BAR_HIGH,
		upper_32_bits(ring->gpu_addr));
	WREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_RPTR, 0);
	WREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_WPTR, 0);
	WREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_CNTL, 0x00000002L);
	WREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_SIZE, ring->ring_size / 4);
	ring->wptr = RREG32_SOC15(JPEG, i, mmUVD_JRBC_RB_WPTR);
}

/**
 * jpeg_v2_5_start - start JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * Setup and start the JPEG block
 */
static int jpeg_v2_5_start(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;
		jpeg_v2_5_start_inst(adev, i);

	}

	return 0;
}

static void jpeg_v2_5_stop_inst(struct amdgpu_device *adev, int i)
{
	/* reset JMI */
	WREG32_P(SOC15_REG_OFFSET(JPEG, i, mmUVD_JMI_CNTL),
		UVD_JMI_CNTL__SOFT_RESET_MASK,
		~UVD_JMI_CNTL__SOFT_RESET_MASK);

	jpeg_v2_5_enable_clock_gating(adev, i);

	/* enable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, i, mmUVD_JPEG_POWER_STATUS),
		UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);
}

/**
 * jpeg_v2_5_stop - stop JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * stop the JPEG block
 */
static int jpeg_v2_5_stop(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;
		jpeg_v2_5_stop_inst(adev, i);
	}

	return 0;
}

/**
 * jpeg_v2_5_dec_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint64_t jpeg_v2_5_dec_ring_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	return RREG32_SOC15(JPEG, ring->me, mmUVD_JRBC_RB_RPTR);
}

/**
 * jpeg_v2_5_dec_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint64_t jpeg_v2_5_dec_ring_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell)
		return *ring->wptr_cpu_addr;
	else
		return RREG32_SOC15(JPEG, ring->me, mmUVD_JRBC_RB_WPTR);
}

/**
 * jpeg_v2_5_dec_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void jpeg_v2_5_dec_ring_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell) {
		*ring->wptr_cpu_addr = lower_32_bits(ring->wptr);
		WDOORBELL32(ring->doorbell_index, lower_32_bits(ring->wptr));
	} else {
		WREG32_SOC15(JPEG, ring->me, mmUVD_JRBC_RB_WPTR, lower_32_bits(ring->wptr));
	}
}

/**
 * jpeg_v2_6_dec_ring_insert_start - insert a start command
 *
 * @ring: amdgpu_ring pointer
 *
 * Write a start command to the ring.
 */
static void jpeg_v2_6_dec_ring_insert_start(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x6aa04); /* PCTL0_MMHUB_DEEPSLEEP_IB */

	amdgpu_ring_write(ring, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x80000000 | (1 << (ring->me * 2 + 14)));
}

/**
 * jpeg_v2_6_dec_ring_insert_end - insert a end command
 *
 * @ring: amdgpu_ring pointer
 *
 * Write a end command to the ring.
 */
static void jpeg_v2_6_dec_ring_insert_end(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKETJ(mmUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x6aa04); /* PCTL0_MMHUB_DEEPSLEEP_IB */

	amdgpu_ring_write(ring, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, (1 << (ring->me * 2 + 14)));
}

static bool jpeg_v2_5_is_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i, ret = 1;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ret &= (((RREG32_SOC15(JPEG, i, mmUVD_JRBC_STATUS) &
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK) ==
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK));
	}

	return ret;
}

static int jpeg_v2_5_wait_for_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i, ret;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ret = SOC15_WAIT_ON_RREG(JPEG, i, mmUVD_JRBC_STATUS,
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK,
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK);
		if (ret)
			return ret;
	}

	return 0;
}

static int jpeg_v2_5_set_clockgating_state(struct amdgpu_ip_block *ip_block,
					  enum amd_clockgating_state state)
{
	struct amdgpu_device *adev = ip_block->adev;
	bool enable = (state == AMD_CG_STATE_GATE);
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		if (enable) {
			if (!jpeg_v2_5_is_idle(ip_block))
				return -EBUSY;
			jpeg_v2_5_enable_clock_gating(adev, i);
		} else {
			jpeg_v2_5_disable_clock_gating(adev, i);
		}
	}

	return 0;
}

static int jpeg_v2_5_set_powergating_state(struct amdgpu_ip_block *ip_block,
					  enum amd_powergating_state state)
{
	struct amdgpu_device *adev = ip_block->adev;
	int ret;

	if (state == adev->jpeg.cur_state)
		return 0;

	if (state == AMD_PG_STATE_GATE)
		ret = jpeg_v2_5_stop(adev);
	else
		ret = jpeg_v2_5_start(adev);

	if (!ret)
		adev->jpeg.cur_state = state;

	return ret;
}

static int jpeg_v2_5_set_interrupt_state(struct amdgpu_device *adev,
					struct amdgpu_irq_src *source,
					unsigned type,
					enum amdgpu_interrupt_state state)
{
	return 0;
}

static int jpeg_v2_6_set_ras_interrupt_state(struct amdgpu_device *adev,
					struct amdgpu_irq_src *source,
					unsigned int type,
					enum amdgpu_interrupt_state state)
{
	return 0;
}

static int jpeg_v2_5_process_interrupt(struct amdgpu_device *adev,
				      struct amdgpu_irq_src *source,
				      struct amdgpu_iv_entry *entry)
{
	uint32_t ip_instance;

	switch (entry->client_id) {
	case SOC15_IH_CLIENTID_VCN:
		ip_instance = 0;
		break;
	case SOC15_IH_CLIENTID_VCN1:
		ip_instance = 1;
		break;
	default:
		DRM_ERROR("Unhandled client id: %d\n", entry->client_id);
		return 0;
	}

	DRM_DEBUG("IH: JPEG TRAP\n");

	switch (entry->src_id) {
	case VCN_2_0__SRCID__JPEG_DECODE:
		amdgpu_fence_process(adev->jpeg.inst[ip_instance].ring_dec);
		break;
	default:
		DRM_ERROR("Unhandled interrupt: %d %d\n",
			  entry->src_id, entry->src_data[0]);
		break;
	}

	return 0;
}

static int jpeg_v2_5_ring_reset(struct amdgpu_ring *ring,
				unsigned int vmid,
				struct amdgpu_fence *timedout_fence)
{
	amdgpu_ring_reset_helper_begin(ring, timedout_fence);
	jpeg_v2_5_stop_inst(ring->adev, ring->me);
	jpeg_v2_5_start_inst(ring->adev, ring->me);
	return amdgpu_ring_reset_helper_end(ring, timedout_fence);
}

static const struct amd_ip_funcs jpeg_v2_5_ip_funcs = {
	.name = "jpeg_v2_5",
	.early_init = jpeg_v2_5_early_init,
	.sw_init = jpeg_v2_5_sw_init,
	.sw_fini = jpeg_v2_5_sw_fini,
	.hw_init = jpeg_v2_5_hw_init,
	.hw_fini = jpeg_v2_5_hw_fini,
	.suspend = jpeg_v2_5_suspend,
	.resume = jpeg_v2_5_resume,
	.is_idle = jpeg_v2_5_is_idle,
	.wait_for_idle = jpeg_v2_5_wait_for_idle,
	.set_clockgating_state = jpeg_v2_5_set_clockgating_state,
	.set_powergating_state = jpeg_v2_5_set_powergating_state,
	.dump_ip_state = amdgpu_jpeg_dump_ip_state,
	.print_ip_state = amdgpu_jpeg_print_ip_state,
};

static const struct amd_ip_funcs jpeg_v2_6_ip_funcs = {
	.name = "jpeg_v2_6",
	.early_init = jpeg_v2_5_early_init,
	.sw_init = jpeg_v2_5_sw_init,
	.sw_fini = jpeg_v2_5_sw_fini,
	.hw_init = jpeg_v2_5_hw_init,
	.hw_fini = jpeg_v2_5_hw_fini,
	.suspend = jpeg_v2_5_suspend,
	.resume = jpeg_v2_5_resume,
	.is_idle = jpeg_v2_5_is_idle,
	.wait_for_idle = jpeg_v2_5_wait_for_idle,
	.set_clockgating_state = jpeg_v2_5_set_clockgating_state,
	.set_powergating_state = jpeg_v2_5_set_powergating_state,
	.dump_ip_state = amdgpu_jpeg_dump_ip_state,
	.print_ip_state = amdgpu_jpeg_print_ip_state,
};

static const struct amdgpu_ring_funcs jpeg_v2_5_dec_ring_vm_funcs = {
	.type = AMDGPU_RING_TYPE_VCN_JPEG,
	.align_mask = 0xf,
	.get_rptr = jpeg_v2_5_dec_ring_get_rptr,
	.get_wptr = jpeg_v2_5_dec_ring_get_wptr,
	.set_wptr = jpeg_v2_5_dec_ring_set_wptr,
	.parse_cs = jpeg_v2_dec_ring_parse_cs,
	.emit_frame_size =
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 6 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 8 +
		8 + /* jpeg_v2_5_dec_ring_emit_vm_flush */
		18 + 18 + /* jpeg_v2_5_dec_ring_emit_fence x2 vm fence */
		8 + 16,
	.emit_ib_size = 22, /* jpeg_v2_5_dec_ring_emit_ib */
	.emit_ib = jpeg_v2_0_dec_ring_emit_ib,
	.emit_fence = jpeg_v2_0_dec_ring_emit_fence,
	.emit_vm_flush = jpeg_v2_0_dec_ring_emit_vm_flush,
	.test_ring = amdgpu_jpeg_dec_ring_test_ring,
	.test_ib = amdgpu_jpeg_dec_ring_test_ib,
	.insert_nop = jpeg_v2_0_dec_ring_nop,
	.insert_start = jpeg_v2_0_dec_ring_insert_start,
	.insert_end = jpeg_v2_0_dec_ring_insert_end,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.begin_use = amdgpu_jpeg_ring_begin_use,
	.end_use = amdgpu_jpeg_ring_end_use,
	.emit_wreg = jpeg_v2_0_dec_ring_emit_wreg,
	.emit_reg_wait = jpeg_v2_0_dec_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = amdgpu_ring_emit_reg_write_reg_wait_helper,
	.reset = jpeg_v2_5_ring_reset,
};

static const struct amdgpu_ring_funcs jpeg_v2_6_dec_ring_vm_funcs = {
	.type = AMDGPU_RING_TYPE_VCN_JPEG,
	.align_mask = 0xf,
	.get_rptr = jpeg_v2_5_dec_ring_get_rptr,
	.get_wptr = jpeg_v2_5_dec_ring_get_wptr,
	.set_wptr = jpeg_v2_5_dec_ring_set_wptr,
	.parse_cs = jpeg_v2_dec_ring_parse_cs,
	.emit_frame_size =
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 6 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 8 +
		8 + /* jpeg_v2_5_dec_ring_emit_vm_flush */
		18 + 18 + /* jpeg_v2_5_dec_ring_emit_fence x2 vm fence */
		8 + 16,
	.emit_ib_size = 22, /* jpeg_v2_5_dec_ring_emit_ib */
	.emit_ib = jpeg_v2_0_dec_ring_emit_ib,
	.emit_fence = jpeg_v2_0_dec_ring_emit_fence,
	.emit_vm_flush = jpeg_v2_0_dec_ring_emit_vm_flush,
	.test_ring = amdgpu_jpeg_dec_ring_test_ring,
	.test_ib = amdgpu_jpeg_dec_ring_test_ib,
	.insert_nop = jpeg_v2_0_dec_ring_nop,
	.insert_start = jpeg_v2_6_dec_ring_insert_start,
	.insert_end = jpeg_v2_6_dec_ring_insert_end,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.begin_use = amdgpu_jpeg_ring_begin_use,
	.end_use = amdgpu_jpeg_ring_end_use,
	.emit_wreg = jpeg_v2_0_dec_ring_emit_wreg,
	.emit_reg_wait = jpeg_v2_0_dec_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = amdgpu_ring_emit_reg_write_reg_wait_helper,
	.reset = jpeg_v2_5_ring_reset,
};

static void jpeg_v2_5_set_dec_ring_funcs(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;
		if (adev->asic_type == CHIP_ARCTURUS)
			adev->jpeg.inst[i].ring_dec->funcs = &jpeg_v2_5_dec_ring_vm_funcs;
		else  /* CHIP_ALDEBARAN */
			adev->jpeg.inst[i].ring_dec->funcs = &jpeg_v2_6_dec_ring_vm_funcs;
		adev->jpeg.inst[i].ring_dec->me = i;
	}
}

static const struct amdgpu_irq_src_funcs jpeg_v2_5_irq_funcs = {
	.set = jpeg_v2_5_set_interrupt_state,
	.process = jpeg_v2_5_process_interrupt,
};

static const struct amdgpu_irq_src_funcs jpeg_v2_6_ras_irq_funcs = {
	.set = jpeg_v2_6_set_ras_interrupt_state,
	.process = amdgpu_jpeg_process_poison_irq,
};

static void jpeg_v2_5_set_irq_funcs(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		adev->jpeg.inst[i].irq.num_types = 1;
		adev->jpeg.inst[i].irq.funcs = &jpeg_v2_5_irq_funcs;

		adev->jpeg.inst[i].ras_poison_irq.num_types = 1;
		adev->jpeg.inst[i].ras_poison_irq.funcs = &jpeg_v2_6_ras_irq_funcs;
	}
}

const struct amdgpu_ip_block_version jpeg_v2_5_ip_block = {
		.type = AMD_IP_BLOCK_TYPE_JPEG,
		.major = 2,
		.minor = 5,
		.rev = 0,
		.funcs = &jpeg_v2_5_ip_funcs,
};

const struct amdgpu_ip_block_version jpeg_v2_6_ip_block = {
		.type = AMD_IP_BLOCK_TYPE_JPEG,
		.major = 2,
		.minor = 6,
		.rev = 0,
		.funcs = &jpeg_v2_6_ip_funcs,
};

static uint32_t jpeg_v2_6_query_poison_by_instance(struct amdgpu_device *adev,
		uint32_t instance, uint32_t sub_block)
{
	uint32_t poison_stat = 0, reg_value = 0;

	switch (sub_block) {
	case AMDGPU_JPEG_V2_6_JPEG0:
		reg_value = RREG32_SOC15(JPEG, instance, mmUVD_RAS_JPEG0_STATUS);
		poison_stat = REG_GET_FIELD(reg_value, UVD_RAS_JPEG0_STATUS, POISONED_PF);
		break;
	case AMDGPU_JPEG_V2_6_JPEG1:
		reg_value = RREG32_SOC15(JPEG, instance, mmUVD_RAS_JPEG1_STATUS);
		poison_stat = REG_GET_FIELD(reg_value, UVD_RAS_JPEG1_STATUS, POISONED_PF);
		break;
	default:
		break;
	}

	if (poison_stat)
		dev_info(adev->dev, "Poison detected in JPEG%d sub_block%d\n",
			instance, sub_block);

	return poison_stat;
}

static bool jpeg_v2_6_query_ras_poison_status(struct amdgpu_device *adev)
{
	uint32_t inst = 0, sub = 0, poison_stat = 0;

	for (inst = 0; inst < adev->jpeg.num_jpeg_inst; inst++)
		for (sub = 0; sub < AMDGPU_JPEG_V2_6_MAX_SUB_BLOCK; sub++)
			poison_stat +=
			jpeg_v2_6_query_poison_by_instance(adev, inst, sub);

	return !!poison_stat;
}

const struct amdgpu_ras_block_hw_ops jpeg_v2_6_ras_hw_ops = {
	.query_poison_status = jpeg_v2_6_query_ras_poison_status,
};

static struct amdgpu_jpeg_ras jpeg_v2_6_ras = {
	.ras_block = {
		.hw_ops = &jpeg_v2_6_ras_hw_ops,
		.ras_late_init = amdgpu_jpeg_ras_late_init,
	},
};

static void jpeg_v2_5_set_ras_funcs(struct amdgpu_device *adev)
{
	switch (amdgpu_ip_version(adev, JPEG_HWIP, 0)) {
	case IP_VERSION(2, 6, 0):
		adev->jpeg.ras = &jpeg_v2_6_ras;
		break;
	default:
		break;
	}
}
