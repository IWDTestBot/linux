// SPDX-License-Identifier: GPL-2.0
#include "cpumap.h"
#include "debug.h"
#include "env.h"
#include "util/header.h"
#include "util/rwsem.h"
#include <linux/compiler.h>
#include <linux/ctype.h>
#include <linux/rbtree.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include "cgroup.h"
#include <errno.h>
#include <sys/utsname.h>
#include <stdlib.h>
#include <string.h>
#include "pmu.h"
#include "pmus.h"
#include "strbuf.h"
#include "trace/beauty/beauty.h"

#ifdef HAVE_LIBBPF_SUPPORT
#include "bpf-event.h"
#include "bpf-utils.h"
#include <bpf/libbpf.h>

bool perf_env__insert_bpf_prog_info(struct perf_env *env,
				    struct bpf_prog_info_node *info_node)
{
	bool ret;

	down_write(&env->bpf_progs.lock);
	ret = __perf_env__insert_bpf_prog_info(env, info_node);
	up_write(&env->bpf_progs.lock);

	return ret;
}

bool __perf_env__insert_bpf_prog_info(struct perf_env *env, struct bpf_prog_info_node *info_node)
{
	__u32 prog_id = info_node->info_linear->info.id;
	struct bpf_prog_info_node *node;
	struct rb_node *parent = NULL;
	struct rb_node **p;

	p = &env->bpf_progs.infos.rb_node;

	while (*p != NULL) {
		parent = *p;
		node = rb_entry(parent, struct bpf_prog_info_node, rb_node);
		if (prog_id < node->info_linear->info.id) {
			p = &(*p)->rb_left;
		} else if (prog_id > node->info_linear->info.id) {
			p = &(*p)->rb_right;
		} else {
			pr_debug("duplicated bpf prog info %u\n", prog_id);
			return false;
		}
	}

	rb_link_node(&info_node->rb_node, parent, p);
	rb_insert_color(&info_node->rb_node, &env->bpf_progs.infos);
	env->bpf_progs.infos_cnt++;
	return true;
}

struct bpf_prog_info_node *perf_env__find_bpf_prog_info(struct perf_env *env,
							__u32 prog_id)
{
	struct bpf_prog_info_node *node = NULL;
	struct rb_node *n;

	down_read(&env->bpf_progs.lock);
	n = env->bpf_progs.infos.rb_node;

	while (n) {
		node = rb_entry(n, struct bpf_prog_info_node, rb_node);
		if (prog_id < node->info_linear->info.id)
			n = n->rb_left;
		else if (prog_id > node->info_linear->info.id)
			n = n->rb_right;
		else
			goto out;
	}
	node = NULL;

out:
	up_read(&env->bpf_progs.lock);
	return node;
}

void perf_env__iterate_bpf_prog_info(struct perf_env *env,
				     void (*cb)(struct bpf_prog_info_node *node,
						void *data),
				     void *data)
{
	struct rb_node *first;

	down_read(&env->bpf_progs.lock);
	first = rb_first(&env->bpf_progs.infos);
	for (struct rb_node *node = first; node != NULL; node = rb_next(node))
		(*cb)(rb_entry(node, struct bpf_prog_info_node, rb_node), data);
	up_read(&env->bpf_progs.lock);
}

bool perf_env__insert_btf(struct perf_env *env, struct btf_node *btf_node)
{
	bool ret;

	down_write(&env->bpf_progs.lock);
	ret = __perf_env__insert_btf(env, btf_node);
	up_write(&env->bpf_progs.lock);
	return ret;
}

bool __perf_env__insert_btf(struct perf_env *env, struct btf_node *btf_node)
{
	struct rb_node *parent = NULL;
	__u32 btf_id = btf_node->id;
	struct btf_node *node;
	struct rb_node **p;

	p = &env->bpf_progs.btfs.rb_node;

	while (*p != NULL) {
		parent = *p;
		node = rb_entry(parent, struct btf_node, rb_node);
		if (btf_id < node->id) {
			p = &(*p)->rb_left;
		} else if (btf_id > node->id) {
			p = &(*p)->rb_right;
		} else {
			pr_debug("duplicated btf %u\n", btf_id);
			return false;
		}
	}

	rb_link_node(&btf_node->rb_node, parent, p);
	rb_insert_color(&btf_node->rb_node, &env->bpf_progs.btfs);
	env->bpf_progs.btfs_cnt++;
	return true;
}

