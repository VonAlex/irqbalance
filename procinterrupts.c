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
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>

#include "cpumask.h"
#include "irqbalance.h"

#define LINESIZE 4096

static int proc_int_has_msi = 0;
static int msi_found_in_sysfs = 0;

// /proc/interrupts 文件解析，将数字开头的中断号解析成 irq_info 结构，放入 list 中
GList* collect_full_irq_list()
{
	GList *tmp_list = NULL;
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	char *irq_name, *savedptr, *last_token, *p;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return NULL;

	/* first line is the header we don't need; nuke it */
	if (getline(&line, &size, file)==0) {  // 第一行是CPU编号，忽略掉
		free(line);
		fclose(file);
		return NULL;
	}

	while (!feof(file)) {
		int	 number;
		struct irq_info *info;
		char *c;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

		/* lines with letters in front are special, like NMI count. Ignore */
		// 以空格+字母开头的行忽略掉
		c = line;
		while (isblank(*(c))) // 跳过空格,每行开头有空格
			c++;

		// 只在乎数字表示的中换号，NMI/LOC 等开头的行忽略掉,NMI 和 LOC 是系统所使用的驱动，用户无法访问和配置
		if (!(*c>='0' && *c<='9'))
			break;
		c = strchr(line, ':'); // 中断号后面是冒号
		if (!c)
			continue;

		strncpy(savedline, line, sizeof(savedline));

		irq_name = strtok_r(savedline, " ", &savedptr);
		last_token = strtok_r(NULL, " ", &savedptr);
		while ((p = strtok_r(NULL, " ", &savedptr))) {
			irq_name = last_token;  // 中断控制器
			last_token = p;         // 设备名
		}

		*c = 0;
		c++;
		number = strtoul(line, NULL, 10); // 中断号

		info = calloc(sizeof(struct irq_info), 1);
		if (info) {
			infxo->irq = number;
			if (strstr(irq_name, "xen-dyn-event") != NULL) {
				info->type = IRQ_TYPE_VIRT_EVENT;
				info->class = IRQ_VIRT_EVENT;
			} else {
				info->type = IRQ_TYPE_LEGACY;
				info->class = IRQ_OTHER;
			}
			tmp_list = g_list_append(tmp_list, info);
		}

	}
	fclose(file);
	free(line);
	return tmp_list;
}

void parse_proc_interrupts(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return;

	/* first line is the header we don't need; nuke it */
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return;
	}

	while (!feof(file)) {
		int cpunr;
		int	 number;
		uint64_t count;
		char *c, *c2;
		struct irq_info *info;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

        /*判断是否有msi中断*/
		if (!proc_int_has_msi)
			if (strstr(line, "MSI") != NULL)
				proc_int_has_msi = 1;

		/* lines with letters in front are special, like NMI count. Ignore */
		// 仅处理 int 中断号
		c = line;
		while (isblank(*(c)))
			c++;

		if (!(*c>='0' && *c<='9'))
			break;
		c = strchr(line, ':');
		if (!c)
			continue;

		strncpy(savedline, line, sizeof(savedline));

		*c = 0;
		c++;
		number = strtoul(line, NULL, 10);

		info = get_irq_info(number);
		if (!info) {  // 中断表里没有 number 编号的中断，需要重新 scan
			need_rescan = 1;
			break;
		}

		count = 0; // 该 irq 总的中断数，各个 cpu 加起来
		cpunr = 0; // cpu 数

		c2=NULL;
		while (1) {
			uint64_t C;
			C = strtoull(c, &c2, 10);
			if (c==c2) /* end of numbers */
				break;
			count += C;
			c=c2;
			cpunr++;
		}
		if (cpunr != core_count) {
			need_rescan = 1;
			break;
		}

		info->last_irq_count = info->irq_count;
		info->irq_count = count;

		/* is interrupt MSI based? */
		/* 如果有MSI/MSI-X中断，进行标记*/
		if ((info->type == IRQ_TYPE_MSI) || (info->type == IRQ_TYPE_MSIX))
			msi_found_in_sysfs = 1;
	}
	if ((proc_int_has_msi) && (!msi_found_in_sysfs) && (!need_rescan)) {
		log(TO_ALL, LOG_WARNING, "WARNING: MSI interrupts found in /proc/interrupts\n");
		log(TO_ALL, LOG_WARNING, "But none found in sysfs, you need to update your kernel\n");
		log(TO_ALL, LOG_WARNING, "Until then, IRQs will be improperly classified\n");
		/*
 		 * Set msi_foun_in_sysfs, so we don't get this error constantly
 		 */
		msi_found_in_sysfs = 1;
	}
	fclose(file);
	free(line);
}


