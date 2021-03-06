/* Copyright (c) 2013, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp_posix_extensions.h>

#include <odp/api/system_info.h>
#include <odp_internal.h>
#include <odp_debug_internal.h>
#include <odp/api/align.h>
#include <odp/api/cpu.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#ifdef HAVE_THUNDERX
#include <errno.h>
#endif

/* sysconf */
#include <unistd.h>
#include <sys/sysinfo.h>

/* opendir, readdir */
#include <sys/types.h>
#include <dirent.h>

#define CACHE_LNSZ_FILE \
	"/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size"

/*
 * Report the number of logical CPUs detected at boot time
 */
static int sysconf_cpu_count(void)
{
	return odp_global_data.num_cpus_installed;
}

#if defined __x86_64__ || defined __i386__ || defined __OCTEON__ || \
defined __powerpc__
/*
 * Analysis of /sys/devices/system/cpu/ files
 */
static int systemcpu_cache_line_size(void)
{
	FILE  *file;
	char str[128];
	int size = 0;

	file = fopen(CACHE_LNSZ_FILE, "rt");
	if (file == NULL) {
		/* File not found */
		return 0;
	}

	if (fgets(str, sizeof(str), file) != NULL) {
		/* Read cache line size */
		if (sscanf(str, "%i", &size) != 1)
			size = 0;
	}

	fclose(file);

	return size;
}

#else
/*
 * Use dummy data if not available from /sys/devices/system/cpu/
 */
static int systemcpu_cache_line_size(void)
{
#ifdef HAVE_THUNDERX
	return ODP_CACHE_LINE_SIZE;
#else
	return 64;
#endif
}
#endif

/* required by the ODP ThunderX shared memory */
#ifdef HAVE_THUNDERX
static int huge_page_dir(system_info_t *sysinfo)
{
	FILE *f;
	const char *mounts = "/proc/mounts";
	const char *hugefs = "hugetlbfs";
	char line[PATH_MAX] = {0};
	char *token;
	const size_t ncols = 3;
	char *cols[ncols];
	size_t i;

	f = fopen(mounts,"r");
	if (!f) {
		ODP_ERR("fopen(%s):%s\n", mounts, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		token = strtok(line, " ");
		for (i = 0; token != NULL && i < ncols; i++) {
			cols[i] = token;
			token = strtok(NULL," ");
		}
		if (i != ncols) {
			ODP_ERR("Error parsing %s: %s\n", mounts, line);
			return -1;
		}
		if (strcmp(cols[2], hugefs) == 0) {
			strncpy(sysinfo->huge_page_dir, cols[1], PATH_MAX);
			sysinfo->huge_page_dir[PATH_MAX-1] = '\0';
			ODP_DBG("found huge page dir: %s\n", sysinfo->huge_page_dir);
			return 0;
		}
	}

	fclose(f);
	return -1;
}
#endif

static uint64_t default_huge_page_size(void)
{
	char str[1024];
	unsigned long sz;
	FILE *file;

	file = fopen("/proc/meminfo", "rt");

	while (fgets(str, sizeof(str), file) != NULL) {
		if (sscanf(str, "Hugepagesize:   %8lu kB", &sz) == 1) {
			ODP_DBG("defaut hp size is %" PRIu64 " kB\n", sz);
			fclose(file);
			return (uint64_t)sz * 1024;
		}
	}

	ODP_ERR("unable to get default hp size\n");
	fclose(file);
	return 0;
}

/*
 * Analysis of /sys/devices/system/cpu/ files
 */
static int systemcpu(system_info_t *sysinfo)
{
	int ret;

	ret = sysconf_cpu_count();
	if (ret == 0) {
		ODP_ERR("sysconf_cpu_count failed.\n");
		return -1;
	}

	sysinfo->cpu_count = ret;


	ret = systemcpu_cache_line_size();
	if (ret == 0) {
		ODP_ERR("systemcpu_cache_line_size failed.\n");
		return -1;
	}

	sysinfo->cache_line_size = ret;

	if (ret != ODP_CACHE_LINE_SIZE) {
		ODP_ERR("Cache line sizes definitions don't match.\n");
		return -1;
	}

	sysinfo->default_huge_page_size = default_huge_page_size();

	return 0;
}


/*
 * System info initialisation
 */
int odp_system_info_init(void)
{
	FILE  *file;
/* required by the ODP ThunderX shared memory */
#if HAVE_THUNDERX
	long sc_pagesz;
#endif

	memset(&odp_global_data.system_info, 0, sizeof(system_info_t));

/* required by the ODP ThunderX shared memory */
#if HAVE_THUNDERX
	sc_pagesz = sysconf(_SC_PAGESIZE);
	if (sc_pagesz < 0) {
		ODP_ERR("sysconf(_SC_PAGESIZE): %s", strerror(errno));
		return -1;
	}
	odp_global_data.system_info.page_size = sc_pagesz;

	if (huge_page_dir(&odp_global_data.system_info)) {
		ODP_ERR("Cannot get hugepage dir from mountpoints. Please, mount hugetlbfs first.\n");
		return -1;
	}
#else
	odp_global_data.system_info.page_size = ODP_PAGE_SIZE;
#endif

	file = fopen("/proc/cpuinfo", "rt");
	if (file == NULL) {
		ODP_ERR("Failed to open /proc/cpuinfo\n");
		return -1;
	}

	cpuinfo_parser(file, &odp_global_data.system_info);

	fclose(file);

	if (systemcpu(&odp_global_data.system_info)) {
		ODP_ERR("systemcpu failed\n");
		return -1;
	}

	return 0;
}

/*
 * System info termination
 */
int odp_system_info_term(void)
{
	return 0;
}

/*
 *************************
 * Public access functions
 *************************
 */
uint64_t odp_cpu_hz(void)
{
	int id = sched_getcpu();

	return odp_cpu_hz_current(id);
}

uint64_t odp_cpu_hz_id(int id)
{
	return odp_cpu_hz_current(id);
}

uint64_t odp_cpu_hz_max(void)
{
	return odp_cpu_hz_max_id(0);
}

uint64_t odp_cpu_hz_max_id(int id)
{
	if (id >= 0 && id < MAX_CPU_NUMBER)
		return odp_global_data.system_info.cpu_hz_max[id];
	else
		return 0;
}

uint64_t odp_sys_huge_page_size(void)
{
	return odp_global_data.system_info.default_huge_page_size;
}

/* required by the ODP ThunderX shared memory */
/* TODO: move prototype to internal header */
#ifdef HAVE_THUNDERX
const char *odp_sys_huge_page_dir(void);
const char *odp_sys_huge_page_dir(void)
{
	return odp_global_data.system_info.huge_page_dir;
}
#endif

uint64_t odp_sys_page_size(void)
{
	return odp_global_data.system_info.page_size;
}

const char *odp_cpu_model_str(void)
{
	return odp_cpu_model_str_id(0);
}

const char *odp_cpu_model_str_id(int id)
{
	if (id >= 0 && id < MAX_CPU_NUMBER)
		return odp_global_data.system_info.model_str[id];
	else
		return NULL;
}

int odp_sys_cache_line_size(void)
{
	return odp_global_data.system_info.cache_line_size;
}

int odp_cpu_count(void)
{
	return odp_global_data.system_info.cpu_count;
}
