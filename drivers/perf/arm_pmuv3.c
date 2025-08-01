// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARMv8 PMUv3 Performance Events handling code.
 *
 * Copyright (C) 2012 ARM Limited
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This code is based heavily on the ARMv7 perf event code.
 */

#include <asm/irq_regs.h>
#include <asm/perf_event.h>
#include <asm/virt.h>

#include <clocksource/arm_arch_timer.h>

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/clocksource.h>
#include <linux/of.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>
#include <linux/platform_device.h>
#include <linux/sched_clock.h>
#include <linux/smp.h>
#include <linux/nmi.h>

#include "arm_brbe.h"

/* ARMv8 Cortex-A53 specific event types. */
#define ARMV8_A53_PERFCTR_PREF_LINEFILL				0xC2

/* ARMv8 Cavium ThunderX specific event types. */
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_MISS_ST			0xE9
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_ACCESS		0xEA
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_MISS		0xEB
#define ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_ACCESS		0xEC
#define ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_MISS		0xED

/*
 * ARMv8 Architectural defined events, not all of these may
 * be supported on any given implementation. Unsupported events will
 * be disabled at run-time based on the PMCEID registers.
 */
static const unsigned armv8_pmuv3_perf_map[PERF_COUNT_HW_MAX] = {
	PERF_MAP_ALL_UNSUPPORTED,
	[PERF_COUNT_HW_CPU_CYCLES]		= ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	[PERF_COUNT_HW_INSTRUCTIONS]		= ARMV8_PMUV3_PERFCTR_INST_RETIRED,
	[PERF_COUNT_HW_CACHE_REFERENCES]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE,
	[PERF_COUNT_HW_CACHE_MISSES]		= ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL,
	[PERF_COUNT_HW_BRANCH_MISSES]		= ARMV8_PMUV3_PERFCTR_BR_MIS_PRED,
	[PERF_COUNT_HW_BUS_CYCLES]		= ARMV8_PMUV3_PERFCTR_BUS_CYCLES,
	[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND]	= ARMV8_PMUV3_PERFCTR_STALL_FRONTEND,
	[PERF_COUNT_HW_STALLED_CYCLES_BACKEND]	= ARMV8_PMUV3_PERFCTR_STALL_BACKEND,
};

