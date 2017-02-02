#include <linux/io.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/pm_wakeup.h>
#include <linux/kernel_stat.h>
/* #include <mach/devices_dtb.h> Not implemented yet */
#include <soc/qcom/htc_util.h>
#include <linux/irq.h>
#include <soc/qcom/pm.h>
#include <soc/qcom/rpm_stats.h>
#include "htc_cpu_usage_stats.h"
#include <linux/htc_flags.h>

#ifdef CONFIG_VM_EVENT_COUNTERS
#include <linux/mm.h>
#include <linux/vmstat.h>
unsigned long prev_vm_event[NR_VM_EVENT_ITEMS];
#ifdef CONFIG_ZONE_DMA
#define TEXT_FOR_DMA(xx) xx "_dma",
#else
#define TEXT_FOR_DMA(xx)
#endif

#ifdef CONFIG_ZONE_DMA32
#define TEXT_FOR_DMA32(xx) xx "_dma32",
#else
#define TEXT_FOR_DMA32(xx)
#endif

#ifdef CONFIG_HIGHMEM
#define TEXT_FOR_HIGHMEM(xx) xx "_high",
#else
#define TEXT_FOR_HIGHMEM(xx)
#endif
#define TEXTS_FOR_ZONES(xx) TEXT_FOR_DMA(xx) TEXT_FOR_DMA32(xx) xx "_normal", \
					TEXT_FOR_HIGHMEM(xx) xx "_movable",
const char * const vm_event_text[] = {
	"pgpgin",
	"pgpgout",
	"pswpin",
	"pswpout",

	TEXTS_FOR_ZONES("pgalloc")

	"pgfree",
	"pgactivate",
	"pgdeactivate",

	"pgfault",
	"pgmajfault",

	TEXTS_FOR_ZONES("pgrefill")
	TEXTS_FOR_ZONES("pgsteal_kswapd")
	TEXTS_FOR_ZONES("pgsteal_direct")
	TEXTS_FOR_ZONES("pgscan_kswapd")
	TEXTS_FOR_ZONES("pgscan_direct")
	"pgscan_direct_throttle",

#ifdef CONFIG_NUMA
	"zone_reclaim_failed",
#endif
	"pginodesteal",
	"slabs_scanned",
	"kswapd_inodesteal",
	"kswapd_low_wmark_hit_quickly",
	"kswapd_high_wmark_hit_quickly",
	"pageoutrun",
	"allocstall",

	"pgrotated",

#ifdef CONFIG_NUMA_BALANCING
	"numa_pte_updates",
	"numa_huge_pte_updates",
	"numa_hint_faults",
	"numa_hint_faults_local",
	"numa_pages_migrated",
#endif
#ifdef CONFIG_MIGRATION
	"pgmigrate_success",
	"pgmigrate_fail",
#endif
#ifdef CONFIG_COMPACTION
	"compact_migrate_scanned",
	"compact_free_scanned",
	"compact_isolated",
	"compact_stall",
	"compact_fail",
	"compact_success",
#endif

#ifdef CONFIG_HUGETLB_PAGE
	"htlb_buddy_alloc_success",
	"htlb_buddy_alloc_fail",
#endif
	"unevictable_pgs_culled",
	"unevictable_pgs_scanned",
	"unevictable_pgs_rescued",
	"unevictable_pgs_mlocked",
	"unevictable_pgs_munlocked",
	"unevictable_pgs_cleared",
	"unevictable_pgs_stranded",

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	"thp_fault_alloc",
	"thp_fault_fallback",
	"thp_collapse_alloc",
	"thp_collapse_alloc_failed",
	"thp_split",
	"thp_zero_page_alloc",
	"thp_zero_page_alloc_failed",
#endif
};
#endif

#define USE_STATISTICS_STRATEGY_CONTINUOUS_3    0
#define SEND_KOBJECT_UEVENT_ENV_ENABLED         0

#define NUM_BUSY_THREAD_CHECK                   5
#define HTC_KERNEL_TOP_CPU_USAGE_THRESHOLD      30
#define BUFFER_WARN_LEN                         64
#define BUFFER_TEMP_LEN                         32

#define FORCE_CHARGE		(1<<2)
#define Y_CABLE			(1<<26)

#define KTOP_ACCU_FD "/sys/power/ktop_accu"

struct process_monitor_statistic {
       unsigned int pid;
       char *ppid_name;
       unsigned int cnt;
       unsigned char set_warn;
       unsigned char is_found;
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
       unsigned char sent_uevent;
#endif /*SEND_KOBJECT_UEVENT_ENV_ENABLED */
};

static int pm_monitor_enabled = 0;
static bool usb_speaker_flag = false;

static struct workqueue_struct *htc_pm_monitor_wq = NULL;
static struct workqueue_struct *htc_kernel_top_monitor_wq = NULL;

/* Previous process state */
#define MAX_PID 32768
#define NUM_BUSY_THREAD_CHECK 5

struct _htc_kernel_top {
	struct delayed_work dwork;
	unsigned int *prev_proc_stat;
	int *curr_proc_delta;
	int *curr_proc_pid;
	struct task_struct **task_ptr_array;
	struct kernel_cpustat curr_cpustat;
	struct kernel_cpustat prev_cpustat;
	unsigned long cpustat_time;
	int top_loading_pid[NUM_BUSY_THREAD_CHECK];
	spinlock_t lock;
};

