/*
 * Copyright (C) 2006, Intel Corporation
 * Copyright (C) 2012, Neil Horman <nhorman@tuxdriver.com>
 *
 * This file is part of irqbalance
 *
 * This program file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named COPYING; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 */

/*
 * This file contains the code to construct and manipulate a hierarchy of processors,
 * cache domains and processor cores.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <glib.h>

#include "irqbalance.h"


GList *cpus;
GList *cache_domains;
GList *packages;

int package_count;
int cache_domain_count;
int core_count;

/* Users want to be able to keep interrupts away from some cpus; store these in a cpumask_t */
cpumask_t banned_cpus;

cpumask_t cpu_possible_map;

/*
   it's convenient to have the complement of banned_cpus available so that
   the AND operator can be used to mask out unwanted cpus
*/
cpumask_t unbanned_cpus;

// 将 cache_domain 结构加入到指定的 package 结构中，如果不存在 packageid 的 package结构，则在 package 结构 list 队尾增加一个 packageid 的 package 结构，并将 cache_domain 结构插入
static struct topo_obj* add_cache_domain_to_package(struct topo_obj *cache,
						    int packageid, cpumask_t package_mask)
{
	GList *entry;
	struct topo_obj *package;
	struct topo_obj *lcache;

	entry = g_list_first(packages);

	while (entry) {
		package = entry->data;
		if (cpus_equal(package_mask, package->mask)) {
			if (packageid != package->number)
				log(TO_ALL, LOG_WARNING, "package_mask with different physical_package_id found!\n");
			break;
		}
		entry = g_list_next(entry);
	}

	if (!entry) {
		package = calloc(sizeof(struct topo_obj), 1);
		if (!package)
			return NULL;
		package->mask = package_mask;
		package->obj_type = OBJ_TYPE_PACKAGE;
		package->obj_type_list = &packages;
		package->number = packageid;
		packages = g_list_append(packages, package);
		package_count++;
	}

	entry = g_list_first(package->children);
	while (entry) {
		lcache = entry->data;
		if (lcache == cache)
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		package->children = g_list_append(package->children, cache);
		cache->parent = package;
	}

	return package;
}
// 将 cpu 结构加入到指定的 cache结构中，如果指定 cache结构不存在，则在 cache结构 list 队尾增加一个指定的 cache结构，并将 cpu 结构插入
// 这里传入的 cache_mask 为 L3 缓存共享 cpu map
static struct topo_obj* add_cpu_to_cache_domain(struct topo_obj *cpu,
						    cpumask_t cache_mask)
{
	GList *entry;
	struct topo_obj *cache;
	struct topo_obj *lcpu;

	entry = g_list_first(cache_domains);

	while (entry) { // 遍历 cache_domains list
		cache = entry->data;
		if (cpus_equal(cache_mask, cache->mask))
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {  // cache_mask 不在已有的 cache_domains 列表中
		cache = calloc(sizeof(struct topo_obj), 1);
		if (!cache)
			return NULL;
		cache->obj_type = OBJ_TYPE_CACHE;
		cache->mask = cache_mask;
		cache->number = cache_domain_count;
		cache->obj_type_list = &cache_domains;
		cache_domains = g_list_append(cache_domains, cache);
		cache_domain_count++;
	}

	entry = g_list_first(cache->children);
	while (entry) {
		lcpu = entry->data;
		if (lcpu == cpu)
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		cache->children = g_list_append(cache->children, cpu);
		cpu->parent = (struct topo_obj *)cache;
	}

	return cache;
}

static void do_one_cpu(char *path)  // path = "/sys/devices/system/cpu/cpu0" 等
{
	struct topo_obj *cpu;
	FILE *file;
	char new_path[PATH_MAX];
	cpumask_t cache_mask, package_mask;
	struct topo_obj *cache;
	struct topo_obj *package;
	DIR *dir;
	struct dirent *entry;
	int nodeid;
	int packageid = 0;
	unsigned int max_cache_index, cache_index, cache_stat;

	/* skip offline cpus */
	snprintf(new_path, PATH_MAX, "%s/online", path); // /sys/devices/system/cpu/cpu#/online, # 表示 cpu 编号
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)==0)
			return;
		fclose(file);
		if (line && line[0]=='0') {  // offline 的 cpu 该文件内容为 0
			free(line);
			return;                  // 忽略掉下线的 cpu
		}
		free(line);
	}

	cpu = calloc(sizeof(struct topo_obj), 1);
	if (!cpu)
		return;

	cpu->obj_type = OBJ_TYPE_CPU;
	cpu->number = strtoul(&path[27], NULL, 10); // 从路径截取 cpu 编号，转换成 10 进制，这个也可以从 topology/core_id 文件获取
	cpu_set(cpu->number, cpu_possible_map);    // 根据 cpu 编号设置 bitmap
	cpu_set(cpu->number, cpu->mask);

	/*
 	 * Default the cache_domain mask to be equal to the cpu
 	 */
	cpus_clear(cache_mask);
	cpu_set(cpu->number, cache_mask);

	// 如果当前 cpu 编号被 cpu 黑名单，那么就不添加它
	if (cpus_intersects(cpu->mask, banned_cpus)) { // 两者相与，如果存在都为1的位,返回1,不存在返回0
		free(cpu);
		/* even though we don't use the cpu we do need to count it */
		core_count++;
		return;
	}


	/* try to read the package mask; if it doesn't exist assume solitary */

	// core_siblings：在同一个物理 package 下 cpu#'s 硬件线程的内部 kernel map
	snprintf(new_path, PATH_MAX, "%s/topology/core_siblings", path); // /sys/devices/system/cpu/cpu#/topology/core_siblings
	file = fopen(new_path, "r");
	cpu_set(cpu->number, package_mask);
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file))
			cpumask_parse_user(line, strlen(line), package_mask); // 将 bitmap package_mask 表示的值转换成字符串 line
		fclose(file);
		free(line);
	}
	/* try to read the package id */
	snprintf(new_path, PATH_MAX, "%s/topology/physical_package_id", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file))
			packageid = strtoul(line, NULL, 10);
		fclose(file);
		free(line);
	}

	/* try to read the cache mask; if it doesn't exist assume solitary */
	/* We want the deepest cache level available */
	cpu_set(cpu->number, cache_mask);
	max_cache_index = 0;
	cache_index = 1;
	cache_stat = 0;
	do {
		struct stat sb;
		snprintf(new_path, PATH_MAX, "%s/cache/index%d/shared_cpu_map", path, cache_index); // 与该 cpu 共享这一级缓存的 cpu 编号表，二进制字符串，如 0000,01000001
		cache_stat = stat(new_path, &sb);
		if (!cache_stat) {
			max_cache_index = cache_index;
			if (max_cache_index == deepest_cache)
				break;
			cache_index ++;
		}
	} while(!cache_stat); // 遍历 L1 ~ L3 级缓存

	if (max_cache_index > 0) {
		snprintf(new_path, PATH_MAX, "%s/cache/index%d/shared_cpu_map", path, max_cache_index); // L3 级别缓存
		file = fopen(new_path, "r");
		if (file) {
			char *line = NULL;
			size_t size = 0;
			if (getline(&line, &size, file))
				cpumask_parse_user(line, strlen(line), cache_mask); // L3 存在于物理核中， 被多个 core 共享
			fclose(file);
			free(line);
		}
	}

	nodeid=-1;
	if (numa_avail) {
		dir = opendir(path);
		do {
			entry = readdir(dir);
			if (!entry)
				break;
			if (strstr(entry->d_name, "node")) {
				nodeid = strtoul(&entry->d_name[4], NULL, 10); // 获得 nodeid，从目录截取
				break;
			}
		} while (entry);
		closedir(dir);
	}

	/*
	   blank out the banned cpus from the various masks so that interrupts
	   will never be told to go there
	 */
	// 将 cache_mask 和 package_mask 中存在的被 ban 的 cpu 去掉，保证 cpu 是 unbanned
	cpus_and(cache_mask, cache_mask, unbanned_cpus);
	cpus_and(package_mask, package_mask, unbanned_cpus);

    // 以下三个函数构建起基本架构，设置 parent 和 children
	cache = add_cpu_to_cache_domain(cpu, cache_mask);
	package = add_cache_domain_to_package(cache, packageid, package_mask);
	add_package_to_node(package, nodeid);

	cpu->obj_type_list = &cpus;
	cpus = g_list_append(cpus, cpu);
	core_count++;
}

