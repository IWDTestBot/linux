/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#ifndef __ND_CORE_H__
#define __ND_CORE_H__
#include <linux/libnvdimm.h>
#include <linux/device.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/nd.h>
#include "nd.h"

extern struct list_head nvdimm_bus_list;
extern struct mutex nvdimm_bus_list_mutex;
extern int nvdimm_major;
extern struct workqueue_struct *nvdimm_wq;

struct nvdimm_bus {
	struct nvdimm_bus_descriptor *nd_desc;
	wait_queue_head_t wait;
	struct list_head list;
	struct device dev;
	int id, probe_active;
	atomic_t ioctl_active;
	struct list_head mapping_list;
	struct mutex reconfig_mutex;
	struct badrange badrange;
};

struct nvdimm {
	unsigned long flags;
	void *provider_data;
	unsigned long cmd_mask;
	struct device dev;
	atomic_t busy;
	int id, num_flush;
	struct resource *flush_wpq;
	const char *dimm_id;
	struct {
		const struct nvdimm_security_ops *ops;
		unsigned long flags;
		unsigned long ext_flags;
		unsigned int overwrite_tmo;
		struct kernfs_node *overwrite_state;
	} sec;
	struct delayed_work dwork;
	const struct nvdimm_fw_ops *fw_ops;
};

static inline unsigned long nvdimm_security_flags(
		struct nvdimm *nvdimm, enum nvdimm_passphrase_type ptype)
{
	u64 flags;
	const u64 state_flags = 1UL << NVDIMM_SECURITY_DISABLED
		| 1UL << NVDIMM_SECURITY_LOCKED
		| 1UL << NVDIMM_SECURITY_UNLOCKED
		| 1UL << NVDIMM_SECURITY_OVERWRITE;

	if (!nvdimm->sec.ops)
		return 0;

	flags = nvdimm->sec.ops->get_flags(nvdimm, ptype);
	/* disabled, locked, unlocked, and overwrite are mutually exclusive */
	dev_WARN_ONCE(&nvdimm->dev, hweight64(flags & state_flags) > 1,
			"reported invalid security state: %#llx\n",
			(unsigned long long) flags);
	return flags;
}
int nvdimm_security_freeze(struct nvdimm *nvdimm);
#if IS_ENABLED(CONFIG_NVDIMM_KEYS)
ssize_t nvdimm_security_store(struct device *dev, const char *buf, size_t len);
void nvdimm_security_overwrite_query(struct work_struct *work);
#else
static inline ssize_t nvdimm_security_store(struct device *dev,
		const char *buf, size_t len)
{
	return -EOPNOTSUPP;
}
static inline void nvdimm_security_overwrite_query(struct work_struct *work)
{
}
#endif

bool is_nvdimm(const struct device *dev);
bool is_nd_pmem(const struct device *dev);
bool is_nd_volatile(const struct device *dev);
static inline bool is_nd_region(const struct device *dev)
{
	return is_nd_pmem(dev) || is_nd_volatile(dev);
}
static inline bool is_memory(const struct device *dev)
{
	return is_nd_pmem(dev) || is_nd_volatile(dev);
}
struct nvdimm_bus *walk_to_nvdimm_bus(struct device *nd_dev);
int __init nvdimm_bus_init(void);
void nvdimm_bus_exit(void);
void nvdimm_devs_exit(void);
struct nd_region;
void nd_region_advance_seeds(struct nd_region *nd_region, struct device *dev);
void nd_region_create_ns_seed(struct nd_region *nd_region);
void nd_region_create_btt_seed(struct nd_region *nd_region);
void nd_region_create_pfn_seed(struct nd_region *nd_region);
void nd_region_create_dax_seed(struct nd_region *nd_region);
int nvdimm_bus_create_ndctl(struct nvdimm_bus *nvdimm_bus);
void nvdimm_bus_destroy_ndctl(struct nvdimm_bus *nvdimm_bus);
void nd_synchronize(void);
void nd_device_register(struct device *dev);
void nd_device_register_sync(struct device *dev);
struct nd_label_id;
char *nd_label_gen_id(struct nd_label_id *label_id, const uuid_t *uuid,
		      u32 flags);
bool nd_is_uuid_unique(struct device *dev, uuid_t *uuid);
struct nd_region;
struct nvdimm_drvdata;
struct nd_mapping;
void nd_mapping_free_labels(struct nd_mapping *nd_mapping);

int __reserve_free_pmem(struct device *dev, void *data);
void release_free_pmem(struct nvdimm_bus *nvdimm_bus,
		       struct nd_mapping *nd_mapping);

resource_size_t nd_pmem_max_contiguous_dpa(struct nd_region *nd_region,
					   struct nd_mapping *nd_mapping);
resource_size_t nd_region_allocatable_dpa(struct nd_region *nd_region);
resource_size_t nd_pmem_available_dpa(struct nd_region *nd_region,
				      struct nd_mapping *nd_mapping);
resource_size_t nd_region_available_dpa(struct nd_region *nd_region);
resource_size_t nvdimm_allocated_dpa(struct nvdimm_drvdata *ndd,
		struct nd_label_id *label_id);
int nvdimm_num_label_slots(struct nvdimm_drvdata *ndd);
void get_ndd(struct nvdimm_drvdata *ndd);
resource_size_t __nvdimm_namespace_capacity(struct nd_namespace_common *ndns);
void nd_detach_ndns(struct device *dev, struct nd_namespace_common **_ndns);
void __nd_detach_ndns(struct device *dev, struct nd_namespace_common **_ndns);
bool __nd_attach_ndns(struct device *dev, struct nd_namespace_common *attach,
		struct nd_namespace_common **_ndns);
ssize_t nd_namespace_store(struct device *dev,
		struct nd_namespace_common **_ndns, const char *buf,
		size_t len);
struct nd_pfn *to_nd_pfn_safe(struct device *dev);
bool is_nvdimm_bus(struct device *dev);

#if IS_ENABLED(CONFIG_ND_CLAIM)
int devm_nsio_enable(struct device *dev, struct nd_namespace_io *nsio,
		resource_size_t size);
void devm_nsio_disable(struct device *dev, struct nd_namespace_io *nsio);
#else
static inline int devm_nsio_enable(struct device *dev,
		struct nd_namespace_io *nsio, resource_size_t size)
{
	return -ENXIO;
}

static inline void devm_nsio_disable(struct device *dev,
		struct nd_namespace_io *nsio)
{
}
#endif
#endif /* __ND_CORE_H__ */