struct btf_node *perf_env__find_btf(struct perf_env *env, __u32 btf_id)
{
	struct btf_node *res;

	down_read(&env->bpf_progs.lock);
	res = __perf_env__find_btf(env, btf_id);
	up_read(&env->bpf_progs.lock);
	return res;
}

struct btf_node *__perf_env__find_btf(struct perf_env *env, __u32 btf_id)
{
	struct btf_node *node = NULL;
	struct rb_node *n;

	n = env->bpf_progs.btfs.rb_node;

	while (n) {
		node = rb_entry(n, struct btf_node, rb_node);
		if (btf_id < node->id)
			n = n->rb_left;
		else if (btf_id > node->id)
			n = n->rb_right;
		else
			return node;
	}
	return NULL;
}

/* purge data in bpf_progs.infos tree */
static void perf_env__purge_bpf(struct perf_env *env)
{
	struct rb_root *root;
	struct rb_node *next;

	down_write(&env->bpf_progs.lock);

	root = &env->bpf_progs.infos;
	next = rb_first(root);

	while (next) {
		struct bpf_prog_info_node *node;

		node = rb_entry(next, struct bpf_prog_info_node, rb_node);
		next = rb_next(&node->rb_node);
		rb_erase(&node->rb_node, root);
		zfree(&node->info_linear);
		bpf_metadata_free(node->metadata);
		free(node);
	}

	env->bpf_progs.infos_cnt = 0;

	root = &env->bpf_progs.btfs;
	next = rb_first(root);

	while (next) {
		struct btf_node *node;

		node = rb_entry(next, struct btf_node, rb_node);
		next = rb_next(&node->rb_node);
		rb_erase(&node->rb_node, root);
		free(node);
	}

	env->bpf_progs.btfs_cnt = 0;

	up_write(&env->bpf_progs.lock);
}
#else // HAVE_LIBBPF_SUPPORT
static void perf_env__purge_bpf(struct perf_env *env __maybe_unused)
{
}
#endif // HAVE_LIBBPF_SUPPORT

void perf_env__exit(struct perf_env *env)
{
	int i, j;

	perf_env__purge_bpf(env);
	perf_env__purge_cgroups(env);
	zfree(&env->hostname);
	zfree(&env->os_release);
	zfree(&env->version);
	zfree(&env->arch);
	zfree(&env->cpu_desc);
	zfree(&env->cpuid);
	zfree(&env->cmdline);
	zfree(&env->cmdline_argv);
	zfree(&env->sibling_dies);
	zfree(&env->sibling_cores);
	zfree(&env->sibling_threads);
	zfree(&env->pmu_mappings);
	zfree(&env->cpu);
	for (i = 0; i < env->nr_cpu_pmu_caps; i++)
		zfree(&env->cpu_pmu_caps[i]);
	zfree(&env->cpu_pmu_caps);
	zfree(&env->numa_map);

	for (i = 0; i < env->nr_numa_nodes; i++)
		perf_cpu_map__put(env->numa_nodes[i].map);
	zfree(&env->numa_nodes);

	for (i = 0; i < env->caches_cnt; i++)
		cpu_cache_level__free(&env->caches[i]);
	zfree(&env->caches);

	for (i = 0; i < env->nr_memory_nodes; i++)
		zfree(&env->memory_nodes[i].set);
	zfree(&env->memory_nodes);

	for (i = 0; i < env->nr_hybrid_nodes; i++) {
		zfree(&env->hybrid_nodes[i].pmu_name);
		zfree(&env->hybrid_nodes[i].cpus);
	}
	zfree(&env->hybrid_nodes);

	for (i = 0; i < env->nr_pmus_with_caps; i++) {
		for (j = 0; j < env->pmu_caps[i].nr_caps; j++)
			zfree(&env->pmu_caps[i].caps[j]);
		zfree(&env->pmu_caps[i].caps);
		zfree(&env->pmu_caps[i].pmu_name);
	}
	zfree(&env->pmu_caps);
}