static void accumulate_irq_count(struct irq_info *info, void *data)
{
	uint64_t *acc = data;

	*acc += (info->irq_count - info->last_irq_count);
}

static void assign_load_slice(struct irq_info *info, void *data)
{
	uint64_t *load_slice = data;
	info->load = (info->irq_count - info->last_irq_count) * *load_slice;

	/*
 	 * Every IRQ has at least a load of 1
 	 */
	if (!info->load)
		info->load++;
}

/*
 * Recursive helper to estimate the number of irqs shared between
 * multiple topology objects that was handled by this particular object
 */
static uint64_t get_parent_branch_irq_count_share(struct topo_obj *d)
{
	uint64_t total_irq_count = 0;

	if (d->parent) {
		total_irq_count = get_parent_branch_irq_count_share(d->parent);
		total_irq_count /= g_list_length(*d->obj_type_list);
	}

	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, accumulate_irq_count, &total_irq_count);

	return total_irq_count; 
}

static void compute_irq_branch_load_share(struct topo_obj *d, void *data __attribute__((unused)))
{
	uint64_t local_irq_counts = 0;
	uint64_t load_slice;
	int	load_divisor = g_list_length(d->children);

	d->load /= (load_divisor ? load_divisor : 1); 

	if (g_list_length(d->interrupts) > 0) {
		local_irq_counts = get_parent_branch_irq_count_share(d);
		load_slice = local_irq_counts ? (d->load / local_irq_counts) : 1;
		for_each_irq(d->interrupts, assign_load_slice, &load_slice);
	}

	if (d->parent)  // 将自身的负载加入到它的 parent
		d->parent->load += d->load;
}

static void reset_load(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (d->parent)
		reset_load(d->parent, NULL);

	d->load = 0;
}

void parse_proc_stat(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	int cpunr, rc, cpucount;
	struct topo_obj *cpu;
	unsigned long long irq_load, softirq_load;

	file = fopen("/proc/stat", "r");
	if (!file) {
		log(TO_ALL, LOG_WARNING, "WARNING cant open /proc/stat.  balacing is broken\n");
		return;
	}

	/* first line is the header we don't need; nuke it */
	if (getline(&line, &size, file)==0) { // 第一行为 cpu 信息汇总，不做处理
		free(line);
		log(TO_ALL, LOG_WARNING, "WARNING read /proc/stat. balancing is broken\n");
		fclose(file);
		return;
	}

	cpucount = 0;
	while (!feof(file)) {
		if (getline(&line, &size, file)==0)
			break;

		if (!strstr(line, "cpu")) // 仅处理包含 cpu 字段的行
			break;

		cpunr = strtoul(&line[3], NULL, 10); // cpuname

		if (cpu_isset(cpunr, banned_cpus)) // 被 ban 的 cpu 不统计
			continue;

		rc = sscanf(line, "%*s %*u %*u %*u %*u %*u %llu %llu", &irq_load, &softirq_load); // 第 7 行为硬中断，第 8 行为软中断
		if (rc < 2)
			break;

		cpu = find_cpu_core(cpunr); // 从 cpus 中获取之前放入的 cpu，以备填充 load 和 last_load 字段

		if (!cpu)
			break;

		cpucount++; // CPU 计数器

		/*
 		 * For each cpu add the irq and softirq load and propagate that
 		 * all the way up the device tree
		 * 对于每一个 cpu 结构，将 irq and softirq 叠加，并放入 device tree
 		 */
		if (cycle_count) {
			cpu->load = (irq_load + softirq_load) - (cpu->last_load); // 当前的负载与上次的做 diff
			/*
			 * the [soft]irq_load values are in jiffies, with
			 * HZ jiffies per second.  Convert the load to nanoseconds
			 * to get a better integer resolution of nanoseconds per
			 * interrupt.
			 */
			cpu->load *= NSEC_PER_SEC/HZ; // 结果转换成 ns
		}
		cpu->last_load = (irq_load + softirq_load);
	}

	fclose(file);
	free(line);
	if (cpucount != get_cpu_count()) {
		log(TO_ALL, LOG_WARNING, "WARNING, didn't collect load info for all cpus, balancing is broken\n");
		return;
	}

	/*
 	 * Reset the load values for all objects above cpus
	 * 重置 CPU 域以上的结构域的负载值，因为需要重新计算
 	 */
	for_each_object(cache_domains, reset_load, NULL);

	/*
 	 * Now that we have load for each cpu attribute a fair share of the load
 	 * to each irq on that cpu
 	 */
	for_each_object(cpus, compute_irq_branch_load_share, NULL);
	for_each_object(cache_domains, compute_irq_branch_load_share, NULL);
	for_each_object(packages, compute_irq_branch_load_share, NULL);
	for_each_object(numa_nodes, compute_irq_branch_load_share, NULL);

}