static const unsigned armv8_pmuv3_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
						[PERF_COUNT_HW_CACHE_OP_MAX]
						[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL,

	[C(L1I)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1I_CACHE,
	[C(L1I)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1I_CACHE_REFILL,

	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1D_TLB_REFILL,
	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1D_TLB,

	[C(ITLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1I_TLB_REFILL,
	[C(ITLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1I_TLB,

	[C(LL)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_LL_CACHE_MISS_RD,
	[C(LL)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_LL_CACHE_RD,

	[C(BPU)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_BR_PRED,
	[C(BPU)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_BR_MIS_PRED,
};

static const unsigned armv8_a53_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_A53_PERFCTR_PREF_LINEFILL,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static const unsigned armv8_a57_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_WR,

	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static const unsigned armv8_a73_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
};

static const unsigned armv8_thunder_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
						   [PERF_COUNT_HW_CACHE_OP_MAX]
						   [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_THUNDER_PERFCTR_L1D_CACHE_MISS_ST,
	[C(L1D)][C(OP_PREFETCH)][C(RESULT_ACCESS)] = ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_ACCESS,
	[C(L1D)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_MISS,

	[C(L1I)][C(OP_PREFETCH)][C(RESULT_ACCESS)] = ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_ACCESS,
	[C(L1I)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_MISS,

	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_RD,
	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_L1D_TLB_WR,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,
};

static const unsigned armv8_vulcan_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_WR,

	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_L1D_TLB_WR,
	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static ssize_t
armv8pmu_events_sysfs_show(struct device *dev,
			   struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=0x%04llx\n", pmu_attr->id);
}

#define ARMV8_EVENT_ATTR(name, config)						\
	PMU_EVENT_ATTR_ID(name, armv8pmu_events_sysfs_show, config)

static struct attribute *armv8_pmuv3_event_attrs[] = {
	/*
	 * Don't expose the sw_incr event in /sys. It's not usable as writes to
	 * PMSWINC_EL0 will trap as PMUSERENR.{SW,EN}=={0,0} and event rotation
	 * means we don't have a fixed event<->counter relationship regardless.
	 */
	ARMV8_EVENT_ATTR(l1i_cache_refill, ARMV8_PMUV3_PERFCTR_L1I_CACHE_REFILL),
	ARMV8_EVENT_ATTR(l1i_tlb_refill, ARMV8_PMUV3_PERFCTR_L1I_TLB_REFILL),
	ARMV8_EVENT_ATTR(l1d_cache_refill, ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL),
	ARMV8_EVENT_ATTR(l1d_cache, ARMV8_PMUV3_PERFCTR_L1D_CACHE),
	ARMV8_EVENT_ATTR(l1d_tlb_refill, ARMV8_PMUV3_PERFCTR_L1D_TLB_REFILL),
	ARMV8_EVENT_ATTR(ld_retired, ARMV8_PMUV3_PERFCTR_LD_RETIRED),
	ARMV8_EVENT_ATTR(st_retired, ARMV8_PMUV3_PERFCTR_ST_RETIRED),
	ARMV8_EVENT_ATTR(inst_retired, ARMV8_PMUV3_PERFCTR_INST_RETIRED),
	ARMV8_EVENT_ATTR(exc_taken, ARMV8_PMUV3_PERFCTR_EXC_TAKEN),
	ARMV8_EVENT_ATTR(exc_return, ARMV8_PMUV3_PERFCTR_EXC_RETURN),
	ARMV8_EVENT_ATTR(cid_write_retired, ARMV8_PMUV3_PERFCTR_CID_WRITE_RETIRED),
	ARMV8_EVENT_ATTR(pc_write_retired, ARMV8_PMUV3_PERFCTR_PC_WRITE_RETIRED),
	ARMV8_EVENT_ATTR(br_immed_retired, ARMV8_PMUV3_PERFCTR_BR_IMMED_RETIRED),
	ARMV8_EVENT_ATTR(br_return_retired, ARMV8_PMUV3_PERFCTR_BR_RETURN_RETIRED),
	ARMV8_EVENT_ATTR(unaligned_ldst_retired, ARMV8_PMUV3_PERFCTR_UNALIGNED_LDST_RETIRED),
	ARMV8_EVENT_ATTR(br_mis_pred, ARMV8_PMUV3_PERFCTR_BR_MIS_PRED),
	ARMV8_EVENT_ATTR(cpu_cycles, ARMV8_PMUV3_PERFCTR_CPU_CYCLES),
	ARMV8_EVENT_ATTR(br_pred, ARMV8_PMUV3_PERFCTR_BR_PRED),
	ARMV8_EVENT_ATTR(mem_access, ARMV8_PMUV3_PERFCTR_MEM_ACCESS),
	ARMV8_EVENT_ATTR(l1i_cache, ARMV8_PMUV3_PERFCTR_L1I_CACHE),
	ARMV8_EVENT_ATTR(l1d_cache_wb, ARMV8_PMUV3_PERFCTR_L1D_CACHE_WB),
	ARMV8_EVENT_ATTR(l2d_cache, ARMV8_PMUV3_PERFCTR_L2D_CACHE),
	ARMV8_EVENT_ATTR(l2d_cache_refill, ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL),
	ARMV8_EVENT_ATTR(l2d_cache_wb, ARMV8_PMUV3_PERFCTR_L2D_CACHE_WB),
	ARMV8_EVENT_ATTR(bus_access, ARMV8_PMUV3_PERFCTR_BUS_ACCESS),
	ARMV8_EVENT_ATTR(memory_error, ARMV8_PMUV3_PERFCTR_MEMORY_ERROR),
	ARMV8_EVENT_ATTR(inst_spec, ARMV8_PMUV3_PERFCTR_INST_SPEC),
	ARMV8_EVENT_ATTR(ttbr_write_retired, ARMV8_PMUV3_PERFCTR_TTBR_WRITE_RETIRED),
	ARMV8_EVENT_ATTR(bus_cycles, ARMV8_PMUV3_PERFCTR_BUS_CYCLES),
	/* Don't expose the chain event in /sys, since it's useless in isolation */
	ARMV8_EVENT_ATTR(l1d_cache_allocate, ARMV8_PMUV3_PERFCTR_L1D_CACHE_ALLOCATE),
	ARMV8_EVENT_ATTR(l2d_cache_allocate, ARMV8_PMUV3_PERFCTR_L2D_CACHE_ALLOCATE),
	ARMV8_EVENT_ATTR(br_retired, ARMV8_PMUV3_PERFCTR_BR_RETIRED),
	ARMV8_EVENT_ATTR(br_mis_pred_retired, ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED),
	ARMV8_EVENT_ATTR(stall_frontend, ARMV8_PMUV3_PERFCTR_STALL_FRONTEND),
	ARMV8_EVENT_ATTR(stall_backend, ARMV8_PMUV3_PERFCTR_STALL_BACKEND),
	ARMV8_EVENT_ATTR(l1d_tlb, ARMV8_PMUV3_PERFCTR_L1D_TLB),
	ARMV8_EVENT_ATTR(l1i_tlb, ARMV8_PMUV3_PERFCTR_L1I_TLB),
	ARMV8_EVENT_ATTR(l2i_cache, ARMV8_PMUV3_PERFCTR_L2I_CACHE),
	ARMV8_EVENT_ATTR(l2i_cache_refill, ARMV8_PMUV3_PERFCTR_L2I_CACHE_REFILL),
	ARMV8_EVENT_ATTR(l3d_cache_allocate, ARMV8_PMUV3_PERFCTR_L3D_CACHE_ALLOCATE),
	ARMV8_EVENT_ATTR(l3d_cache_refill, ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL),
	ARMV8_EVENT_ATTR(l3d_cache, ARMV8_PMUV3_PERFCTR_L3D_CACHE),
	ARMV8_EVENT_ATTR(l3d_cache_wb, ARMV8_PMUV3_PERFCTR_L3D_CACHE_WB),
	ARMV8_EVENT_ATTR(l2d_tlb_refill, ARMV8_PMUV3_PERFCTR_L2D_TLB_REFILL),
	ARMV8_EVENT_ATTR(l2i_tlb_refill, ARMV8_PMUV3_PERFCTR_L2I_TLB_REFILL),
	ARMV8_EVENT_ATTR(l2d_tlb, ARMV8_PMUV3_PERFCTR_L2D_TLB),
	ARMV8_EVENT_ATTR(l2i_tlb, ARMV8_PMUV3_PERFCTR_L2I_TLB),
	ARMV8_EVENT_ATTR(remote_access, ARMV8_PMUV3_PERFCTR_REMOTE_ACCESS),
	ARMV8_EVENT_ATTR(ll_cache, ARMV8_PMUV3_PERFCTR_LL_CACHE),
	ARMV8_EVENT_ATTR(ll_cache_miss, ARMV8_PMUV3_PERFCTR_LL_CACHE_MISS),
	ARMV8_EVENT_ATTR(dtlb_walk, ARMV8_PMUV3_PERFCTR_DTLB_WALK),
	ARMV8_EVENT_ATTR(itlb_walk, ARMV8_PMUV3_PERFCTR_ITLB_WALK),
	ARMV8_EVENT_ATTR(ll_cache_rd, ARMV8_PMUV3_PERFCTR_LL_CACHE_RD),
	ARMV8_EVENT_ATTR(ll_cache_miss_rd, ARMV8_PMUV3_PERFCTR_LL_CACHE_MISS_RD),
	ARMV8_EVENT_ATTR(remote_access_rd, ARMV8_PMUV3_PERFCTR_REMOTE_ACCESS_RD),
	ARMV8_EVENT_ATTR(l1d_cache_lmiss_rd, ARMV8_PMUV3_PERFCTR_L1D_CACHE_LMISS_RD),
	ARMV8_EVENT_ATTR(op_retired, ARMV8_PMUV3_PERFCTR_OP_RETIRED),
	ARMV8_EVENT_ATTR(op_spec, ARMV8_PMUV3_PERFCTR_OP_SPEC),
	ARMV8_EVENT_ATTR(stall, ARMV8_PMUV3_PERFCTR_STALL),
	ARMV8_EVENT_ATTR(stall_slot_backend, ARMV8_PMUV3_PERFCTR_STALL_SLOT_BACKEND),
	ARMV8_EVENT_ATTR(stall_slot_frontend, ARMV8_PMUV3_PERFCTR_STALL_SLOT_FRONTEND),
	ARMV8_EVENT_ATTR(stall_slot, ARMV8_PMUV3_PERFCTR_STALL_SLOT),
	ARMV8_EVENT_ATTR(sample_pop, ARMV8_SPE_PERFCTR_SAMPLE_POP),
	ARMV8_EVENT_ATTR(sample_feed, ARMV8_SPE_PERFCTR_SAMPLE_FEED),
	ARMV8_EVENT_ATTR(sample_filtrate, ARMV8_SPE_PERFCTR_SAMPLE_FILTRATE),
	ARMV8_EVENT_ATTR(sample_collision, ARMV8_SPE_PERFCTR_SAMPLE_COLLISION),
	ARMV8_EVENT_ATTR(cnt_cycles, ARMV8_AMU_PERFCTR_CNT_CYCLES),
	ARMV8_EVENT_ATTR(stall_backend_mem, ARMV8_AMU_PERFCTR_STALL_BACKEND_MEM),
	ARMV8_EVENT_ATTR(l1i_cache_lmiss, ARMV8_PMUV3_PERFCTR_L1I_CACHE_LMISS),
	ARMV8_EVENT_ATTR(l2d_cache_lmiss_rd, ARMV8_PMUV3_PERFCTR_L2D_CACHE_LMISS_RD),
	ARMV8_EVENT_ATTR(l2i_cache_lmiss, ARMV8_PMUV3_PERFCTR_L2I_CACHE_LMISS),
	ARMV8_EVENT_ATTR(l3d_cache_lmiss_rd, ARMV8_PMUV3_PERFCTR_L3D_CACHE_LMISS_RD),
	ARMV8_EVENT_ATTR(trb_wrap, ARMV8_PMUV3_PERFCTR_TRB_WRAP),
	ARMV8_EVENT_ATTR(trb_trig, ARMV8_PMUV3_PERFCTR_TRB_TRIG),
	ARMV8_EVENT_ATTR(trcextout0, ARMV8_PMUV3_PERFCTR_TRCEXTOUT0),
	ARMV8_EVENT_ATTR(trcextout1, ARMV8_PMUV3_PERFCTR_TRCEXTOUT1),
	ARMV8_EVENT_ATTR(trcextout2, ARMV8_PMUV3_PERFCTR_TRCEXTOUT2),
	ARMV8_EVENT_ATTR(trcextout3, ARMV8_PMUV3_PERFCTR_TRCEXTOUT3),
	ARMV8_EVENT_ATTR(cti_trigout4, ARMV8_PMUV3_PERFCTR_CTI_TRIGOUT4),
	ARMV8_EVENT_ATTR(cti_trigout5, ARMV8_PMUV3_PERFCTR_CTI_TRIGOUT5),
	ARMV8_EVENT_ATTR(cti_trigout6, ARMV8_PMUV3_PERFCTR_CTI_TRIGOUT6),
	ARMV8_EVENT_ATTR(cti_trigout7, ARMV8_PMUV3_PERFCTR_CTI_TRIGOUT7),
	ARMV8_EVENT_ATTR(ldst_align_lat, ARMV8_PMUV3_PERFCTR_LDST_ALIGN_LAT),
	ARMV8_EVENT_ATTR(ld_align_lat, ARMV8_PMUV3_PERFCTR_LD_ALIGN_LAT),
	ARMV8_EVENT_ATTR(st_align_lat, ARMV8_PMUV3_PERFCTR_ST_ALIGN_LAT),
	ARMV8_EVENT_ATTR(mem_access_checked, ARMV8_MTE_PERFCTR_MEM_ACCESS_CHECKED),
	ARMV8_EVENT_ATTR(mem_access_checked_rd, ARMV8_MTE_PERFCTR_MEM_ACCESS_CHECKED_RD),
	ARMV8_EVENT_ATTR(mem_access_checked_wr, ARMV8_MTE_PERFCTR_MEM_ACCESS_CHECKED_WR),
	NULL,
};

static umode_t
armv8pmu_event_attr_is_visible(struct kobject *kobj,
			       struct attribute *attr, int unused)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr.attr);

	if (pmu_attr->id < ARMV8_PMUV3_MAX_COMMON_EVENTS &&
	    test_bit(pmu_attr->id, cpu_pmu->pmceid_bitmap))
		return attr->mode;

	if (pmu_attr->id >= ARMV8_PMUV3_EXT_COMMON_EVENT_BASE) {
		u64 id = pmu_attr->id - ARMV8_PMUV3_EXT_COMMON_EVENT_BASE;

		if (id < ARMV8_PMUV3_MAX_COMMON_EVENTS &&
		    test_bit(id, cpu_pmu->pmceid_ext_bitmap))
			return attr->mode;
	}

	return 0;
}

static const struct attribute_group armv8_pmuv3_events_attr_group = {
	.name = "events",
	.attrs = armv8_pmuv3_event_attrs,
	.is_visible = armv8pmu_event_attr_is_visible,
};

/* User ABI */
#define ATTR_CFG_FLD_event_CFG		config
#define ATTR_CFG_FLD_event_LO		0
#define ATTR_CFG_FLD_event_HI		15
#define ATTR_CFG_FLD_long_CFG		config1
#define ATTR_CFG_FLD_long_LO		0
#define ATTR_CFG_FLD_long_HI		0
#define ATTR_CFG_FLD_rdpmc_CFG		config1
#define ATTR_CFG_FLD_rdpmc_LO		1
#define ATTR_CFG_FLD_rdpmc_HI		1
#define ATTR_CFG_FLD_threshold_count_CFG	config1 /* PMEVTYPER.TC[0] */
#define ATTR_CFG_FLD_threshold_count_LO		2
#define ATTR_CFG_FLD_threshold_count_HI		2
#define ATTR_CFG_FLD_threshold_compare_CFG	config1 /* PMEVTYPER.TC[2:1] */
#define ATTR_CFG_FLD_threshold_compare_LO	3
#define ATTR_CFG_FLD_threshold_compare_HI	4
#define ATTR_CFG_FLD_threshold_CFG		config1 /* PMEVTYPER.TH */
#define ATTR_CFG_FLD_threshold_LO		5
#define ATTR_CFG_FLD_threshold_HI		16

GEN_PMU_FORMAT_ATTR(event);
GEN_PMU_FORMAT_ATTR(long);
GEN_PMU_FORMAT_ATTR(rdpmc);
GEN_PMU_FORMAT_ATTR(threshold_count);
GEN_PMU_FORMAT_ATTR(threshold_compare);
GEN_PMU_FORMAT_ATTR(threshold);

static int sysctl_perf_user_access __read_mostly;

static bool armv8pmu_event_is_64bit(struct perf_event *event)
{
	return ATTR_CFG_GET_FLD(&event->attr, long);
}

static bool armv8pmu_event_want_user_access(struct perf_event *event)
{
	return ATTR_CFG_GET_FLD(&event->attr, rdpmc);
}

static u32 armv8pmu_event_get_threshold(struct perf_event_attr *attr)
{
	return ATTR_CFG_GET_FLD(attr, threshold);
}

static u8 armv8pmu_event_threshold_control(struct perf_event_attr *attr)
{
	u8 th_compare = ATTR_CFG_GET_FLD(attr, threshold_compare);
	u8 th_count = ATTR_CFG_GET_FLD(attr, threshold_count);

	/*
	 * The count bit is always the bottom bit of the full control field, and
	 * the comparison is the upper two bits, but it's not explicitly
	 * labelled in the Arm ARM. For the Perf interface we split it into two
	 * fields, so reconstruct it here.
	 */
	return (th_compare << 1) | th_count;
}

static struct attribute *armv8_pmuv3_format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_long.attr,
	&format_attr_rdpmc.attr,
	&format_attr_threshold.attr,
	&format_attr_threshold_compare.attr,
	&format_attr_threshold_count.attr,
	NULL,
};

static const struct attribute_group armv8_pmuv3_format_attr_group = {
	.name = "format",
	.attrs = armv8_pmuv3_format_attrs,
};

static ssize_t slots_show(struct device *dev, struct device_attribute *attr,
			  char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);
	u32 slots = FIELD_GET(ARMV8_PMU_SLOTS, cpu_pmu->reg_pmmir);

	return sysfs_emit(page, "0x%08x\n", slots);
}

static DEVICE_ATTR_RO(slots);

static ssize_t bus_slots_show(struct device *dev, struct device_attribute *attr,
			      char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);
	u32 bus_slots = FIELD_GET(ARMV8_PMU_BUS_SLOTS, cpu_pmu->reg_pmmir);

	return sysfs_emit(page, "0x%08x\n", bus_slots);
}

static DEVICE_ATTR_RO(bus_slots);

static ssize_t bus_width_show(struct device *dev, struct device_attribute *attr,
			      char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);
	u32 bus_width = FIELD_GET(ARMV8_PMU_BUS_WIDTH, cpu_pmu->reg_pmmir);
	u32 val = 0;

	/* Encoded as Log2(number of bytes), plus one */
	if (bus_width > 2 && bus_width < 13)
		val = 1 << (bus_width - 1);

	return sysfs_emit(page, "0x%08x\n", val);
}

static DEVICE_ATTR_RO(bus_width);

static u32 threshold_max(struct arm_pmu *cpu_pmu)
{
	/*
	 * PMMIR.THWIDTH is readable and non-zero on aarch32, but it would be
	 * impossible to write the threshold in the upper 32 bits of PMEVTYPER.
	 */
	if (IS_ENABLED(CONFIG_ARM))
		return 0;

	/*
	 * The largest value that can be written to PMEVTYPER<n>_EL0.TH is
	 * (2 ^ PMMIR.THWIDTH) - 1.
	 */
	return (1 << FIELD_GET(ARMV8_PMU_THWIDTH, cpu_pmu->reg_pmmir)) - 1;
}

static ssize_t threshold_max_show(struct device *dev,
				  struct device_attribute *attr, char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);

	return sysfs_emit(page, "0x%08x\n", threshold_max(cpu_pmu));
}

static DEVICE_ATTR_RO(threshold_max);

static ssize_t branches_show(struct device *dev,
			     struct device_attribute *attr, char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);

	return sysfs_emit(page, "%d\n", brbe_num_branch_records(cpu_pmu));
}

static DEVICE_ATTR_RO(branches);

static struct attribute *armv8_pmuv3_caps_attrs[] = {
	&dev_attr_branches.attr,
	&dev_attr_slots.attr,
	&dev_attr_bus_slots.attr,
	&dev_attr_bus_width.attr,
	&dev_attr_threshold_max.attr,
	NULL,
};

static umode_t caps_is_visible(struct kobject *kobj, struct attribute *attr, int i)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);

	if (i == 0)
		return brbe_num_branch_records(cpu_pmu) ? attr->mode : 0;

	return attr->mode;
}

static const struct attribute_group armv8_pmuv3_caps_attr_group = {
	.name = "caps",
	.attrs = armv8_pmuv3_caps_attrs,
	.is_visible = caps_is_visible,
};

/*
 * We unconditionally enable ARMv8.5-PMU long event counter support
 * (64-bit events) where supported. Indicate if this arm_pmu has long
 * event counter support.
 *
 * On AArch32, long counters make no sense (you can't access the top
 * bits), so we only enable this on AArch64.
 */
static bool armv8pmu_has_long_event(struct arm_pmu *cpu_pmu)
{
	return (IS_ENABLED(CONFIG_ARM64) && is_pmuv3p5(cpu_pmu->pmuver));
}

static bool armv8pmu_event_has_user_read(struct perf_event *event)
{
	return event->hw.flags & PERF_EVENT_FLAG_USER_READ_CNT;
}

/*
 * We must chain two programmable counters for 64 bit events,
 * except when we have allocated the 64bit cycle counter (for CPU
 * cycles event) or when user space counter access is enabled.
 */
static bool armv8pmu_event_is_chained(struct perf_event *event)
{
	int idx = event->hw.idx;
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);

	return !armv8pmu_event_has_user_read(event) &&
	       armv8pmu_event_is_64bit(event) &&
	       !armv8pmu_has_long_event(cpu_pmu) &&
	       (idx < ARMV8_PMU_MAX_GENERAL_COUNTERS);
}