void perf_env__init(struct perf_env *env)
{
	memset(env, 0, sizeof(*env));
#ifdef HAVE_LIBBPF_SUPPORT
	env->bpf_progs.infos = RB_ROOT;
	env->bpf_progs.btfs = RB_ROOT;
	init_rwsem(&env->bpf_progs.lock);
#endif
	env->kernel_is_64_bit = -1;
}

static void perf_env__init_kernel_mode(struct perf_env *env)
{
	const char *arch = perf_env__raw_arch(env);

	if (!strncmp(arch, "x86_64", 6) || !strncmp(arch, "aarch64", 7) ||
	    !strncmp(arch, "arm64", 5) || !strncmp(arch, "mips64", 6) ||
	    !strncmp(arch, "parisc64", 8) || !strncmp(arch, "riscv64", 7) ||
	    !strncmp(arch, "s390x", 5) || !strncmp(arch, "sparc64", 7))
		env->kernel_is_64_bit = 1;
	else
		env->kernel_is_64_bit = 0;
}

int perf_env__kernel_is_64_bit(struct perf_env *env)
{
	if (env->kernel_is_64_bit == -1)
		perf_env__init_kernel_mode(env);

	return env->kernel_is_64_bit;
}

int perf_env__set_cmdline(struct perf_env *env, int argc, const char *argv[])
{
	int i;

	/* do not include NULL termination */
	env->cmdline_argv = calloc(argc, sizeof(char *));
	if (env->cmdline_argv == NULL)
		goto out_enomem;

	/*
	 * Must copy argv contents because it gets moved around during option
	 * parsing:
	 */
	for (i = 0; i < argc ; i++) {
		env->cmdline_argv[i] = argv[i];
		if (env->cmdline_argv[i] == NULL)
			goto out_free;
	}

	env->nr_cmdline = argc;

	return 0;
out_free:
	zfree(&env->cmdline_argv);
out_enomem:
	return -ENOMEM;
}

int perf_env__read_cpu_topology_map(struct perf_env *env)
{
	int idx, nr_cpus;

	if (env->cpu != NULL)
		return 0;

	if (env->nr_cpus_avail == 0)
		env->nr_cpus_avail = cpu__max_present_cpu().cpu;

	nr_cpus = env->nr_cpus_avail;
	if (nr_cpus == -1)
		return -EINVAL;

	env->cpu = calloc(nr_cpus, sizeof(env->cpu[0]));
	if (env->cpu == NULL)
		return -ENOMEM;

	for (idx = 0; idx < nr_cpus; ++idx) {
		struct perf_cpu cpu = { .cpu = idx };
		int core_id   = cpu__get_core_id(cpu);
		int socket_id = cpu__get_socket_id(cpu);
		int die_id    = cpu__get_die_id(cpu);

		env->cpu[idx].core_id	= core_id >= 0 ? core_id : -1;
		env->cpu[idx].socket_id	= socket_id >= 0 ? socket_id : -1;
		env->cpu[idx].die_id	= die_id >= 0 ? die_id : -1;
	}

	env->nr_cpus_avail = nr_cpus;
	return 0;
}

int perf_env__read_pmu_mappings(struct perf_env *env)
{
	struct perf_pmu *pmu = NULL;
	u32 pmu_num = 0;
	struct strbuf sb;

	while ((pmu = perf_pmus__scan(pmu)))
		pmu_num++;

	if (!pmu_num) {
		pr_debug("pmu mappings not available\n");
		return -ENOENT;
	}
	env->nr_pmu_mappings = pmu_num;

	if (strbuf_init(&sb, 128 * pmu_num) < 0)
		return -ENOMEM;

	while ((pmu = perf_pmus__scan(pmu))) {
		if (strbuf_addf(&sb, "%u:%s", pmu->type, pmu->name) < 0)
			goto error;
		/* include a NULL character at the end */
		if (strbuf_add(&sb, "", 1) < 0)
			goto error;
	}

	env->pmu_mappings = strbuf_detach(&sb, NULL);

	return 0;

error:
	strbuf_release(&sb);
	return -1;
}