static void dump_irq(struct irq_info *info, void *data)
{
	int spaces = (long int)data;
	int i;
	for (i=0; i<spaces; i++) log(TO_CONSOLE, LOG_INFO, " ");
	log(TO_CONSOLE, LOG_INFO, "Interrupt %i node_num is %d (%s/%u) \n",
	    info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned int)info->load);
}

static void dump_topo_obj(struct topo_obj *d, void *data __attribute__((unused)))
{
	struct topo_obj *c = (struct topo_obj *)d;
	log(TO_CONSOLE, LOG_INFO, "                CPU number %i  numa_node is %d (load %lu)\n",
	    c->number, cpu_numa_node(c)->number , (unsigned long)c->load);
	if (c->interrupts)
		for_each_irq(c->interrupts, dump_irq, (void *)18);
}

static void dump_cache_domain(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4095, d->mask);
	log(TO_CONSOLE, LOG_INFO, "        Cache domain %i:  numa_node is %d cpu mask is %s  (load %lu) \n",
	    d->number, cache_domain_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_object(d->children, dump_topo_obj, NULL);
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, dump_irq, (void *)10);
}

static void dump_package(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4096, d->mask);
	log(TO_CONSOLE, LOG_INFO, "Package %i:  numa_node is %d cpu mask is %s (load %lu)\n",
	    d->number, package_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_object(d->children, dump_cache_domain, buffer);
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, dump_irq, (void *)2);
}