/*
 * ARMv8 low level PMU access
 */
static u64 armv8pmu_pmcr_read(void)
{
	return read_pmcr();
}

static void armv8pmu_pmcr_write(u64 val)
{
	val &= ARMV8_PMU_PMCR_MASK;
	isb();
	write_pmcr(val);
}

static int armv8pmu_has_overflowed(u64 pmovsr)
{
	return !!(pmovsr & ARMV8_PMU_OVERFLOWED_MASK);
}

static int armv8pmu_counter_has_overflowed(u64 pmnc, int idx)
{
	return !!(pmnc & BIT(idx));
}

static u64 armv8pmu_read_evcntr(int idx)
{
	return read_pmevcntrn(idx);
}

static u64 armv8pmu_read_hw_counter(struct perf_event *event)
{
	int idx = event->hw.idx;
	u64 val = armv8pmu_read_evcntr(idx);

	if (armv8pmu_event_is_chained(event))
		val = (val << 32) | armv8pmu_read_evcntr(idx - 1);
	return val;
}

/*
 * The cycle counter is always a 64-bit counter. When ARMV8_PMU_PMCR_LP
 * is set the event counters also become 64-bit counters. Unless the
 * user has requested a long counter (attr.config1) then we want to
 * interrupt upon 32-bit overflow - we achieve this by applying a bias.
 */