int perf_env__read_cpuid(struct perf_env *env)
{
	char cpuid[128];
	struct perf_cpu cpu = {-1};
	int err = get_cpuid(cpuid, sizeof(cpuid), cpu);

	if (err)
		return err;

	free(env->cpuid);
	env->cpuid = strdup(cpuid);
	if (env->cpuid == NULL)
		return ENOMEM;
	return 0;
}

static int perf_env__read_arch(struct perf_env *env)
{
	struct utsname uts;

	if (env->arch)
		return 0;

	if (!uname(&uts))
		env->arch = strdup(uts.machine);

	return env->arch ? 0 : -ENOMEM;
}

static int perf_env__read_nr_cpus_avail(struct perf_env *env)
{
	if (env->nr_cpus_avail == 0)
		env->nr_cpus_avail = cpu__max_present_cpu().cpu;

	return env->nr_cpus_avail ? 0 : -ENOENT;
}

static int __perf_env__read_core_pmu_caps(const struct perf_pmu *pmu,
					  int *nr_caps, char ***caps,
					  unsigned int *max_branches,
					  unsigned int *br_cntr_nr,
					  unsigned int *br_cntr_width)
{
	struct perf_pmu_caps *pcaps = NULL;
	char *ptr, **tmp;
	int ret = 0;

	*nr_caps = 0;
	*caps = NULL;

	if (!pmu->nr_caps)
		return 0;

	*caps = calloc(pmu->nr_caps, sizeof(char *));
	if (!*caps)
		return -ENOMEM;

	tmp = *caps;
	list_for_each_entry(pcaps, &pmu->caps, list) {
		if (asprintf(&ptr, "%s=%s", pcaps->name, pcaps->value) < 0) {
			ret = -ENOMEM;
			goto error;
		}

		*tmp++ = ptr;

		if (!strcmp(pcaps->name, "branches"))
			*max_branches = atoi(pcaps->value);
		else if (!strcmp(pcaps->name, "branch_counter_nr"))
			*br_cntr_nr = atoi(pcaps->value);
		else if (!strcmp(pcaps->name, "branch_counter_width"))
			*br_cntr_width = atoi(pcaps->value);
	}
	*nr_caps = pmu->nr_caps;
	return 0;
error:
	while (tmp-- != *caps)
		zfree(tmp);
	zfree(caps);
	*nr_caps = 0;
	return ret;
}

int perf_env__read_core_pmu_caps(struct perf_env *env)
{
	struct pmu_caps *pmu_caps;
	struct perf_pmu *pmu = NULL;
	int nr_pmu, i = 0, j;
	int ret;

	nr_pmu = perf_pmus__num_core_pmus();

	if (!nr_pmu)
		return -ENODEV;

	if (nr_pmu == 1) {
		pmu = perf_pmus__find_core_pmu();
		if (!pmu)
			return -ENODEV;
		ret = perf_pmu__caps_parse(pmu);
		if (ret < 0)
			return ret;
		return __perf_env__read_core_pmu_caps(pmu, &env->nr_cpu_pmu_caps,
						      &env->cpu_pmu_caps,
						      &env->max_branches,
						      &env->br_cntr_nr,
						      &env->br_cntr_width);
	}

	pmu_caps = calloc(nr_pmu, sizeof(*pmu_caps));
	if (!pmu_caps)
		return -ENOMEM;

	while ((pmu = perf_pmus__scan_core(pmu)) != NULL) {
		if (perf_pmu__caps_parse(pmu) <= 0)
			continue;
		ret = __perf_env__read_core_pmu_caps(pmu, &pmu_caps[i].nr_caps,
						     &pmu_caps[i].caps,
						     &pmu_caps[i].max_branches,
						     &pmu_caps[i].br_cntr_nr,
						     &pmu_caps[i].br_cntr_width);
		if (ret)
			goto error;

		pmu_caps[i].pmu_name = strdup(pmu->name);
		if (!pmu_caps[i].pmu_name) {
			ret = -ENOMEM;
			goto error;
		}
		i++;
	}

	env->nr_pmus_with_caps = nr_pmu;
	env->pmu_caps = pmu_caps;

	return 0;
error:
	for (i = 0; i < nr_pmu; i++) {
		for (j = 0; j < pmu_caps[i].nr_caps; j++)
			zfree(&pmu_caps[i].caps[j]);
		zfree(&pmu_caps[i].caps);
		zfree(&pmu_caps[i].pmu_name);
	}
	zfree(&pmu_caps);
	return ret;
}