struct st_htc_idle_statistic {
	u32 count;
	u32 time;
};

struct st_htc_idle_statistic htc_idle_stat[CONFIG_NR_CPUS][3];

static int msm_htc_util_delay_time = 10000;
module_param_named(kmonitor_delay, msm_htc_util_delay_time, int, S_IRUGO | S_IWUSR | S_IWGRP);
static int msm_htc_util_top_delay_time = 60000;
module_param_named(ktop_delay, msm_htc_util_top_delay_time, int, S_IRUGO | S_IWUSR | S_IWGRP);

enum {
	KERNEL_TOP,
	KERNEL_TOP_ACCU, /* Kernel Top Accumulation */
};

#if USE_STATISTICS_STRATEGY_CONTINUOUS_3
#define MAX_CONSECUTIVE_THRES_TIMES             3
#define SIZE_OF_CURR_PID_FOUND_ARRAY                    ARRAY_SIZE(current_pid_found_array)
#define SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY      ARRAY_SIZE(process_monitor_continuous_3_array)
struct current_pid_found {
       unsigned char pid_found;
       unsigned char need_to_add;
};

static struct current_pid_found current_pid_found_array[NUM_BUSY_THREAD_CHECK];
static struct process_monitor_statistic process_monitor_continuous_3_array[NUM_BUSY_THREAD_CHECK];

#else /* <Not> USE_STATISTICS_STRATEGY_CONTINUOUS_3 */
#define MAX_OVER_THRES_TIMES                    5
#define HTC_KERNEL_TOP_MONITOR_PERIOD           10
#define MAX_PROCESS_MONITOR_ARRAY_FIELDS        (HTC_KERNEL_TOP_MONITOR_PERIOD * NUM_BUSY_THREAD_CHECK)
#define PROCESS_MONITOR_ARRAY_5_IN_10_SIZE      MAX_PROCESS_MONITOR_ARRAY_FIELDS
#define BUFFER_WARN_5_IN_10_SIZE                HTC_KERNEL_TOP_MONITOR_PERIOD
#define SIZE_OF_PROCESS_MONITOR_5_IN_10_ARRAY           ARRAY_SIZE(process_monitor_5_in_10_array)

static int statistic_monitor_period = 1;
static struct process_monitor_statistic process_monitor_5_in_10_array[PROCESS_MONITOR_ARRAY_5_IN_10_SIZE];
#endif /* USE_STATISTICS_STRATEGY_CONTINUOUS_3 */

#define MSM_HTC_UTIL_DELAY_TIME_COMMERCIAL 60000
bool is_commercial()
{
	return get_radio_flag() == 0;
}

static void clear_process_monitor_array(struct process_monitor_statistic *pArray, int array_size)
{
	int j;

	for (j = 0; j < array_size; j++) {
		(pArray + j)->pid = 0;
		(pArray + j)->ppid_name = NULL;
		(pArray + j)->cnt = 0;
		(pArray + j)->set_warn = 0;
		(pArray + j)->is_found = 0;
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
		(pArray + j)->sent_uevent = 0;
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
	}
} /* clear_process_monitor_array() */

static void write_ktop_fd(struct _htc_kernel_top *ktop)
{
	struct file *fp = NULL;
	char buf[BUFFER_TEMP_LEN * NUM_BUSY_THREAD_CHECK];
	char temp[BUFFER_TEMP_LEN];
	uint8_t idx = 0;
	int tmp;
	uint8_t size;
	int pid, usage;
	char *name;
	unsigned long duration;

	fp = filp_open(KTOP_ACCU_FD, O_WRONLY, 0);
	if (IS_ERR(fp)) {
		pr_err("Unable to open KTOP_ACCU_FD\n");
		return;
	}

	duration = ktop->cpustat_time;
	if (duration) {
		buf[0] = '\0';
		size = sizeof(buf);

		while(idx < NUM_BUSY_THREAD_CHECK) {
			pid = ktop->top_loading_pid[idx];
			name = ktop->task_ptr_array[pid]->comm;
			usage = ktop->curr_proc_delta[pid] * 100 / duration;
			pr_debug("pid: %d, name: %s, usage: %d\n", pid, name, usage);
			snprintf(temp, BUFFER_TEMP_LEN, "%d\t%d\t%s\n", usage, pid, name);
			strncat(buf, temp, BUFFER_TEMP_LEN);
			pr_debug("buf: %s, temp: %s\n", buf, temp);
			idx++;
		}

		tmp = kernel_write(fp, buf, size, 0);
		if (tmp < 0)
			pr_err("Fail to write KTOP_ACCU_FD\n");
	}

	if (fp)
		filp_close(fp, NULL);
	return;
}

#if USE_STATISTICS_STRATEGY_CONTINUOUS_3
static void clear_current_pid_found_array(void)
{
	int i;

	for (i = 0; i < SIZE_OF_CURR_PID_FOUND_ARRAY; i++) {
		current_pid_found_array[i].pid_found = 0;
		current_pid_found_array[i].need_to_add = 0;
	}
}