static bool armv8pmu_event_needs_bias(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	if (armv8pmu_event_is_64bit(event))
		return false;

	if (armv8pmu_has_long_event(cpu_pmu) ||
	    idx >= ARMV8_PMU_MAX_GENERAL_COUNTERS)
		return true;

	return false;
}

static u64 armv8pmu_bias_long_counter(struct perf_event *event, u64 value)
{
	if (armv8pmu_event_needs_bias(event))
		value |= GENMASK_ULL(63, 32);

	return value;
}

static u64 armv8pmu_unbias_long_counter(struct perf_event *event, u64 value)
{
	if (armv8pmu_event_needs_bias(event))
		value &= ~GENMASK_ULL(63, 32);

	return value;
}

static u64 armv8pmu_read_counter(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;
	u64 value;

	if (idx == ARMV8_PMU_CYCLE_IDX)
		value = read_pmccntr();
	else if (idx == ARMV8_PMU_INSTR_IDX)
		value = read_pmicntr();
	else
		value = armv8pmu_read_hw_counter(event);

	return  armv8pmu_unbias_long_counter(event, value);
}

static void armv8pmu_write_evcntr(int idx, u64 value)
{
	write_pmevcntrn(idx, value);
}

static void armv8pmu_write_hw_counter(struct perf_event *event,
					     u64 value)
{
	int idx = event->hw.idx;

	if (armv8pmu_event_is_chained(event)) {
		armv8pmu_write_evcntr(idx, upper_32_bits(value));
		armv8pmu_write_evcntr(idx - 1, lower_32_bits(value));
	} else {
		armv8pmu_write_evcntr(idx, value);
	}
}

static void armv8pmu_write_counter(struct perf_event *event, u64 value)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	value = armv8pmu_bias_long_counter(event, value);

	if (idx == ARMV8_PMU_CYCLE_IDX)
		write_pmccntr(value);
	else if (idx == ARMV8_PMU_INSTR_IDX)
		write_pmicntr(value);
	else
		armv8pmu_write_hw_counter(event, value);
}

static void armv8pmu_write_evtype(int idx, unsigned long val)
{
	unsigned long mask = ARMV8_PMU_EVTYPE_EVENT |
			     ARMV8_PMU_INCLUDE_EL2 |
			     ARMV8_PMU_EXCLUDE_EL0 |
			     ARMV8_PMU_EXCLUDE_EL1;

	if (IS_ENABLED(CONFIG_ARM64))
		mask |= ARMV8_PMU_EVTYPE_TC | ARMV8_PMU_EVTYPE_TH;

	val &= mask;
	write_pmevtypern(idx, val);
}

static void armv8pmu_write_event_type(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	/*
	 * For chained events, the low counter is programmed to count
	 * the event of interest and the high counter is programmed
	 * with CHAIN event code with filters set to count at all ELs.
	 */
	if (armv8pmu_event_is_chained(event)) {
		u32 chain_evt = ARMV8_PMUV3_PERFCTR_CHAIN |
				ARMV8_PMU_INCLUDE_EL2;

		armv8pmu_write_evtype(idx - 1, hwc->config_base);
		armv8pmu_write_evtype(idx, chain_evt);
	} else {
		if (idx == ARMV8_PMU_CYCLE_IDX)
			write_pmccfiltr(hwc->config_base);
		else if (idx == ARMV8_PMU_INSTR_IDX)
			write_pmicfiltr(hwc->config_base);
		else
			armv8pmu_write_evtype(idx, hwc->config_base);
	}
}

static u64 armv8pmu_event_cnten_mask(struct perf_event *event)
{
	int counter = event->hw.idx;
	u64 mask = BIT(counter);

	if (armv8pmu_event_is_chained(event))
		mask |= BIT(counter - 1);
	return mask;
}

static void armv8pmu_enable_counter(u64 mask)
{
	/*
	 * Make sure event configuration register writes are visible before we
	 * enable the counter.
	 * */
	isb();
	write_pmcntenset(mask);
}

static void armv8pmu_enable_event_counter(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;
	u64 mask = armv8pmu_event_cnten_mask(event);

	kvm_set_pmu_events(mask, attr);

	/* We rely on the hypervisor switch code to enable guest counters */
	if (!kvm_pmu_counter_deferred(attr))
		armv8pmu_enable_counter(mask);
}

static void armv8pmu_disable_counter(u64 mask)
{
	write_pmcntenclr(mask);
	/*
	 * Make sure the effects of disabling the counter are visible before we
	 * start configuring the event.
	 */
	isb();
}

static void armv8pmu_disable_event_counter(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;
	u64 mask = armv8pmu_event_cnten_mask(event);

	kvm_clr_pmu_events(mask);

	/* We rely on the hypervisor switch code to disable guest counters */
	if (!kvm_pmu_counter_deferred(attr))
		armv8pmu_disable_counter(mask);
}

static void armv8pmu_enable_intens(u64 mask)
{
	write_pmintenset(mask);
}

static void armv8pmu_enable_event_irq(struct perf_event *event)
{
	armv8pmu_enable_intens(BIT(event->hw.idx));
}

static void armv8pmu_disable_intens(u64 mask)
{
	write_pmintenclr(mask);
	isb();
	/* Clear the overflow flag in case an interrupt is pending. */
	write_pmovsclr(mask);
	isb();
}

static void armv8pmu_disable_event_irq(struct perf_event *event)
{
	armv8pmu_disable_intens(BIT(event->hw.idx));
}

static u64 armv8pmu_getreset_flags(void)
{
	u64 value;

	/* Read */
	value = read_pmovsclr();

	/* Write to clear flags */
	value &= ARMV8_PMU_OVERFLOWED_MASK;
	write_pmovsclr(value);

	return value;
}

static void update_pmuserenr(u64 val)
{
	lockdep_assert_irqs_disabled();

	/*
	 * The current PMUSERENR_EL0 value might be the value for the guest.
	 * If that's the case, have KVM keep tracking of the register value
	 * for the host EL0 so that KVM can restore it before returning to
	 * the host EL0. Otherwise, update the register now.
	 */
	if (kvm_set_pmuserenr(val))
		return;

	write_pmuserenr(val);
}