const char *perf_env__raw_arch(struct perf_env *env)
{
	return env && !perf_env__read_arch(env) ? env->arch : "unknown";
}

int perf_env__nr_cpus_avail(struct perf_env *env)
{
	return env && !perf_env__read_nr_cpus_avail(env) ? env->nr_cpus_avail : 0;
}

void cpu_cache_level__free(struct cpu_cache_level *cache)
{
	zfree(&cache->type);
	zfree(&cache->map);
	zfree(&cache->size);
}

/*
 * Return architecture name in a normalized form.
 * The conversion logic comes from the Makefile.
 */
static const char *normalize_arch(char *arch)
{
	if (!strcmp(arch, "x86_64"))
		return "x86";
	if (arch[0] == 'i' && arch[2] == '8' && arch[3] == '6')
		return "x86";
	if (!strcmp(arch, "sun4u") || !strncmp(arch, "sparc", 5))
		return "sparc";
	if (!strncmp(arch, "aarch64", 7) || !strncmp(arch, "arm64", 5))
		return "arm64";
	if (!strncmp(arch, "arm", 3) || !strcmp(arch, "sa110"))
		return "arm";
	if (!strncmp(arch, "s390", 4))
		return "s390";
	if (!strncmp(arch, "parisc", 6))
		return "parisc";
	if (!strncmp(arch, "powerpc", 7) || !strncmp(arch, "ppc", 3))
		return "powerpc";
	if (!strncmp(arch, "mips", 4))
		return "mips";
	if (!strncmp(arch, "sh", 2) && isdigit(arch[2]))
		return "sh";
	if (!strncmp(arch, "loongarch", 9))
		return "loongarch";

	return arch;
}

const char *perf_env__arch(struct perf_env *env)
{
	char *arch_name;

	if (!env || !env->arch) { /* Assume local operation */
		static struct utsname uts = { .machine[0] = '\0', };
		if (uts.machine[0] == '\0' && uname(&uts) < 0)
			return NULL;
		arch_name = uts.machine;
	} else
		arch_name = env->arch;

	return normalize_arch(arch_name);
}

#if defined(HAVE_LIBTRACEEVENT)
#include "trace/beauty/arch_errno_names.c"
#endif

const char *perf_env__arch_strerrno(struct perf_env *env __maybe_unused, int err __maybe_unused)
{
#if defined(HAVE_LIBTRACEEVENT)
	if (env->arch_strerrno == NULL)
		env->arch_strerrno = arch_syscalls__strerrno_function(perf_env__arch(env));

	return env->arch_strerrno ? env->arch_strerrno(err) : "no arch specific strerrno function";
#else
	return "!HAVE_LIBTRACEEVENT";
#endif
}

const char *perf_env__cpuid(struct perf_env *env)
{
	int status;

	if (!env->cpuid) { /* Assume local operation */
		status = perf_env__read_cpuid(env);
		if (status)
			return NULL;
	}

	return env->cpuid;
}

int perf_env__nr_pmu_mappings(struct perf_env *env)
{
	int status;

	if (!env->nr_pmu_mappings) { /* Assume local operation */
		status = perf_env__read_pmu_mappings(env);
		if (status)
			return 0;
	}

	return env->nr_pmu_mappings;
}

const char *perf_env__pmu_mappings(struct perf_env *env)
{
	int status;

	if (!env->pmu_mappings) { /* Assume local operation */
		status = perf_env__read_pmu_mappings(env);
		if (status)
			return NULL;
	}

	return env->pmu_mappings;
}