static int htc_kernel_top_statistics_continuous_3(struct _htc_kernel_top *ktop)
{
	int rtn = 0;
	int i, j, cpu_usage = 0;
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
	int ok_to_send_uevent = 0;
	char buf_warn[SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY * BUFFER_WARN_LEN];
	char buf_temp[BUFFER_TEMP_LEN];
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
	unsigned long delta_time = ktop->cpustat_time;
	int *ptr_top_loading = ktop->top_loading_pid;

	for (i = 0 ; i < SIZE_OF_CURR_PID_FOUND_ARRAY ; i++) {
		if (delta_time > 0)
			cpu_usage = ktop->curr_proc_delta[*(ptr_top_loading + i)] * 100 / delta_time;
		/* Reach the threshold */
		if (cpu_usage >= HTC_KERNEL_TOP_CPU_USAGE_THRESHOLD) {
			/* Search in the array to check if we got any PID match. */
			for (j = 0; j < SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY; j++) {
				/* Mate with the PID records. */
				if (process_monitor_continuous_3_array[j].pid == *(ptr_top_loading + i)) {
					/* Found the PID record. */
					process_monitor_continuous_3_array[j].cnt++;
					process_monitor_continuous_3_array[j].is_found = 1;
					/* Mark the PID was found. */
					current_pid_found_array[i].pid_found = 1;
					if ((process_monitor_continuous_3_array[j].cnt >= MAX_CONSECUTIVE_THRES_TIMES) &&
							(!process_monitor_continuous_3_array[j].set_warn)) {
						process_monitor_continuous_3_array[j].set_warn = 1;
						pr_info("[K] CPU_Sniffer: PID=[%d], name=[%s], over-cpu-usage-threshold.\n",
						process_monitor_continuous_3_array[j].pid, process_monitor_continuous_3_array[j].ppid_name);
					}
					break;
				}
			}
			if (!current_pid_found_array[i].pid_found) {
				current_pid_found_array[i].need_to_add = 1;
			}
		}
	}

#if SEND_KOBJECT_UEVENT_ENV_ENABLED
	/* Pack buffer for sending out kobject_uevent. */
	memset(buf_warn, 0x0, sizeof(buf_warn));
	strcpy(buf_warn, "");
	for (j = 0; j < SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY; j++) {
		if ((process_monitor_continuous_3_array[j].set_warn == 1) &&
			(process_monitor_continuous_3_array[j].sent_uevent == 0)) {
			strcat(buf_warn, "PID=");
			sprintf(buf_temp, "%d", process_monitor_continuous_3_array[j].pid);
			strcat(buf_warn, buf_temp);
			strcat(buf_warn, ",0,0,0;");
			process_monitor_continuous_3_array[j].sent_uevent = 1;
			ok_to_send_uevent++;
		}
	}

	/* Need to send notification by kobject_uevent_env(). */
	if (ok_to_send_uevent) {
		/* End string. */
		strcat(buf_warn, "PID=0,0,0,0;");
		strcat(buf_warn, "#");
		send_cpu_usage_stats_kobject_uevent(&buf_warn[0]);
	}
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */

	/* Kick out the non-consecutive PID record. */
	for (j = 0; j < SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY; j++) {
		if (!process_monitor_continuous_3_array[j].is_found) {
			/* Clear the record. */
			process_monitor_continuous_3_array[j].pid = 0;
			process_monitor_continuous_3_array[j].ppid_name = NULL;
			process_monitor_continuous_3_array[j].cnt = 0;
			process_monitor_continuous_3_array[j].set_warn = 0;
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
			process_monitor_continuous_3_array[j].sent_uevent = 0;
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
		}
		/* Clear the found flag of this round. */
		process_monitor_continuous_3_array[j].is_found = 0;
	}

	/* Add new record. */
	for (i = 0 ; i < SIZE_OF_CURR_PID_FOUND_ARRAY ; i++) {
		/* Store the newer to add into process monitor array. */
		for (j = 0; j < SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY; j++) {
			if (current_pid_found_array[i].need_to_add && !process_monitor_continuous_3_array[j].pid) {
				process_monitor_continuous_3_array[j].pid = *(ptr_top_loading + i);
				process_monitor_continuous_3_array[j].ppid_name = ktop->task_ptr_array[*(ptr_top_loading + i)]->comm;
				process_monitor_continuous_3_array[j].cnt++;
				current_pid_found_array[i].need_to_add = 0;
				break;
			}
		}
	}
	clear_current_pid_found_array();

	return rtn;
}
#else /* <Not> USE_STATISTICS_STRATEGY_CONTINUOUS_3 */
static int htc_kernel_top_statistics_5_in_10(struct _htc_kernel_top *ktop)
{
	int rtn = 0;
	int i, j, cpu_usage = 0;
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
	int ok_to_send_uevent = 0;
	char buf_warn[BUFFER_WARN_5_IN_10_SIZE * BUFFER_WARN_LEN];
	char buf_temp[BUFFER_TEMP_LEN];
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
	unsigned long delta_time = ktop->cpustat_time;
	int *ptr_top_loading = ktop->top_loading_pid;

	for (i = 0 ; i < NUM_BUSY_THREAD_CHECK ; i++) {
		if (delta_time > 0)
			cpu_usage = ktop->curr_proc_delta[*(ptr_top_loading + i)] * 100 / delta_time;
		/* Reach the threshold */
		if (cpu_usage >= HTC_KERNEL_TOP_CPU_USAGE_THRESHOLD) {
			/* Search in the array to check if we got any PID match. */
			for (j = 0; j < SIZE_OF_PROCESS_MONITOR_5_IN_10_ARRAY; j++) {
				/* Mate with the PID records. */
				if (process_monitor_5_in_10_array[j].pid == *(ptr_top_loading + i)) {
					/* Found the PID record. */
					process_monitor_5_in_10_array[j].cnt++;
					if ((process_monitor_5_in_10_array[j].cnt >= MAX_OVER_THRES_TIMES) &&
							(process_monitor_5_in_10_array[j].set_warn == 0)) {
						process_monitor_5_in_10_array[j].set_warn = 1;
						process_monitor_5_in_10_array[j].ppid_name =
								ktop->task_ptr_array[*(ptr_top_loading + i)]->comm;
					}
					break;
				}
				/* Add as the new PID record. */
				else if (process_monitor_5_in_10_array[j].pid == 0) {
		                    process_monitor_5_in_10_array[j].pid = *(ptr_top_loading + i);
				    process_monitor_5_in_10_array[j].cnt++;
		                    break;
				}
			}
		}
	}

	if (statistic_monitor_period < HTC_KERNEL_TOP_MONITOR_PERIOD) {
	        /* 1 ~ 9 */
		statistic_monitor_period++;
	} else {
	        /* 10 -> 1 */
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
		/* Pack buffer for sending out kobject_uevent. */
	        memset(buf_warn, 0x0, sizeof(buf_warn));
		strcpy(buf_warn, "");
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
	        for (j = 0; j < SIZE_OF_PROCESS_MONITOR_5_IN_10_ARRAY; j++) {
			if (process_monitor_5_in_10_array[j].set_warn == 1) {
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
				strcat(buf_warn, "PID=");
			        sprintf(buf_temp, "%d", process_monitor_5_in_10_array[j].pid);
				strcat(buf_warn, buf_temp);
			        strcat(buf_warn, ",0,0,0;");
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
				pr_info("[K] CPU_Sniffer: PID=[%d], name=[%s], over-cpu-usage-threshold.\n",
				process_monitor_5_in_10_array[j].pid, process_monitor_5_in_10_array[j].ppid_name);
#if SEND_KOBJECT_UEVENT_ENV_ENABLED
				process_monitor_5_in_10_array[j].sent_uevent = 1;
				ok_to_send_uevent++;
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */
				rtn = 1;
			}
		}

#if SEND_KOBJECT_UEVENT_ENV_ENABLED
	        /* Need to send notification by kobject_uevent_env(). */
		if (ok_to_send_uevent) {
	            /* End string. */
		    strcat(buf_warn, "PID=0,0,0,0;");
	            strcat(buf_warn, "#");
		    send_cpu_usage_stats_kobject_uevent(&buf_warn[0]);
	        }
#endif /* SEND_KOBJECT_UEVENT_ENV_ENABLED */

		if (pm_monitor_enabled)
	            pr_debug("[K] [KTOP] Reach the number of statistics monitor period.\n");
	        statistic_monitor_period = 1;
		clear_process_monitor_array(&process_monitor_5_in_10_array[0], SIZE_OF_PROCESS_MONITOR_5_IN_10_ARRAY);
	}

	return rtn;
}
#endif /* USE_STATISTICS_STRATEGY_CONTINUOUS_3 */