static void armv8pmu_disable_user_access(void)
{
	update_pmuserenr(0);
}

static void armv8pmu_enable_user_access(struct arm_pmu *cpu_pmu)
{
	int i;
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);

	if (is_pmuv3p9(cpu_pmu->pmuver)) {
		u64 mask = 0;
		for_each_set_bit(i, cpuc->used_mask, ARMPMU_MAX_HWEVENTS) {
			if (armv8pmu_event_has_user_read(cpuc->events[i]))
				mask |= BIT(i);
		}
		write_pmuacr(mask);
	} else {
		/* Clear any unused counters to avoid leaking their contents */
		for_each_andnot_bit(i, cpu_pmu->cntr_mask, cpuc->used_mask,
				    ARMPMU_MAX_HWEVENTS) {
			if (i == ARMV8_PMU_CYCLE_IDX)
				write_pmccntr(0);
			else if (i == ARMV8_PMU_INSTR_IDX)
				write_pmicntr(0);
			else
				armv8pmu_write_evcntr(i, 0);
		}
	}

	update_pmuserenr(ARMV8_PMU_USERENR_ER | ARMV8_PMU_USERENR_CR | ARMV8_PMU_USERENR_UEN);
}

static void armv8pmu_enable_event(struct perf_event *event)
{
	armv8pmu_write_event_type(event);
	armv8pmu_enable_event_irq(event);
	armv8pmu_enable_event_counter(event);
}

static void armv8pmu_disable_event(struct perf_event *event)
{
	armv8pmu_disable_event_counter(event);
	armv8pmu_disable_event_irq(event);
}

static void armv8pmu_start(struct arm_pmu *cpu_pmu)
{
	struct perf_event_context *ctx;
	struct pmu_hw_events *hw_events = this_cpu_ptr(cpu_pmu->hw_events);
	int nr_user = 0;

	ctx = perf_cpu_task_ctx();
	if (ctx)
		nr_user = ctx->nr_user;

	if (sysctl_perf_user_access && nr_user)
		armv8pmu_enable_user_access(cpu_pmu);
	else
		armv8pmu_disable_user_access();

	kvm_vcpu_pmu_resync_el0();

	if (hw_events->branch_users)
		brbe_enable(cpu_pmu);

	/* Enable all counters */
	armv8pmu_pmcr_write(armv8pmu_pmcr_read() | ARMV8_PMU_PMCR_E);
}

static void armv8pmu_stop(struct arm_pmu *cpu_pmu)
{
	struct pmu_hw_events *hw_events = this_cpu_ptr(cpu_pmu->hw_events);

	if (hw_events->branch_users)
		brbe_disable();

	/* Disable all counters */
	armv8pmu_pmcr_write(armv8pmu_pmcr_read() & ~ARMV8_PMU_PMCR_E);
}

static void read_branch_records(struct pmu_hw_events *cpuc,
				struct perf_event *event,
				struct perf_sample_data *data)
{
	struct perf_branch_stack *branch_stack = cpuc->branch_stack;

	brbe_read_filtered_entries(branch_stack, event);
	perf_sample_save_brstack(data, event, branch_stack, NULL);
}

static irqreturn_t armv8pmu_handle_irq(struct arm_pmu *cpu_pmu)
{
	u64 pmovsr;
	struct perf_sample_data data;
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);
	struct pt_regs *regs;
	int idx;

	/*
	 * Get and reset the IRQ flags
	 */
	pmovsr = armv8pmu_getreset_flags();

	/*
	 * Did an overflow occur?
	 */
	if (!armv8pmu_has_overflowed(pmovsr))
		return IRQ_NONE;

	/*
	 * Handle the counter(s) overflow(s)
	 */
	regs = get_irq_regs();

	/*
	 * Stop the PMU while processing the counter overflows
	 * to prevent skews in group events.
	 */
	armv8pmu_stop(cpu_pmu);
	for_each_set_bit(idx, cpu_pmu->cntr_mask, ARMPMU_MAX_HWEVENTS) {
		struct perf_event *event = cpuc->events[idx];
		struct hw_perf_event *hwc;

		/* Ignore if we don't have an event. */
		if (!event)
			continue;

		/*
		 * We have a single interrupt for all counters. Check that
		 * each counter has overflowed before we process it.
		 */
		if (!armv8pmu_counter_has_overflowed(pmovsr, idx))
			continue;

		hwc = &event->hw;
		armpmu_event_update(event);
		perf_sample_data_init(&data, 0, hwc->last_period);
		if (!armpmu_event_set_period(event))
			continue;

		if (has_branch_stack(event))
			read_branch_records(cpuc, event, &data);

		/*
		 * Perf event overflow will queue the processing of the event as
		 * an irq_work which will be taken care of in the handling of
		 * IPI_IRQ_WORK.
		 */
		perf_event_overflow(event, &data, regs);
	}
	armv8pmu_start(cpu_pmu);

	return IRQ_HANDLED;
}

static int armv8pmu_get_single_idx(struct pmu_hw_events *cpuc,
				    struct arm_pmu *cpu_pmu)
{
	int idx;

	for_each_set_bit(idx, cpu_pmu->cntr_mask, ARMV8_PMU_MAX_GENERAL_COUNTERS) {
		if (!test_and_set_bit(idx, cpuc->used_mask))
			return idx;
	}
	return -EAGAIN;
}

static int armv8pmu_get_chain_idx(struct pmu_hw_events *cpuc,
				   struct arm_pmu *cpu_pmu)
{
	int idx;

	/*
	 * Chaining requires two consecutive event counters, where
	 * the lower idx must be even.
	 */
	for_each_set_bit(idx, cpu_pmu->cntr_mask, ARMV8_PMU_MAX_GENERAL_COUNTERS) {
		if (!(idx & 0x1))
			continue;
		if (!test_and_set_bit(idx, cpuc->used_mask)) {
			/* Check if the preceding even counter is available */
			if (!test_and_set_bit(idx - 1, cpuc->used_mask))
				return idx;
			/* Release the Odd counter */
			clear_bit(idx, cpuc->used_mask);
		}
	}
	return -EAGAIN;
}

static int armv8pmu_get_event_idx(struct pmu_hw_events *cpuc,
				  struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long evtype = hwc->config_base & ARMV8_PMU_EVTYPE_EVENT;

	/* Always prefer to place a cycle counter into the cycle counter. */
	if ((evtype == ARMV8_PMUV3_PERFCTR_CPU_CYCLES) &&
	    !armv8pmu_event_get_threshold(&event->attr) && !has_branch_stack(event)) {
		if (!test_and_set_bit(ARMV8_PMU_CYCLE_IDX, cpuc->used_mask))
			return ARMV8_PMU_CYCLE_IDX;
		else if (armv8pmu_event_is_64bit(event) &&
			   armv8pmu_event_want_user_access(event) &&
			   !armv8pmu_has_long_event(cpu_pmu))
				return -EAGAIN;
	}

	/*
	 * Always prefer to place a instruction counter into the instruction counter,
	 * but don't expose the instruction counter to userspace access as userspace
	 * may not know how to handle it.
	 */
	if ((evtype == ARMV8_PMUV3_PERFCTR_INST_RETIRED) &&
	    !armv8pmu_event_get_threshold(&event->attr) &&
	    test_bit(ARMV8_PMU_INSTR_IDX, cpu_pmu->cntr_mask) &&
	    !armv8pmu_event_want_user_access(event)) {
		if (!test_and_set_bit(ARMV8_PMU_INSTR_IDX, cpuc->used_mask))
			return ARMV8_PMU_INSTR_IDX;
	}

	/*
	 * Otherwise use events counters
	 */
	if (armv8pmu_event_is_chained(event))
		return	armv8pmu_get_chain_idx(cpuc, cpu_pmu);
	else
		return armv8pmu_get_single_idx(cpuc, cpu_pmu);
}