int perf_env__numa_node(struct perf_env *env, struct perf_cpu cpu)
{
	if (!env->nr_numa_map) {
		struct numa_node *nn;
		int i, nr = 0;

		for (i = 0; i < env->nr_numa_nodes; i++) {
			nn = &env->numa_nodes[i];
			nr = max(nr, (int)perf_cpu_map__max(nn->map).cpu);
		}

		nr++;

		/*
		 * We initialize the numa_map array to prepare
		 * it for missing cpus, which return node -1
		 */
		env->numa_map = malloc(nr * sizeof(int));
		if (!env->numa_map)
			return -1;

		for (i = 0; i < nr; i++)
			env->numa_map[i] = -1;

		env->nr_numa_map = nr;

		for (i = 0; i < env->nr_numa_nodes; i++) {
			struct perf_cpu tmp;
			int j;

			nn = &env->numa_nodes[i];
			perf_cpu_map__for_each_cpu(tmp, j, nn->map)
				env->numa_map[tmp.cpu] = i;
		}
	}

	return cpu.cpu >= 0 && cpu.cpu < env->nr_numa_map ? env->numa_map[cpu.cpu] : -1;
}

bool perf_env__has_pmu_mapping(struct perf_env *env, const char *pmu_name)
{
	char *pmu_mapping = env->pmu_mappings, *colon;

	for (int i = 0; i < env->nr_pmu_mappings; ++i) {
		if (strtoul(pmu_mapping, &colon, 0) == ULONG_MAX || *colon != ':')
			goto out_error;

		pmu_mapping = colon + 1;
		if (strcmp(pmu_mapping, pmu_name) == 0)
			return true;

		pmu_mapping += strlen(pmu_mapping) + 1;
	}
out_error:
	return false;
}

char *perf_env__find_pmu_cap(struct perf_env *env, const char *pmu_name,
			     const char *cap)
{
	char *cap_eq;
	int cap_size;
	char **ptr;
	int i, j;

	if (!pmu_name || !cap)
		return NULL;

	cap_size = strlen(cap);
	cap_eq = zalloc(cap_size + 2);
	if (!cap_eq)
		return NULL;

	memcpy(cap_eq, cap, cap_size);
	cap_eq[cap_size] = '=';

	if (!strcmp(pmu_name, "cpu")) {
		for (i = 0; i < env->nr_cpu_pmu_caps; i++) {
			if (!strncmp(env->cpu_pmu_caps[i], cap_eq, cap_size + 1)) {
				free(cap_eq);
				return &env->cpu_pmu_caps[i][cap_size + 1];
			}
		}
		goto out;
	}

	for (i = 0; i < env->nr_pmus_with_caps; i++) {
		if (strcmp(env->pmu_caps[i].pmu_name, pmu_name))
			continue;

		ptr = env->pmu_caps[i].caps;

		for (j = 0; j < env->pmu_caps[i].nr_caps; j++) {
			if (!strncmp(ptr[j], cap_eq, cap_size + 1)) {
				free(cap_eq);
				return &ptr[j][cap_size + 1];
			}
		}
	}

out:
	free(cap_eq);
	return NULL;
}

void perf_env__find_br_cntr_info(struct perf_env *env,
				 unsigned int *nr,
				 unsigned int *width)
{
	if (nr) {
		*nr = env->cpu_pmu_caps ? env->br_cntr_nr :
					  env->pmu_caps->br_cntr_nr;
	}

	if (width) {
		*width = env->cpu_pmu_caps ? env->br_cntr_width :
					     env->pmu_caps->br_cntr_width;
	}
}

bool perf_env__is_x86_amd_cpu(struct perf_env *env)
{
	static int is_amd; /* 0: Uninitialized, 1: Yes, -1: No */

	if (is_amd == 0)
		is_amd = env->cpuid && strstarts(env->cpuid, "AuthenticAMD") ? 1 : -1;

	return is_amd >= 1 ? true : false;
}

bool x86__is_amd_cpu(void)
{
	struct perf_env env = { .total_mem = 0, };
	bool is_amd;

	perf_env__cpuid(&env);
	is_amd = perf_env__is_x86_amd_cpu(&env);
	perf_env__exit(&env);

	return is_amd;
}
