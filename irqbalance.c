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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#endif

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif
#include "irqbalance.h"

// volatile 告诉编译器在编译的时候不要优化掉这个变量
volatile int keep_going = 1;

// 3 种 mode
int one_shot_mode;
int debug_mode;
int foreground_mode;

int numa_avail;
int need_rescan;
unsigned int log_mask = TO_ALL;
enum hp_e hint_policy = HINT_POLICY_SUBSET;
unsigned long power_thresh = ULONG_MAX;
unsigned long deepest_cache = ULONG_MAX;
unsigned long long cycle_count = 0;
char *pidfile = NULL;
char *banscript = NULL;
char *polscript = NULL;
long HZ;

void sleep_approx(int seconds)
{
	struct timespec ts;    // 包含 s 和 ns
	struct timeval tv;     // 包含 s 和 us
	gettimeofday(&tv, NULL); // 获取时间精确到 us
	ts.tv_sec = seconds;
	ts.tv_nsec = -tv.tv_usec*1000;
	while (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}
	nanosleep(&ts, NULL); 	// ns 精度设置睡眠时间
}

#ifdef HAVE_GETOPT_LONG
struct option lopts[] = {
	{"oneshot", 0, NULL, 'o'},
	{"debug", 0, NULL, 'd'},
	{"foreground", 0, NULL, 'f'},
	{"hintpolicy", 1, NULL, 'h'},
	{"powerthresh", 1, NULL, 'p'},
	{"banirq", 1 , NULL, 'i'},
	{"banscript", 1, NULL, 'b'},
	{"deepestcache", 1, NULL, 'c'},
	{"policyscript", 1, NULL, 'l'},
	{"pid", 1, NULL, 's'},
	{0, 0, 0, 0}
};

static void usage(void)
{
	log(TO_CONSOLE, LOG_INFO, "irqbalance [--oneshot | -o] [--debug | -d] [--foreground | -f] [--hintpolicy= | -h [exact|subset|ignore]]\n");
	log(TO_CONSOLE, LOG_INFO, "	[--powerthresh= | -p <off> | <n>] [--banirq= | -i <n>] [--policyscript=<script>] [--pid= | -s <file>] [--deepestcache= | -c <n>]\n");
}

static void parse_command_line(int argc, char **argv)
{
	int opt;
	int longind;
	unsigned long val;

	while ((opt = getopt_long(argc, argv,
		"odfh:i:p:s:c:b:l:",
		lopts, &longind)) != -1) {

		switch(opt) {
			case '?':
				usage();
				exit(1);
				break;
			case 'b':
#ifndef INCLUDE_BANSCRIPT
				/*
				 * Banscript is no longer supported unless
				 * explicitly enabled
				 */
				log(TO_CONSOLE, LOG_INFO, "--banscript is not supported on this version of irqbalance, please use --polscript");
				usage();
				exit(1);
#endif
				banscript = strdup(optarg);
				break;
			case 'c':
				deepest_cache = strtoul(optarg, NULL, 10);
				if (deepest_cache == ULONG_MAX || deepest_cache < 1) {
					usage();
					exit(1);
				}
				break;
			case 'd':
				debug_mode=1;
				foreground_mode=1;
				break;
			case 'f':
				foreground_mode=1;
				break;
			case 'h':
				if (!strncmp(optarg, "exact", strlen(optarg)))
					hint_policy = HINT_POLICY_EXACT;
				else if (!strncmp(optarg, "subset", strlen(optarg)))
					hint_policy = HINT_POLICY_SUBSET;
				else if (!strncmp(optarg, "ignore", strlen(optarg)))
					hint_policy = HINT_POLICY_IGNORE;
				else {
					usage();
					exit(1);
				}
				break;
			case 'i':
				val = strtoull(optarg, NULL, 10);
				if (val == ULONG_MAX) {
					usage();
					exit(1);
				}
				add_banned_irq((int)val);
				break;
			case 'l':
				polscript = strdup(optarg);
				break;
			case 'p':
				if (!strncmp(optarg, "off", strlen(optarg)))
					power_thresh = ULONG_MAX;
				else {
					power_thresh = strtoull(optarg, NULL, 10);
					if (power_thresh == ULONG_MAX) {
						usage();
						exit(1);
					}
				}
				break;
			case 'o':
				one_shot_mode=1;
				break;
			case 's':
				pidfile = optarg;
				break;
		}
	}
}
#endif

/*
 * 创建 object tree，层次结构很明确
 * 最顶层是 numa_nodes，向下以此为CPU packages、Cache domains以及CPU cores
 *
 * objects 以上结构自顶而下创建
 * 一个 Object 的负载是它下面所有 objects 负载之和
 */
static void build_object_tree(void)
{
	build_numa_node_list();
	parse_cpu_tree();
	rebuild_irq_db();
}

static void free_object_tree(void)
{
	free_numa_node_list();
	clear_cpu_tree();
	free_irq_db();
}

static void dump_object_tree(void)
{
	for_each_object(numa_nodes, dump_numa_node_info, NULL);
}

// 将中断加入到迁移表中
void force_rebalance_irq(struct irq_info *info, void *data __attribute__((unused)))
{
	if (info->level == BALANCE_NONE)
		return;

	if (info->assigned_obj == NULL)
		rebalance_irq_list = g_list_append(rebalance_irq_list, info);
	else
		migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);

	info->assigned_obj = NULL;
}