static void armv8pmu_clear_event_idx(struct pmu_hw_events *cpuc,
				     struct perf_event *event)
{
	int idx = event->hw.idx;

	clear_bit(idx, cpuc->used_mask);
	if (armv8pmu_event_is_chained(event))
		clear_bit(idx - 1, cpuc->used_mask);
}

static int armv8pmu_user_event_idx(struct perf_event *event)
{
	if (!sysctl_perf_user_access || !armv8pmu_event_has_user_read(event))
		return 0;

	return event->hw.idx + 1;
}

static void armv8pmu_sched_task(struct perf_event_pmu_context *pmu_ctx,
				struct task_struct *task, bool sched_in)
{
	struct arm_pmu *armpmu = *this_cpu_ptr(&cpu_armpmu);
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);

	if (!hw_events->branch_users)
		return;

	if (sched_in)
		brbe_invalidate();
}

/*
 * Add an event filter to a given event.
 */
static int armv8pmu_set_event_filter(struct hw_perf_event *event,
				     struct perf_event_attr *attr)
{
	unsigned long config_base = 0;
	struct perf_event *perf_event = container_of(attr, struct perf_event,
						     attr);
	struct arm_pmu *cpu_pmu = to_arm_pmu(perf_event->pmu);
	u32 th;

	if (attr->exclude_idle) {
		pr_debug("ARM performance counters do not support mode exclusion\n");
		return -EOPNOTSUPP;
	}

	if (has_branch_stack(perf_event)) {
		if (!brbe_num_branch_records(cpu_pmu) || !brbe_branch_attr_valid(perf_event))
			return -EOPNOTSUPP;

		perf_event->attach_state |= PERF_ATTACH_SCHED_CB;
	}

	/*
	 * If we're running in hyp mode, then we *are* the hypervisor.
	 * Therefore we ignore exclude_hv in this configuration, since
	 * there's no hypervisor to sample anyway. This is consistent
	 * with other architectures (x86 and Power).
	 */
	if (is_kernel_in_hyp_mode()) {
		if (!attr->exclude_kernel && !attr->exclude_host)
			config_base |= ARMV8_PMU_INCLUDE_EL2;
		if (attr->exclude_guest)
			config_base |= ARMV8_PMU_EXCLUDE_EL1;
		if (attr->exclude_host)
			config_base |= ARMV8_PMU_EXCLUDE_EL0;
	} else {
		if (!attr->exclude_hv && !attr->exclude_host)
			config_base |= ARMV8_PMU_INCLUDE_EL2;
	}

	/*
	 * Filter out !VHE kernels and guest kernels
	 */
	if (attr->exclude_kernel)
		config_base |= ARMV8_PMU_EXCLUDE_EL1;

	if (attr->exclude_user)
		config_base |= ARMV8_PMU_EXCLUDE_EL0;

	/*
	 * If FEAT_PMUv3_TH isn't implemented, then THWIDTH (threshold_max) will
	 * be 0 and will also trigger this check, preventing it from being used.
	 */
	th = armv8pmu_event_get_threshold(attr);
	if (th > threshold_max(cpu_pmu)) {
		pr_debug("PMU event threshold exceeds max value\n");
		return -EINVAL;
	}

	if (th) {
		config_base |= FIELD_PREP(ARMV8_PMU_EVTYPE_TH, th);
		config_base |= FIELD_PREP(ARMV8_PMU_EVTYPE_TC,
					  armv8pmu_event_threshold_control(attr));
	}

	/*
	 * Install the filter into config_base as this is used to
	 * construct the event type.
	 */
	event->config_base = config_base;

	return 0;
}

static void armv8pmu_reset(void *info)
{
	struct arm_pmu *cpu_pmu = (struct arm_pmu *)info;
	u64 pmcr, mask;

	bitmap_to_arr64(&mask, cpu_pmu->cntr_mask, ARMPMU_MAX_HWEVENTS);

	/* The counter and interrupt enable registers are unknown at reset. */
	armv8pmu_disable_counter(mask);
	armv8pmu_disable_intens(mask);

	/* Clear the counters we flip at guest entry/exit */
	kvm_clr_pmu_events(mask);

	if (brbe_num_branch_records(cpu_pmu)) {
		brbe_disable();
		brbe_invalidate();
	}

	/*
	 * Initialize & Reset PMNC. Request overflow interrupt for
	 * 64 bit cycle counter but cheat in armv8pmu_write_counter().
	 */
	pmcr = ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C | ARMV8_PMU_PMCR_LC;

	/* Enable long event counter support where available */
	if (armv8pmu_has_long_event(cpu_pmu))
		pmcr |= ARMV8_PMU_PMCR_LP;

	armv8pmu_pmcr_write(pmcr);
}

static int __armv8_pmuv3_map_event_id(struct arm_pmu *armpmu,
				      struct perf_event *event)
{
	if (event->attr.type == PERF_TYPE_HARDWARE &&
	    event->attr.config == PERF_COUNT_HW_BRANCH_INSTRUCTIONS) {

		if (test_bit(ARMV8_PMUV3_PERFCTR_BR_RETIRED,
			     armpmu->pmceid_bitmap))
			return ARMV8_PMUV3_PERFCTR_BR_RETIRED;

		if (test_bit(ARMV8_PMUV3_PERFCTR_PC_WRITE_RETIRED,
			     armpmu->pmceid_bitmap))
			return ARMV8_PMUV3_PERFCTR_PC_WRITE_RETIRED;

		return HW_OP_UNSUPPORTED;
	}

	return armpmu_map_event(event, &armv8_pmuv3_perf_map,
				&armv8_pmuv3_perf_cache_map,
				ARMV8_PMU_EVTYPE_EVENT);
}

static int __armv8_pmuv3_map_event(struct perf_event *event,
				   const unsigned (*extra_event_map)
						  [PERF_COUNT_HW_MAX],
				   const unsigned (*extra_cache_map)
						  [PERF_COUNT_HW_CACHE_MAX]
						  [PERF_COUNT_HW_CACHE_OP_MAX]
						  [PERF_COUNT_HW_CACHE_RESULT_MAX])
{
	int hw_event_id;
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);

	hw_event_id = __armv8_pmuv3_map_event_id(armpmu, event);

	/*
	 * CHAIN events only work when paired with an adjacent counter, and it
	 * never makes sense for a user to open one in isolation, as they'll be
	 * rotated arbitrarily.
	 */
	if (hw_event_id == ARMV8_PMUV3_PERFCTR_CHAIN)
		return -EINVAL;

	if (armv8pmu_event_is_64bit(event))
		event->hw.flags |= ARMPMU_EVT_64BIT;

	/*
	 * User events must be allocated into a single counter, and so
	 * must not be chained.
	 *
	 * Most 64-bit events require long counter support, but 64-bit
	 * CPU_CYCLES events can be placed into the dedicated cycle
	 * counter when this is free.
	 */
	if (armv8pmu_event_want_user_access(event)) {
		if (!(event->attach_state & PERF_ATTACH_TASK))
			return -EINVAL;
		if (armv8pmu_event_is_64bit(event) &&
		    (hw_event_id != ARMV8_PMUV3_PERFCTR_CPU_CYCLES) &&
		    !armv8pmu_has_long_event(armpmu))
			return -EOPNOTSUPP;

		event->hw.flags |= PERF_EVENT_FLAG_USER_READ_CNT;
	}

	/* Only expose micro/arch events supported by this PMU */
	if ((hw_event_id > 0) && (hw_event_id < ARMV8_PMUV3_MAX_COMMON_EVENTS)
	    && test_bit(hw_event_id, armpmu->pmceid_bitmap)) {
		return hw_event_id;
	}

	return armpmu_map_event(event, extra_event_map, extra_cache_map,
				ARMV8_PMU_EVTYPE_EVENT);
}

static int armv8_pmuv3_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, NULL);
}

static int armv8_a53_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a53_perf_cache_map);
}

static int armv8_a57_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a57_perf_cache_map);
}

static int armv8_a73_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a73_perf_cache_map);
}

static int armv8_thunder_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL,
				       &armv8_thunder_perf_cache_map);
}