void htc_idle_stat_add(int sleep_mode, u32 time)
{
	int cpu = smp_processor_id();

	if (cpu < CONFIG_NR_CPUS) {
		switch (sleep_mode) {
		case MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT:
			htc_idle_stat[cpu][0].count++;
			htc_idle_stat[cpu][0].time += time;
			break;
		case MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE:
			htc_idle_stat[cpu][1].count++;
			htc_idle_stat[cpu][1].time += time;
			break;
		case MSM_PM_SLEEP_MODE_POWER_COLLAPSE:
			htc_idle_stat[cpu][2].count++;
			htc_idle_stat[cpu][2].time += time;
			break;
		default:
			break;
		}
	}
}

static void htc_idle_stat_clear(void)
{
	memset(htc_idle_stat, 0, sizeof(htc_idle_stat));
}

static void htc_xo_vddmin_stat_show(void)
{
	uint32_t xo_count = 0, vddmin_count = 0;
	uint64_t xo_time = 0, vddmin_time = 0;
	static uint32_t prev_xo_count = 0, prev_vddmin_count = 0;
	static uint64_t prev_xo_time = 0, prev_vddmin_time = 0;

	if (htc_get_xo_vddmin_info(&xo_count, &xo_time, &vddmin_count, &vddmin_time)) {
		if (xo_count > prev_xo_count) {
			k_pr_embedded("[K] XO: %u, %llums\n", xo_count - prev_xo_count,
							xo_time - prev_xo_time);
			prev_xo_count = xo_count;
			prev_xo_time = xo_time;
		}

		if (vddmin_count > prev_vddmin_count) {
			k_pr_embedded("[K] Vdd-min: %u, %llums\n", vddmin_count - prev_vddmin_count,
							vddmin_time - prev_vddmin_time);
			prev_vddmin_count = vddmin_count;
			prev_vddmin_time = vddmin_time;
		}
	}
}

