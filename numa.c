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
 * This file tries to map numa affinity of pci devices to their interrupts
 * In addition the PCI class information is used to refine the classification
 * of interrupt sources
 */
#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

#include "irqbalance.h"

#define SYSFS_NODE_PATH "/sys/devices/system/node"

GList *numa_nodes = NULL;

static struct topo_obj unspecified_node_template = {  // 模板，初始化
	.load = 0,
	.number = -1,
	.obj_type = OBJ_TYPE_NODE,
	.mask = CPU_MASK_ALL,
	.interrupts = NULL,
	.children = NULL,
	.parent = NULL,
	.obj_type_list = &numa_nodes,
};

static struct topo_obj unspecified_node;

// 根据节点名字，往 numa_nodes list 中添加一个 node 结构
static void add_one_node(const char *nodename)
{
	char path[PATH_MAX];
	struct topo_obj *new;
	char *cpustr = NULL;
	FILE *f;
	ssize_t ret;
	size_t blen;

	new = calloc(1, sizeof(struct topo_obj)); // 分配一块 topo_obj 大小的内存，若失败，直接返回
	if (!new)
		return;
	sprintf(n, "%s/%s/cpumap", SYSFS_NODE_PATH, nodename); // path=/sys/devices/system/node/nodename/cpumap, nodename=node0/1..
	f = fopen(path, "r");
	if (!f) {
		free(new);
		return;
	}
	if (ferror(f)) {
		cpus_clear(new->mask); // 将 bitmap 先清空
	} else {
		ret = getline(&cpustr, &blen, f);               // cpustr 得到一个 hex 的字符串
		if (ret <= 0) {
			cpus_clear(new->mask);
		} else {
			cpumask_parse_user(cpustr, ret, new->mask); // 将 cpustr (如 ffff,ffffffff) 解析到 bitmap 中
			free(cpustr);
		}
	}
	fclose(f);
	new->obj_type = OBJ_TYPE_NODE;                     // 类型为 numa node
	new->number = strtoul(&nodename[4], NULL, 10);     // node 序号从文件名获取，如 0/1..等
	new->obj_type_list = &numa_nodes;
	numa_nodes = g_list_append(numa_nodes, new);      // 将新node 添加到 numa_nodes 这个 list 上
}

// 新建一个 numa_nodes list，并将所有的 numa node 加进去, 如果支持 numa 结构的话
void build_numa_node_list(void)
{
	DIR *dir;
	struct dirent *entry;

	/*
	 * Note that we copy the unspcified node from the template here
	 * in the event we just freed the object tree during a rescan.
	 * This ensures we don't get stale list pointers anywhere
	 */
	// 利用模版结构 unspecified_node_template 创建一个 numa node 结构
	memcpy(&unspecified_node, &unspecified_node_template, sizeof (struct topo_obj));

	/*
	 * Add the unspecified node
	 */
	// 将该结构加入到NUMA域链表中，作为一个无实际意义的头结点 dummy head
	numa_nodes = g_list_append(numa_nodes, &unspecified_node);

	if (!numa_avail) // 不支持 numa 结构， 直接返回
		return;

	dir = opendir(SYSFS_NODE_PATH); // /sys/devices/system/node
	if (!dir)
		return;
	// 遍历目录 /sys/devices/system/node
	do {
		entry = readdir(dir);
		if (!entry) // 读文件异常
			break;
		if ((entry->d_type == DT_DIR) && (strstr(entry->d_name, "node"))) { // 名字包含 node 的目录,看有几个 numa node，d_type 表示文件类型，d_name 表示文件名
			add_one_node(entry->d_name); // node0 或者 node1
		}
	} while (entry);
	closedir(dir);
}

static void free_numa_node(gpointer data)
{
	struct topo_obj *obj = data;
	g_list_free(obj->children);
	g_list_free(obj->interrupts);

	if (data != &unspecified_node)
		free(data);
}

// glist 释放内存
void free_numa_node_list(void)
{
	g_list_free_full(numa_nodes, free_numa_node);
	numa_nodes = NULL;
}

// 比较 2 个节点的 numa number 是否相同，相同返回 0，否则返回 1
static gint compare_node(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return (ai->number == bi->number) ? 0 : 1;
}
// 将一个 package 结构加入到 nodeid 这个 node 结构
void add_package_to_node(struct topo_obj *p, int nodeid)
{
	struct topo_obj *node;

	node = get_numa_node(nodeid);

	if (!node) {
		log(TO_CONSOLE, LOG_INFO, "Could not find numa node for node id %d\n", nodeid);
		return;
	}


	if (!p->parent) {
		node->children = g_list_append(node->children, p);
		p->parent = node;
	}
}

// 打印 numa_node 节点的信息包括 number 和 cpu mask
// __attribute__((unused)) 表示该函数或变量可能不使用，这个属性可以避免编译器产生警告信息
void dump_numa_node_info(struct topo_obj *d, void *unused __attribute__((unused)))
{
	char buffer[4096];
	log(TO_CONSOLE, LOG_INFO, "NUMA NODE NUMBER: %d\n", d->number);
	cpumask_scnprintf(buffer, 4096, d->mask);  // 将 bitmap 形式的掩码转换成 char*
	log(TO_CONSOLE, LOG_INFO, "LOCAL CPU MASK: %s\n", buffer);
	log(TO_CONSOLE, LOG_INFO, "\n");
}

// 根据 nodeid 从 numa_nodes list 中获得 numa 节点， 不存在的话返回 null
struct topo_obj *get_numa_node(int nodeid)
{
	struct topo_obj find;
	GList *entry;

	if (!numa_avail)
		return &unspecified_node;

	if (nodeid == -1)
		return &unspecified_node;

	find.number = nodeid;

	entry = g_list_find_custom(numa_nodes, &find, compare_node);
	return entry ? entry->data : NULL;
}