static void handler(int signum)
{
	(void)signum;
	keep_going = 0;
}

static void force_rescan(int signum)
{
	(void)signum;
	if (cycle_count)
		need_rescan = 1;
}

int main(int argc, char** argv)
{
	struct sigaction action, hupaction;

// 确定进程运行模式 debug、foreground 或者 oneshot
#ifdef HAVE_GETOPT_LONG
	parse_command_line(argc, argv);
#else
	if (argc>1 && strstr(argv[1],"--debug")) {
		debug_mode=1;
		foreground_mode=1;
	}
	if (argc>1 && strstr(argv[1],"--foreground"))
		foreground_mode=1;
	if (argc>1 && strstr(argv[1],"--oneshot"))
		one_shot_mode=1;
#endif

	/*
 	 * Open the syslog connection
 	 */
	openlog(argv[0], 0, LOG_DAEMON);

// 通过环境变量获得要禁用 cpu，并解析成 bitmap
	if (getenv("IRQBALANCE_BANNED_CPUS"))  {
		cpumask_parse_user(getenv("IRQBALANCE_BANNED_CPUS"), strlen(getenv("IRQBALANCE_BANNED_CPUS")), banned_cpus);
	}

// 通过环境变量的方式设置运行模式
	if (getenv("IRQBALANCE_ONESHOT"))
		one_shot_mode=1;

	if (getenv("IRQBALANCE_DEBUG"))
		debug_mode=1;

	/*
 	 * If we are't in debug mode, don't dump anything to the console
 	 * note that everything goes to the console before we check this
	  *
	  * 如果不是 debug 模式， 不在 console 端打印出任何东西，
	  * 在我们检查之前，所有的东西还是要打印到 console 端的
 	 */
	if (!debug_mode)
		log_mask &= ~TO_CONSOLE;

	if (numa_available() > -1) {  // 默认 -1
		numa_avail = 1;
	} else
		log(TO_CONSOLE, LOG_INFO, "This machine seems not NUMA capable.\n");

	if (banscript) {
		char *note = "Please note that --banscript is deprecated, please use --policyscript instead";
		log(TO_ALL, LOG_WARNING, "%s\n", note);
	}

	HZ = sysconf(_SC_CLK_TCK); // the number of clock ticks per second，时钟频率
	if (HZ == -1) {
		log(TO_ALL, LOG_WARNING, "Unable to determin HZ defaulting to 100\n");
		HZ = 100;
	}

	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);

	build_object_tree();
	if (debug_mode)   // debug 模式下， 打印 numa_node 节点的信息包括 number 和 cpu mask
		dump_object_tree();


	/* On single core UP systems irqbalance obviously has no work to do */
	// 单核 cpu 没必要做 irqbalance
	if (core_count<2) {
		char *msg = "Balancing is ineffective on systems with a "
			    "single cpu.  Shutting down\n";

		log(TO_ALL, LOG_WARNING, "%s", msg);
		exit(EXIT_SUCCESS);
	}

	if (!foreground_mode) {
		int pidfd = -1;
		if (daemon(0,0))
			exit(EXIT_FAILURE);
		/* Write pidfile */
		if (pidfile && (pidfd = open(pidfile,
			O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) >= 0) {
			char str[16];
			snprintf(str, sizeof(str), "%u\n", getpid());
			write(pidfd, str, strlen(str));
			close(pidfd);
		}
	}


#ifdef HAVE_LIBCAP_NG
	// Drop capabilities
	capng_clear(CAPNG_SELECT_BOTH);
	capng_lock();
	capng_apply(CAPNG_SELECT_BOTH);
#endif

	for_each_irq(NULL, force_rebalance_irq, NULL);

	parse_proc_interrupts(); // 主要是计算每个中断的次数
	parse_proc_stat();

	hupaction.sa_handler = force_rescan;
	sigemptyset(&hupaction.sa_mask);
	hupaction.sa_flags = 0;
	sigaction(SIGHUP, &hupaction, NULL);

	while (keep_going) { //  循环执行，时间周期为 SLEEP_INTERVAL
		sleep_approx(SLEEP_INTERVAL);
		log(TO_CONSOLE, LOG_INFO, "\n\n\n-----------------------------------------------------------------------------\n");
		clear_work_stats();
		parse_proc_interrupts();
		parse_proc_stat();

		/* cope with cpu hotplug -- detected during /proc/interrupts parsing */
		if (need_rescan) {
			need_rescan = 0;
			cycle_count = 0;
			log(TO_CONSOLE, LOG_INFO, "Rescanning cpu topology \n");
			clear_work_stats();

			free_object_tree();
			build_object_tree();
			for_each_irq(NULL, force_rebalance_irq, NULL);
			parse_proc_interrupts();
			parse_proc_stat();
			sleep_approx(SLEEP_INTERVAL);
			clear_work_stats();
			parse_proc_interrupts();
			parse_proc_stat();
		}

		if (cycle_count)
			update_migration_status();

		calculate_placement();
		activate_mappings();

		if (debug_mode)
			dump_tree();
		if (one_shot_mode)
			keep_going = 0;
		cycle_count++;

	}
	free_object_tree();

	/* Remove pidfile */
	if (!foreground_mode && pidfile)
		unlink(pidfile);

	return EXIT_SUCCESS;
}