static void htc_idle_stat_show(void)
{
	int i = 0, cpu = 0;
	char output[256];
	char piece[32];

	for (i = 0; i < 3; i++) {
		memset(output, 0, sizeof(output));
		for (cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
			if (htc_idle_stat[cpu][i].count > 0) {
				if (0 == strlen(output)) {
					memset(piece, 0, sizeof(piece));
					snprintf(piece, sizeof(piece), "C%d: ", i);
					safe_strcat(output, piece);
				} else {
					safe_strcat(output, ",");
				}
				memset(piece, 0, sizeof(piece));
				snprintf(piece, sizeof(piece), "(%d,%d,%dms)",
					cpu,
					htc_idle_stat[cpu][i].count,
					htc_idle_stat[cpu][i].time / 1000);
				safe_strcat(output, piece);
			}
		}
		if (strlen(output) > 0) {
			k_pr_embedded("[K] %s\n", output);
		}
	}
}

static void htc_debug_flag_show(void)
{

	unsigned int cfg = 0 ;
    /* To check debug flag and set cfg to 1 if kernel flag is 6 4 or 6 4000000 */
	if(get_tamper_sf() == 0){
		if((get_kernel_flag() & FORCE_CHARGE) || (get_kernel_flag() & Y_CABLE))
			cfg = 1 ;
		pr_info("[K] CFG:0x%x\n", cfg);
	 }
}

#ifdef arch_idle_time
static cputime64_t get_idle_time(int cpu)
{
        cputime64_t idle;

        idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
        if (cpu_online(cpu) && !nr_iowait_cpu(cpu))
                idle += arch_idle_time(cpu);
        return idle;
}

static cputime64_t get_iowait_time(int cpu)
{
        cputime64_t iowait;

        iowait = kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
        if (cpu_online(cpu) && nr_iowait_cpu(cpu))
                iowait += arch_idle_time(cpu);
        return iowait;
}

#else

static u64 get_idle_time(int cpu)
{
        u64 idle, idle_time = get_cpu_idle_time_us(cpu, NULL);

        if (idle_time == -1ULL)
                /* !NO_HZ so we can rely on cpustat.idle */
                idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
        else
                idle = usecs_to_cputime64(idle_time);

        return idle;
}

static u64 get_iowait_time(int cpu)
{
        u64 iowait, iowait_time = get_cpu_iowait_time_us(cpu, NULL);

        if (iowait_time == -1ULL)
                /* !NO_HZ so we can rely on cpustat.iowait */
                iowait = kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
        else
                iowait = usecs_to_cputime64(iowait_time);

        return iowait;
}

#endif

static void get_all_cpustat(struct kernel_cpustat *cpu_stat)
{
	int cpu;

	if (!cpu_stat)
		return;

	memset(cpu_stat, 0, sizeof(struct kernel_cpustat));

	for_each_possible_cpu(cpu) {
		cpu_stat->cpustat[CPUTIME_USER] += kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
		cpu_stat->cpustat[CPUTIME_NICE] += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];
		cpu_stat->cpustat[CPUTIME_SYSTEM] += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
		cpu_stat->cpustat[CPUTIME_IDLE] += get_idle_time(cpu);
		cpu_stat->cpustat[CPUTIME_IOWAIT] += get_iowait_time(cpu);
		cpu_stat->cpustat[CPUTIME_IRQ] += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
		cpu_stat->cpustat[CPUTIME_SOFTIRQ] += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
		cpu_stat->cpustat[CPUTIME_STEAL] += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
		cpu_stat->cpustat[CPUTIME_GUEST] += kcpustat_cpu(cpu).cpustat[CPUTIME_GUEST];
		cpu_stat->cpustat[CPUTIME_GUEST_NICE] += kcpustat_cpu(cpu).cpustat[CPUTIME_GUEST_NICE];
	}
}

static void sort_cputime_by_pid(int *source, int *pid_pos, int pid_cnt, int *result)
{
    int i = 0, j = 0, k = 0, l = 0;
    int pid_found = 0;

    for (i = 0; i < NUM_BUSY_THREAD_CHECK; i++) {
        result[i] = 0;
        if (i == 0) {
            for (j = 0; j < pid_cnt; j++) {
                k = pid_pos[j];
                /* Find the largest one. */
                if(source[result[i]] < source[k]) {
                    result[i] = k;
                }
            }
        } else {
            for (j = 0; j < pid_cnt; j++) {
                k = pid_pos[j];
                /* Skip the saved PIDs. */
                for (l = 0; l < i; l++) {
                    /* Field (index k) is saved already. */
                    if (result[l] == k) {
                        pid_found = 1;
                        break;
                    }
                }
                /* Found the saved PID and skip it (index k). */
                if (pid_found) {
                    pid_found = 0;
                    continue;
                }

                /* Find the largest one from rest fields. */
                if(source[result[i]] < source[k]) {
                    result[i] = k;
                }
            }
        }
    }
}

static unsigned long htc_calculate_cpustat_time(struct kernel_cpustat curr_cpustat,
						struct kernel_cpustat prev_cpustat)
{
	unsigned long user_time = 0, system_time = 0, io_time = 0;
	unsigned long irq_time = 0, idle_time = 0;