void dump_tree(void)
{
	char buffer[4096];
	for_each_object(packages, dump_package, buffer);
}

static void clear_irq_stats(struct irq_info *info, void *data __attribute__((unused)))
{
	info->load = 0;
}

static void clear_obj_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	for_each_object(d->children, clear_obj_stats, NULL);
	for_each_irq(d->interrupts, clear_irq_stats, NULL);
}

/*
 * this function removes previous state from the cpu tree, such as
 * which level does how much work and the actual lists of interrupts
 * assigned to each component
 */
void clear_work_stats(void)
{
	for_each_object(numa_nodes, clear_obj_stats, NULL);
}

// 遍历系统所有 cpu, 数据来源 /sys/devices/system/cpu/cpu#
void parse_cpu_tree(void)
{
	DIR *dir;
	struct dirent *entry;

	cpus_complement(unbanned_cpus, banned_cpus); // banned_cpus （IRQBALANCE_BANNED_CPUS）按位取反得到 unbanned_cpus

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;
	do {
		int num;
		char pad;
		entry = readdir(dir);
		/*
		 * cpufreq/cpuidle 目录不统计，仅统计 cpuX 的目录
 		 */
		if (entry &&
		    sscanf(entry->d_name, "cpu%d%c", &num, &pad) == 1 &&
		    !strchr(entry->d_name, ' ')) {
			char new_path[PATH_MAX];
			sprintf(new_path, "/sys/devices/system/cpu/%s", entry->d_name); // entry->d_name 为 cpu 序号
			do_one_cpu(new_path);
		}
	} while (entry);
	closedir(dir);

	if (debug_mode)
		dump_tree();
}


/*
 * This function frees all memory related to a cpu tree so that a new tree
 * can be read
 */
void clear_cpu_tree(void)
{
	GList *item;
	struct topo_obj *cpu;
	struct topo_obj *cache_domain;
	struct topo_obj *package;

	while (packages) {
		item = g_list_first(packages);
		package = item->data;
		g_list_free(package->children);
		g_list_free(package->interrupts);
		free(package);
		packages = g_list_delete_link(packages, item);
	}
	package_count = 0;

	while (cache_domains) {
		item = g_list_first(cache_domains);
		cache_domain = item->data;
		g_list_free(cache_domain->children);
		g_list_free(cache_domain->interrupts);
		free(cache_domain);
		cache_domains = g_list_delete_link(cache_domains, item);
	}
	cache_domain_count = 0;


	while (cpus) {
		item = g_list_first(cpus);
		cpu = item->data;
		g_list_free(cpu->interrupts);
		free(cpu);
		cpus = g_list_delete_link(cpus, item);
	}
	core_count = 0;

}

static gint compare_cpus(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return ai->number - bi->number;
}

struct topo_obj *find_cpu_core(int cpunr)
{
	GList *entry;
	struct topo_obj find;

	find.number = cpunr;
	entry = g_list_find_custom(cpus, &find, compare_cpus);

	return entry ? entry->data : NULL;
}

int get_cpu_count(void)
{
	return g_list_length(cpus);
}