static int armv8_vulcan_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL,
				       &armv8_vulcan_perf_cache_map);
}

struct armv8pmu_probe_info {
	struct arm_pmu *pmu;
	bool present;
};

static void __armv8pmu_probe_pmu(void *info)
{
	struct armv8pmu_probe_info *probe = info;
	struct arm_pmu *cpu_pmu = probe->pmu;
	u64 pmceid_raw[2];
	u32 pmceid[2];
	int pmuver;

	pmuver = read_pmuver();
	if (!pmuv3_implemented(pmuver))
		return;

	cpu_pmu->pmuver = pmuver;
	probe->present = true;

	/* Read the nb of CNTx counters supported from PMNC */
	bitmap_set(cpu_pmu->cntr_mask,
		   0, FIELD_GET(ARMV8_PMU_PMCR_N, armv8pmu_pmcr_read()));

	/* Add the CPU cycles counter */
	set_bit(ARMV8_PMU_CYCLE_IDX, cpu_pmu->cntr_mask);

	/* Add the CPU instructions counter */
	if (pmuv3_has_icntr())
		set_bit(ARMV8_PMU_INSTR_IDX, cpu_pmu->cntr_mask);

	pmceid[0] = pmceid_raw[0] = read_pmceid0();
	pmceid[1] = pmceid_raw[1] = read_pmceid1();

	bitmap_from_arr32(cpu_pmu->pmceid_bitmap,
			     pmceid, ARMV8_PMUV3_MAX_COMMON_EVENTS);

	pmceid[0] = pmceid_raw[0] >> 32;
	pmceid[1] = pmceid_raw[1] >> 32;

	bitmap_from_arr32(cpu_pmu->pmceid_ext_bitmap,
			     pmceid, ARMV8_PMUV3_MAX_COMMON_EVENTS);

	/* store PMMIR register for sysfs */
	if (is_pmuv3p4(pmuver))
		cpu_pmu->reg_pmmir = read_pmmir();
	else
		cpu_pmu->reg_pmmir = 0;

	brbe_probe(cpu_pmu);
}

static int branch_records_alloc(struct arm_pmu *armpmu)
{
	size_t size = struct_size_t(struct perf_branch_stack, entries,
				    brbe_num_branch_records(armpmu));
	int cpu;

	for_each_cpu(cpu, &armpmu->supported_cpus) {
		struct pmu_hw_events *events_cpu;

		events_cpu = per_cpu_ptr(armpmu->hw_events, cpu);
		events_cpu->branch_stack = kmalloc(size, GFP_KERNEL);
		if (!events_cpu->branch_stack)
			return -ENOMEM;
	}
	return 0;
}

static int armv8pmu_probe_pmu(struct arm_pmu *cpu_pmu)
{
	struct armv8pmu_probe_info probe = {
		.pmu = cpu_pmu,
		.present = false,
	};
	int ret;

	ret = smp_call_function_any(&cpu_pmu->supported_cpus,
				    __armv8pmu_probe_pmu,
				    &probe, 1);
	if (ret)
		return ret;

	if (!probe.present)
		return -ENODEV;

	if (brbe_num_branch_records(cpu_pmu)) {
		ret = branch_records_alloc(cpu_pmu);
		if (ret)
			return ret;
	}
	return 0;
}

static void armv8pmu_disable_user_access_ipi(void *unused)
{
	armv8pmu_disable_user_access();
}

static int armv8pmu_proc_user_access_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write || sysctl_perf_user_access)
		return ret;

	on_each_cpu(armv8pmu_disable_user_access_ipi, NULL, 1);
	return 0;
}

static const struct ctl_table armv8_pmu_sysctl_table[] = {
	{
		.procname       = "perf_user_access",
		.data		= &sysctl_perf_user_access,
		.maxlen		= sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler	= armv8pmu_proc_user_access_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static void armv8_pmu_register_sysctl_table(void)
{
	static u32 tbl_registered = 0;

	if (!cmpxchg_relaxed(&tbl_registered, 0, 1))
		register_sysctl("kernel", armv8_pmu_sysctl_table);
}

static int armv8_pmu_init(struct arm_pmu *cpu_pmu, char *name,
			  int (*map_event)(struct perf_event *event))
{
	int ret = armv8pmu_probe_pmu(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->handle_irq		= armv8pmu_handle_irq;
	cpu_pmu->enable			= armv8pmu_enable_event;
	cpu_pmu->disable		= armv8pmu_disable_event;
	cpu_pmu->read_counter		= armv8pmu_read_counter;
	cpu_pmu->write_counter		= armv8pmu_write_counter;
	cpu_pmu->get_event_idx		= armv8pmu_get_event_idx;
	cpu_pmu->clear_event_idx	= armv8pmu_clear_event_idx;
	cpu_pmu->start			= armv8pmu_start;
	cpu_pmu->stop			= armv8pmu_stop;
	cpu_pmu->reset			= armv8pmu_reset;
	cpu_pmu->set_event_filter	= armv8pmu_set_event_filter;

	cpu_pmu->pmu.event_idx		= armv8pmu_user_event_idx;
	if (brbe_num_branch_records(cpu_pmu))
		cpu_pmu->pmu.sched_task		= armv8pmu_sched_task;

	cpu_pmu->name			= name;
	cpu_pmu->map_event		= map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] = &armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] = &armv8_pmuv3_format_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_CAPS] = &armv8_pmuv3_caps_attr_group;
	armv8_pmu_register_sysctl_table();
	return 0;
}