	user_time = (unsigned long) ((curr_cpustat.cpustat[CPUTIME_USER] +
					curr_cpustat.cpustat[CPUTIME_NICE]) -
					(prev_cpustat.cpustat[CPUTIME_USER] +
					prev_cpustat.cpustat[CPUTIME_NICE]));
	system_time = (unsigned long) (curr_cpustat.cpustat[CPUTIME_SYSTEM] -
					prev_cpustat.cpustat[CPUTIME_SYSTEM]);
	io_time = (unsigned long) (curr_cpustat.cpustat[CPUTIME_IOWAIT] -
					prev_cpustat.cpustat[CPUTIME_IOWAIT]);
	irq_time = (unsigned long) ((curr_cpustat.cpustat[CPUTIME_IRQ] +
					curr_cpustat.cpustat[CPUTIME_SOFTIRQ]) -
					(prev_cpustat.cpustat[CPUTIME_IRQ] +
					 prev_cpustat.cpustat[CPUTIME_SOFTIRQ]));
	idle_time = (unsigned long) ((curr_cpustat.cpustat[CPUTIME_IDLE] > prev_cpustat.cpustat[CPUTIME_IDLE]) ?
					curr_cpustat.cpustat[CPUTIME_IDLE] - prev_cpustat.cpustat[CPUTIME_IDLE] : 0);
	idle_time += (unsigned long) ((curr_cpustat.cpustat[CPUTIME_STEAL] +
					curr_cpustat.cpustat[CPUTIME_GUEST]) -
					(prev_cpustat.cpustat[CPUTIME_STEAL] +
					prev_cpustat.cpustat[CPUTIME_GUEST]));

	return (user_time + system_time + io_time + irq_time + idle_time);
}

static void htc_kernel_top_cal(struct _htc_kernel_top *ktop, int type)
{
	int pid_cnt = 0;
	struct task_struct *process;
	struct task_cputime cputime;

	if (ktop->task_ptr_array == NULL ||
	    ktop->curr_proc_delta == NULL ||
	    ktop->curr_proc_pid == NULL ||
	    ktop->prev_proc_stat == NULL)
	  return;

	rcu_read_lock();
	/* Calculate cpu time of each process */
	for_each_process(process) {
		thread_group_cputime(process, &cputime);
		if (process->pid < MAX_PID) {
			ktop->curr_proc_delta[process->pid] =
				(cputime.utime + cputime.stime) - ktop->prev_proc_stat[process->pid];
			ktop->task_ptr_array[process->pid] = process;

			if (ktop->curr_proc_delta[process->pid] > 0) {
				ktop->curr_proc_pid[pid_cnt] = process->pid;
				pid_cnt++;
			}
		}
	}
	sort_cputime_by_pid(ktop->curr_proc_delta, ktop->curr_proc_pid, pid_cnt, ktop->top_loading_pid);

	/* Calculate cpu time of cpus */
	get_all_cpustat(&ktop->curr_cpustat);
	ktop->cpustat_time = htc_calculate_cpustat_time(ktop->curr_cpustat, ktop->prev_cpustat);

	if (type == KERNEL_TOP_ACCU) {
#if USE_STATISTICS_STRATEGY_CONTINUOUS_3
		htc_kernel_top_statistics_continuous_3(ktop);
#else /* <Not> USE_STATISTICS_STRATEGY_CONTINUOUS_3 */
		htc_kernel_top_statistics_5_in_10(ktop);
#endif
	}

	/* Save old process cpu time info */
	for_each_process(process) {
		if (process->pid < MAX_PID) {
			thread_group_cputime(process, &cputime);
			ktop->prev_proc_stat[process->pid] = cputime.stime + cputime.utime;
		}
	}
	memcpy(&ktop->prev_cpustat, &ktop->curr_cpustat, sizeof(struct kernel_cpustat));
	rcu_read_unlock();
}

static void htc_kernel_top_show(struct _htc_kernel_top *ktop, int type)
{
	int top_n_pid = 0, i;
	char piece[32];
	char output[256];

	/* Print most time consuming processes */
	memset(output, 0, sizeof(output));
	k_pr_info("[K]%sCPU Usage\t\tPID\t\tName\n", type == KERNEL_TOP_ACCU ? "[KTOP]" : " ");
	for (i = 0; i < NUM_BUSY_THREAD_CHECK; i++) {
		memset(piece, 0, sizeof(piece));
		if (ktop->cpustat_time > 0) {
			top_n_pid = ktop->top_loading_pid[i];
			k_pr_info("[K]%s%8lu%%\t\t%d\t\t%s\t\t%d\n", type == KERNEL_TOP_ACCU ? "[KTOP]" : " ",
				ktop->curr_proc_delta[top_n_pid] * 100 / ktop->cpustat_time,
				top_n_pid,
				ktop->task_ptr_array[top_n_pid]->comm,
				ktop->curr_proc_delta[top_n_pid]);
			if (type == KERNEL_TOP_ACCU) {
				snprintf(piece, sizeof(piece), "(%lu%%,%d,%s)",
					ktop->curr_proc_delta[top_n_pid] * 100 / ktop->cpustat_time,
					top_n_pid,
					ktop->task_ptr_array[top_n_pid]->comm);
				if (0 == strlen(output)) {
					safe_strcat(output, piece);
				} else {
					safe_strcat(output, ",");
					safe_strcat(output, piece);
				}
			}
		}

	}
	if (type == KERNEL_TOP_ACCU) {
		k_pr_embedded("[K][KTOP] %s\n", output);
	}
	memset(ktop->curr_proc_delta, 0, sizeof(int) * MAX_PID);
	memset(ktop->task_ptr_array, 0, sizeof(int) * MAX_PID);
	memset(ktop->curr_proc_pid, 0, sizeof(int) * MAX_PID);
}