#define PMUV3_INIT_SIMPLE(name)						\
static int name##_pmu_init(struct arm_pmu *cpu_pmu)			\
{									\
	return armv8_pmu_init(cpu_pmu, #name, armv8_pmuv3_map_event);	\
}

#define PMUV3_INIT_MAP_EVENT(name, map_event)				\
static int name##_pmu_init(struct arm_pmu *cpu_pmu)			\
{									\
	return armv8_pmu_init(cpu_pmu, #name, map_event);		\
}

PMUV3_INIT_SIMPLE(armv8_pmuv3)

PMUV3_INIT_SIMPLE(armv8_cortex_a34)
PMUV3_INIT_SIMPLE(armv8_cortex_a55)
PMUV3_INIT_SIMPLE(armv8_cortex_a65)
PMUV3_INIT_SIMPLE(armv8_cortex_a75)
PMUV3_INIT_SIMPLE(armv8_cortex_a76)
PMUV3_INIT_SIMPLE(armv8_cortex_a77)
PMUV3_INIT_SIMPLE(armv8_cortex_a78)
PMUV3_INIT_SIMPLE(armv9_cortex_a510)
PMUV3_INIT_SIMPLE(armv9_cortex_a520)
PMUV3_INIT_SIMPLE(armv9_cortex_a710)
PMUV3_INIT_SIMPLE(armv9_cortex_a715)
PMUV3_INIT_SIMPLE(armv9_cortex_a720)
PMUV3_INIT_SIMPLE(armv9_cortex_a725)
PMUV3_INIT_SIMPLE(armv8_cortex_x1)
PMUV3_INIT_SIMPLE(armv9_cortex_x2)
PMUV3_INIT_SIMPLE(armv9_cortex_x3)
PMUV3_INIT_SIMPLE(armv9_cortex_x4)
PMUV3_INIT_SIMPLE(armv9_cortex_x925)
PMUV3_INIT_SIMPLE(armv8_neoverse_e1)
PMUV3_INIT_SIMPLE(armv8_neoverse_n1)
PMUV3_INIT_SIMPLE(armv9_neoverse_n2)
PMUV3_INIT_SIMPLE(armv9_neoverse_n3)
PMUV3_INIT_SIMPLE(armv8_neoverse_v1)
PMUV3_INIT_SIMPLE(armv8_neoverse_v2)
PMUV3_INIT_SIMPLE(armv8_neoverse_v3)
PMUV3_INIT_SIMPLE(armv8_neoverse_v3ae)
PMUV3_INIT_SIMPLE(armv8_rainier)

PMUV3_INIT_SIMPLE(armv8_nvidia_carmel)
PMUV3_INIT_SIMPLE(armv8_nvidia_denver)

PMUV3_INIT_SIMPLE(armv8_samsung_mongoose)

PMUV3_INIT_MAP_EVENT(armv8_cortex_a35, armv8_a53_map_event)
PMUV3_INIT_MAP_EVENT(armv8_cortex_a53, armv8_a53_map_event)
PMUV3_INIT_MAP_EVENT(armv8_cortex_a57, armv8_a57_map_event)
PMUV3_INIT_MAP_EVENT(armv8_cortex_a72, armv8_a57_map_event)
PMUV3_INIT_MAP_EVENT(armv8_cortex_a73, armv8_a73_map_event)
PMUV3_INIT_MAP_EVENT(armv8_cavium_thunder, armv8_thunder_map_event)
PMUV3_INIT_MAP_EVENT(armv8_brcm_vulcan, armv8_vulcan_map_event)

static const struct of_device_id armv8_pmu_of_device_ids[] = {
	{.compatible = "arm,armv8-pmuv3",	.data = armv8_pmuv3_pmu_init},
	{.compatible = "arm,cortex-a34-pmu",	.data = armv8_cortex_a34_pmu_init},
	{.compatible = "arm,cortex-a35-pmu",	.data = armv8_cortex_a35_pmu_init},
	{.compatible = "arm,cortex-a53-pmu",	.data = armv8_cortex_a53_pmu_init},
	{.compatible = "arm,cortex-a55-pmu",	.data = armv8_cortex_a55_pmu_init},
	{.compatible = "arm,cortex-a57-pmu",	.data = armv8_cortex_a57_pmu_init},
	{.compatible = "arm,cortex-a65-pmu",	.data = armv8_cortex_a65_pmu_init},
	{.compatible = "arm,cortex-a72-pmu",	.data = armv8_cortex_a72_pmu_init},
	{.compatible = "arm,cortex-a73-pmu",	.data = armv8_cortex_a73_pmu_init},
	{.compatible = "arm,cortex-a75-pmu",	.data = armv8_cortex_a75_pmu_init},
	{.compatible = "arm,cortex-a76-pmu",	.data = armv8_cortex_a76_pmu_init},
	{.compatible = "arm,cortex-a77-pmu",	.data = armv8_cortex_a77_pmu_init},
	{.compatible = "arm,cortex-a78-pmu",	.data = armv8_cortex_a78_pmu_init},
	{.compatible = "arm,cortex-a510-pmu",	.data = armv9_cortex_a510_pmu_init},
	{.compatible = "arm,cortex-a520-pmu",	.data = armv9_cortex_a520_pmu_init},
	{.compatible = "arm,cortex-a710-pmu",	.data = armv9_cortex_a710_pmu_init},
	{.compatible = "arm,cortex-a715-pmu",	.data = armv9_cortex_a715_pmu_init},
	{.compatible = "arm,cortex-a720-pmu",	.data = armv9_cortex_a720_pmu_init},
	{.compatible = "arm,cortex-a725-pmu",	.data = armv9_cortex_a725_pmu_init},
	{.compatible = "arm,cortex-x1-pmu",	.data = armv8_cortex_x1_pmu_init},
	{.compatible = "arm,cortex-x2-pmu",	.data = armv9_cortex_x2_pmu_init},
	{.compatible = "arm,cortex-x3-pmu",	.data = armv9_cortex_x3_pmu_init},
	{.compatible = "arm,cortex-x4-pmu",	.data = armv9_cortex_x4_pmu_init},
	{.compatible = "arm,cortex-x925-pmu",	.data = armv9_cortex_x925_pmu_init},
	{.compatible = "arm,neoverse-e1-pmu",	.data = armv8_neoverse_e1_pmu_init},
	{.compatible = "arm,neoverse-n1-pmu",	.data = armv8_neoverse_n1_pmu_init},
	{.compatible = "arm,neoverse-n2-pmu",	.data = armv9_neoverse_n2_pmu_init},
	{.compatible = "arm,neoverse-n3-pmu",	.data = armv9_neoverse_n3_pmu_init},
	{.compatible = "arm,neoverse-v1-pmu",	.data = armv8_neoverse_v1_pmu_init},
	{.compatible = "arm,neoverse-v2-pmu",	.data = armv8_neoverse_v2_pmu_init},
	{.compatible = "arm,neoverse-v3-pmu",	.data = armv8_neoverse_v3_pmu_init},
	{.compatible = "arm,neoverse-v3ae-pmu",	.data = armv8_neoverse_v3ae_pmu_init},
	{.compatible = "arm,rainier-pmu",	.data = armv8_rainier_pmu_init},
	{.compatible = "cavium,thunder-pmu",	.data = armv8_cavium_thunder_pmu_init},
	{.compatible = "brcm,vulcan-pmu",	.data = armv8_brcm_vulcan_pmu_init},
	{.compatible = "nvidia,carmel-pmu",	.data = armv8_nvidia_carmel_pmu_init},
	{.compatible = "nvidia,denver-pmu",	.data = armv8_nvidia_denver_pmu_init},
	{.compatible = "samsung,mongoose-pmu",	.data = armv8_samsung_mongoose_pmu_init},
	{},
};

static int armv8_pmu_device_probe(struct platform_device *pdev)
{
	return arm_pmu_device_probe(pdev, armv8_pmu_of_device_ids, NULL);
}

static struct platform_driver armv8_pmu_driver = {
	.driver		= {
		.name	= ARMV8_PMU_PDEV_NAME,
		.of_match_table = armv8_pmu_of_device_ids,
		.suppress_bind_attrs = true,
	},
	.probe		= armv8_pmu_device_probe,
};

static int __init armv8_pmu_driver_init(void)
{
	int ret;

	if (acpi_disabled)
		ret = platform_driver_register(&armv8_pmu_driver);
	else
		ret = arm_pmu_acpi_probe(armv8_pmuv3_pmu_init);

	if (!ret)
		lockup_detector_retry_init();

	return ret;
}
device_initcall(armv8_pmu_driver_init)

void arch_perf_update_userpage(struct perf_event *event,
			       struct perf_event_mmap_page *userpg, u64 now)
{
	struct clock_read_data *rd;
	unsigned int seq;
	u64 ns;

	userpg->cap_user_time = 0;
	userpg->cap_user_time_zero = 0;
	userpg->cap_user_time_short = 0;
	userpg->cap_user_rdpmc = armv8pmu_event_has_user_read(event);

	if (userpg->cap_user_rdpmc) {
		if (event->hw.flags & ARMPMU_EVT_64BIT)
			userpg->pmc_width = 64;
		else
			userpg->pmc_width = 32;
	}

	do {
		rd = sched_clock_read_begin(&seq);

		if (rd->read_sched_clock != arch_timer_read_counter)
			return;

		userpg->time_mult = rd->mult;
		userpg->time_shift = rd->shift;
		userpg->time_zero = rd->epoch_ns;
		userpg->time_cycles = rd->epoch_cyc;
		userpg->time_mask = rd->sched_clock_mask;

		/*
		 * Subtract the cycle base, such that software that
		 * doesn't know about cap_user_time_short still 'works'
		 * assuming no wraps.
		 */
		ns = mul_u64_u32_shr(rd->epoch_cyc, rd->mult, rd->shift);
		userpg->time_zero -= ns;

	} while (sched_clock_read_retry(seq));

	userpg->time_offset = userpg->time_zero - now;

	/*
	 * time_shift is not expected to be greater than 31 due to
	 * the original published conversion algorithm shifting a
	 * 32-bit value (now specifies a 64-bit value) - refer
	 * perf_event_mmap_page documentation in perf_event.h.
	 */
	if (userpg->time_shift == 32) {
		userpg->time_shift = 31;
		userpg->time_mult >>= 1;
	}

	/*
	 * Internal timekeeping for enabled/running/stopped times
	 * is always computed with the sched_clock.
	 */
	userpg->cap_user_time = 1;
	userpg->cap_user_time_zero = 1;
	userpg->cap_user_time_short = 1;
}