extern int sensor_get_id(char *name);
extern int sensor_get_temp(uint32_t sensor_id, long *temp);

static void htc_show_sensor_temp(void)
{
	int ret, id, i;
	long temp;
	char *sensors[] = {"emmc_therm", "quiet_therm", "hub_therm"};
	char piece[32];
	char output[256];

	memset(output, 0, sizeof(output));
	for (i = 0; i < ARRAY_SIZE(sensors); i++) {
		id = sensor_get_id(sensors[i]);
		if (id >= 0) {
			temp = 0;
			ret = sensor_get_temp(id, &temp);
			memset(piece, 0, sizeof(piece));
			snprintf(piece, sizeof(piece), "%s(%d,%s,%ld,%d)",
				strlen(output)>0 ? "," : "",
				id,
				sensors[i],
				temp,
				ret);
			safe_strcat(output, piece);
		}
	}
	k_pr_embedded("[K] sensor_temp: %s\n", output);
}

/* Workaround for audio fizz noise*/
void htc_disable_partial_pm_log_for_audio(bool flag)
{
	usb_speaker_flag = flag;
}

static void htc_pm_monitor_work_func(struct work_struct *work)
{
	struct _htc_kernel_top *ktop = container_of(work, struct _htc_kernel_top,
		                        dwork.work);
	struct timespec ts;
	struct rtc_time tm;

	unsigned long vm_event[NR_VM_EVENT_ITEMS];
	int i;

	if (!htc_pm_monitor_wq) {
		pr_info("[K] htc_pm_monitor_wq is unavaliable.\n");
		return;
	}

	getnstimeofday(&ts);
	rtc_time_to_tm(ts.tv_sec - (sys_tz.tz_minuteswest * 60), &tm);
	k_pr_info("[K][PM] hTC PM Statistic start (%02d-%02d %02d:%02d:%02d)\n",
		tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	/* Show interesting sensor temperature */
	htc_show_sensor_temp();

	/* Show interrupt status */
	htc_show_interrupts();

	/* Show idle stats */
	htc_idle_stat_show();
	htc_idle_stat_clear();
	htc_xo_vddmin_stat_show();
	msm_rpm_dump_stat(true);

	/* Show timer stats */
	if(!is_commercial())
	{
		htc_timer_stats_onoff('0');
		htc_timer_stats_show(300); /*Show timer events which greater than 300 every 10 sec*/
		htc_timer_stats_onoff('1');
	}

	/* Show wakeup source */
	htc_print_active_wakeup_sources(true);
	queue_delayed_work(htc_pm_monitor_wq, &ktop->dwork, msecs_to_jiffies(msm_htc_util_delay_time));

	if (!is_commercial()) {
		htc_kernel_top_cal(ktop, KERNEL_TOP);
		htc_kernel_top_show(ktop, KERNEL_TOP);

		/* Show Debug flag */
		htc_debug_flag_show();

		all_vm_events(vm_event);
		vm_event[PGPGIN] /= 2;
		/* sectors -> kbytes */
		vm_event[PGPGOUT] /= 2;

		for(i = 0; i < NR_VM_EVENT_ITEMS; i++) {
			if (vm_event[i] - prev_vm_event[i] > 0)
				pr_info("[K] %s = %lu\n", vm_event_text[i], vm_event[i] - prev_vm_event[i]);
		}
		memcpy(prev_vm_event, vm_event, sizeof(unsigned long) * NR_VM_EVENT_ITEMS);
	}

	k_pr_info("[K][PM] hTC PM Statistic done\n");
}

static void htc_kernel_top_accumulation_monitor_work_func(struct work_struct *work)
{
        struct _htc_kernel_top *ktop = container_of(work, struct _htc_kernel_top,
		                        dwork.work);
	struct timespec ts;
	struct rtc_time tm;

	if (htc_kernel_top_monitor_wq == NULL){
		if (pm_monitor_enabled)
			printk( "[K] hTc Kernel Top statistic is NULL.\n");
		return;
	}

	getnstimeofday(&ts);
	rtc_time_to_tm(ts.tv_sec - (sys_tz.tz_minuteswest * 60), &tm);
	if (pm_monitor_enabled)
		k_pr_info("[K][KTOP] hTC Kernel Top Statistic start (%02d-%02d %02d:%02d:%02d) \n",
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	queue_delayed_work(htc_kernel_top_monitor_wq, &ktop->dwork, msecs_to_jiffies(msm_htc_util_top_delay_time));
	htc_kernel_top_cal(ktop, KERNEL_TOP_ACCU);
	write_ktop_fd(ktop);
	htc_kernel_top_show(ktop, KERNEL_TOP_ACCU);

	if (pm_monitor_enabled)
		k_pr_info("[K][KTOP] hTC Kernel Top Statistic done\n");
}

void htc_monitor_init(void)
{
	struct _htc_kernel_top *htc_kernel_top;
	struct _htc_kernel_top *htc_kernel_top_accu;

	/*
	*  enable: writeconfig 6 2000000
	*/
	if ((get_kernel_flag() & KERNEL_FLAG_PM_MONITOR) ||
		!(get_kernel_flag() & KERNEL_FLAG_TEST_PWR_SUPPLY))
		pm_monitor_enabled = 1;
	else
		pm_monitor_enabled = 0;

	if (pm_monitor_enabled) {
		if (htc_pm_monitor_wq == NULL)
			/* Create private workqueue */
			htc_pm_monitor_wq = create_workqueue("htc_pm_monitor_wq");

		if (!htc_pm_monitor_wq)
			return;

		pr_info("[K] Success to create htc_pm_monitor_wq (0x%p).\n",
						htc_pm_monitor_wq);
		htc_kernel_top = vmalloc(sizeof(*htc_kernel_top));
		spin_lock_init(&htc_kernel_top->lock);

		htc_kernel_top->prev_proc_stat = vmalloc(sizeof(long) * MAX_PID);
		htc_kernel_top->curr_proc_delta = vmalloc(sizeof(long) * MAX_PID);
		htc_kernel_top->task_ptr_array = vmalloc(sizeof(long) * MAX_PID);
		htc_kernel_top->curr_proc_pid = vmalloc(sizeof(long) * MAX_PID);

		memset(htc_kernel_top->prev_proc_stat, 0, sizeof(long) * MAX_PID);
		memset(htc_kernel_top->curr_proc_delta, 0, sizeof(long) * MAX_PID);
		memset(htc_kernel_top->task_ptr_array, 0, sizeof(long) * MAX_PID);
		memset(htc_kernel_top->curr_proc_pid, 0, sizeof(long) * MAX_PID);

		get_all_cpustat(&htc_kernel_top->curr_cpustat);
	        get_all_cpustat(&htc_kernel_top->prev_cpustat);

		if (is_commercial()) {
			msm_htc_util_delay_time = MSM_HTC_UTIL_DELAY_TIME_COMMERCIAL;
		}
		INIT_DELAYED_WORK(&htc_kernel_top->dwork, htc_pm_monitor_work_func);
		queue_delayed_work(htc_pm_monitor_wq, &htc_kernel_top->dwork,
						msecs_to_jiffies(msm_htc_util_delay_time));
	}

	if (htc_kernel_top_monitor_wq == NULL) {
		/* Create private workqueue... */
		htc_kernel_top_monitor_wq = create_workqueue("htc_kernel_top_monitor_wq");
		printk( "[K][KTOP] Create HTC private workqueue(0x%p)...\n",
						htc_kernel_top_monitor_wq);
	}

	if (!htc_kernel_top_monitor_wq)
		return;

	printk( "[K][KTOP] Success to create htc_kernel_top_monitor_wq (0x%p).\n",
						htc_kernel_top_monitor_wq);
#if USE_STATISTICS_STRATEGY_CONTINUOUS_3
	clear_current_pid_found_array();
	clear_process_monitor_array(&process_monitor_continuous_3_array[0],
						SIZE_OF_PROCESS_MONITOR_CONTINUOUS_3_ARRAY);
#else /* <Not> USE_STATISTICS_STRATEGY_CONTINUOUS_3 */
	clear_process_monitor_array(&process_monitor_5_in_10_array[0],
						SIZE_OF_PROCESS_MONITOR_5_IN_10_ARRAY);
#endif /* USE_STATISTICS_STRATEGY_CONTINUOUS_3 */
	htc_kernel_top_accu = vmalloc(sizeof(*htc_kernel_top_accu));
	spin_lock_init(&htc_kernel_top_accu->lock);

	htc_kernel_top_accu->prev_proc_stat = vmalloc(sizeof(long) * MAX_PID);
	htc_kernel_top_accu->curr_proc_delta = vmalloc(sizeof(long) * MAX_PID);
	htc_kernel_top_accu->task_ptr_array = vmalloc(sizeof(long) * MAX_PID);
	htc_kernel_top_accu->curr_proc_pid = vmalloc(sizeof(long) * MAX_PID);
	memset(htc_kernel_top_accu->prev_proc_stat, 0, sizeof(long) * MAX_PID);
	memset(htc_kernel_top_accu->curr_proc_delta, 0, sizeof(long) * MAX_PID);
	memset(htc_kernel_top_accu->task_ptr_array, 0, sizeof(long) * MAX_PID);
	memset(htc_kernel_top_accu->curr_proc_pid, 0, sizeof(long) * MAX_PID);

	get_all_cpustat(&htc_kernel_top_accu->curr_cpustat);
	get_all_cpustat(&htc_kernel_top_accu->prev_cpustat);
	INIT_DELAYED_WORK(&htc_kernel_top_accu->dwork,
				htc_kernel_top_accumulation_monitor_work_func);
	queue_delayed_work(htc_kernel_top_monitor_wq, &htc_kernel_top_accu->dwork,
				msecs_to_jiffies(msm_htc_util_top_delay_time));
}
