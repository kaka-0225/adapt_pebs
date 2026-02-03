/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>
#include <linux/math64.h> // 新增：内核 64 位除法支持

// Welford 算法的定点数缩放因子
#define AP_SCALE_SHIFT 10 // 放大 1024 倍 (2^10)

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;

static bool valid_va(unsigned long addr)
{
	if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
		return true;
	else
		return false;
}

static __u64 get_pebs_event(enum events e)
{
	switch (e) {
	case L1_HIT:
		return ICL_L1_HIT;
	case L1_MISS:
		return ICL_L1_MISS;
	case L2_HIT:
		return ICL_L2_HIT;
	case L2_MISS:
		return ICL_L2_MISS;
	case L3_HIT:
		return ICL_L3_HIT;
	case L3_MISS:
		return ICL_L3_MISS;
	case DRAMREAD:
		return ICL_LOCAL_DRAM;
	case NVMREAD:
		return ICL_LOCAL_PMM;
	case MEMWRITE:
		return ICL_ALL_STORES;
	default:
		return N_HTMMEVENTS;
	}
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu, __u64 type,
			     __u32 pid)
{
	struct perf_event_attr attr;
	struct file *file;
	int event_fd, __pid;

	memset(&attr, 0, sizeof(struct perf_event_attr));

	attr.type = PERF_TYPE_RAW;
	attr.size = sizeof(struct perf_event_attr);
	attr.config = config;
	attr.config1 = config1;
	// 三级采样周期：L1/WRITE 用十万级，L2 用五万，其他用百级
	if (type == L1_HIT || type == L1_MISS || type == MEMWRITE) {
		//attr.sample_period = get_sample_inst_period(0); // 100,003
		attr.sample_period = 500000; // 100,003
	} else if (type == L2_HIT || type == L2_MISS) {
		attr.sample_period = L2_SAMPLE_PERIOD; // 50,000（固定）
	} else {
		//attr.sample_period = get_sample_period(0); // 199
		attr.sample_period = 5000;
	}
	attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR |
			   PERF_SAMPLE_TIME;
	attr.disabled = 0;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_callchain_kernel = 1;
	attr.exclude_callchain_user = 1;
	attr.precise_ip = 1;
	attr.enable_on_exec = 1;

	if (pid == 0)
		__pid = -1;
	else
		__pid = pid;

	event_fd = htmm__perf_event_open(&attr, __pid, cpu, -1, 0);
	//event_fd = htmm__perf_event_open(&attr, -1, cpu, -1, 0);
	if (event_fd <= 0) {
		printk("[error htmm__perf_event_open failure] event_fd: %d, config %llx, config1 %llx\n",
		       event_fd, config, config1);
		return -1;
	}

	file = fget(event_fd);
	if (!file) {
		printk("invalid file\n");
		return -1;
	}
	mem_event[cpu][type] = fget(event_fd)->private_data;
	return 0;
}

static int pebs_init(pid_t pid, int node)
{
	int cpu, event;

	mem_event =
		kzalloc(sizeof(struct perf_event **) * nr_cpu_ids, GFP_KERNEL);
	for_each_online_cpu (cpu) {
		mem_event[cpu] = kzalloc(
			sizeof(struct perf_event *) * N_HTMMEVENTS, GFP_KERNEL);
	}

	printk("pebs_init\n");

	for_each_online_cpu (cpu) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (get_pebs_event(event) == N_HTMMEVENTS) {
				mem_event[cpu][event] = NULL;
				continue;
			}

			if (__perf_event_open(get_pebs_event(event), 0, cpu,
					      event, pid))
				return -1;
			if (htmm__perf_event_init(mem_event[cpu][event],
						  BUFFER_SIZE))
				return -1;
		}
	}

	return 0;
}

static void pebs_disable(void)
{
	int cpu, event;
	printk("pebs disable\n");

	/* Check if mem_event was initialized */
	if (!mem_event)
		return;

	for_each_online_cpu (cpu) {
		/* Check if this CPU's event array was allocated */
		if (!mem_event[cpu])
			continue;

		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (mem_event[cpu][event])
				perf_event_disable(mem_event[cpu][event]);
		}
	}
}

static void pebs_enable(void)
{
	int cpu, event;

	printk("pebs enable\n");
	for_each_online_cpu (cpu) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (mem_event[cpu][event])
				perf_event_enable(mem_event[cpu][event]);
		}
	}
}

static void pebs_update_period(uint64_t value, uint64_t inst_value)
{
	int cpu, event;

	for_each_online_cpu (cpu) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			int ret;
			if (!mem_event[cpu][event])
				continue;

			switch (event) {
			case L1_HIT:
			case L1_MISS:
			case MEMWRITE:
				ret = perf_event_period(mem_event[cpu][event],
							inst_value);
				break;
			case L2_HIT:
			case L2_MISS:
				// L2 固定周期 50000，不动态调整
				ret = 0;
				break;
			case L3_HIT:
			case L3_MISS:
			case DRAMREAD:
			case NVMREAD:
				ret = perf_event_period(mem_event[cpu][event],
							value);
				break;
			default:
				ret = 0;
				break;
			}

			if (ret == -EINVAL)
				printk("failed to update sample period");
		}
	}
}

/**
 * Adaptive-PEBS: 更新页面的 Welford 在线方差（定点数版本）
 * @pinfo: 目标页面的 pginfo 结构指针
 * @now:   当前系统时间戳（来自 PERF_SAMPLE_TIME）
 * 
 * 算法说明：
 *   Welford 在线方差算法用于计算访问间隔的抖动/波动性
 *   mean_n = mean_{n-1} + (x - mean_{n-1}) / n
 *   M2_n = M2_{n-1} + (x - mean_{n-1}) * (x - mean_n)
 *   variance = M2 / n
 * 
 * 定点数处理：
 *   所有间隔值放大 1024 倍（左移 10 位）以保留精度
 * 
 * trace_printk 埋点：
 *   用于离线分析各字段的数值范围，判断位数是否合适
 */
void update_page_fluctuation(pginfo_t *pinfo, u64 now)
{
	u64 interval; // 原始间隔（未缩放，单位：纳秒或 TSC cycles）
	u64 x_scaled; // 缩放后的间隔（1024 倍）
	s64 delta, delta2; // Welford 算法的两个 delta
	u32 n; // 样本计数

	// ========== 第 1 步：冷启动处理 ==========
	if (unlikely(pinfo->last_hit_time == 0)) {
		pinfo->last_hit_time = now;
		pinfo->adaptive_hit = 1;
		pinfo->mean_interval = 0;
		pinfo->fluctuation = 0;

		// 【trace_printk】：记录首次采样
		trace_printk("[Welford-Init] pg=%px, init_time=%llu\n", pinfo,
			     now);
		return;
	}

	// ========== 第 2 步：计算间隔并缩放 ==========
	// ⚠️ FIX: PEBS 时间戳可能乱序！忽略时间倒退的样本
	if (unlikely(now <= pinfo->last_hit_time)) {
		// 时间戳倒退或相等，完全跳过本次样本
		trace_printk(
			"[Welford-SKIP-TIME-REWIND] pg=%px, now=%llu <= last=%llu (delta=%lld)\n",
			pinfo, now, pinfo->last_hit_time,
			(s64)(now - pinfo->last_hit_time));
		// 不更新 last_hit_time，不增加 adaptive_hit，完全忽略这个乱序样本
		return;
	}

	interval = now - pinfo->last_hit_time;
	x_scaled = interval << AP_SCALE_SHIFT; // 放大 1024 倍

	// 【trace_printk】：监控原始间隔值
	// 用途：判断 u64 是否会溢出，观察间隔分布
	trace_printk(
		"[Welford-Interval] pg=%px, raw_interval=%llu, scaled=%llu\n",
		pinfo, interval, x_scaled);

	// ========== 第 3 步：更新时间戳 ==========
	pinfo->last_hit_time = now;

	// ========== 第 4 步：样本数递增 ==========
	n = ++pinfo->adaptive_hit;

	// 【trace_printk】：监控样本数，判断 u32 (42亿) 是否足够
	// 每达到 2^20 (约 100 万) 的倍数时打印一次里程碑
	if (unlikely((n & 0xFFFFF) == 0)) {
		trace_printk(
			"[Welford-Milestone] pg=%px, n=%u (every 1M samples)\n",
			pinfo, n);
	}

	// ========== 第 5 步：Welford 方差计算 ==========

	// 步骤 5.1：delta = x - mean_{n-1}
	delta = (s64)x_scaled - (s64)pinfo->mean_interval;

	// 【trace_printk】：监控 delta1 范围
	// 用途：判断 s64 是否足够，观察是否有异常大的波动
	trace_printk(
		"[Welford-Delta1] pg=%px, delta=%lld, x_scaled=%llu, old_mean=%llu\n",
		pinfo, delta, x_scaled, pinfo->mean_interval);

	// 步骤 5.2：mean_n = mean_{n-1} + delta / n
	// 注意：必须使用 div_s64 进行 64 位有符号除法
	pinfo->mean_interval += (u64)div_s64(delta, n);

	// 步骤 5.3：delta2 = x - mean_n
	delta2 = (s64)x_scaled - (s64)pinfo->mean_interval;

	// 【trace_printk】：监控 delta2 范围
	trace_printk("[Welford-Delta2] pg=%px, delta2=%lld, new_mean=%llu\n",
		     pinfo, delta2, pinfo->mean_interval);

	// 步骤 5.4：M2_n = M2_{n-1} + delta * delta2
	// 注意：delta * delta2 会放大到 1024*1024 = 2^20 倍
	//       右移 AP_SCALE_SHIFT 恢复到 1024 倍精度
	pinfo->fluctuation += (u64)((delta * delta2) >> AP_SCALE_SHIFT);

	// ========== 【核心 trace_printk】：汇总所有字段数值 ==========
	// 这是最重要的日志，用于离线分析各字段的位数是否合适
	trace_printk(
		"[Welford-Summary] pg=%px | n=%u | mean=%llu | M2=%llu | var_approx=%llu | interval=%llu\n",
		pinfo,
		pinfo->adaptive_hit, // 样本数（u32，最大 ~42 亿）
		pinfo->mean_interval, // 均值（u64，1024 倍缩放）
		pinfo->fluctuation, // M2（u64，1024 倍缩放）
		(pinfo->adaptive_hit > 1) ?
			(pinfo->fluctuation / (pinfo->adaptive_hit - 1)) :
			0, // 近似标准差²
		interval); // 原始间隔（未缩放）
}

static int ksamplingd(void *data)
{
	unsigned long long nr_sampled = 0, nr_dram = 0, nr_nvm = 0,
			   nr_write = 0;
	unsigned long long nr_throttled = 0, nr_lost = 0, nr_unknown = 0;
	unsigned long long nr_skip = 0;

	/* used for calculating average cpu usage of ksampled */
	struct task_struct *t = current;
	/* a unit of cputime: permil (1/1000) */
	u64 total_runtime, exec_runtime, cputime = 0;
	unsigned long total_cputime, elapsed_cputime, cur;
	/* used for periodic checks*/
	unsigned long cpucap_period = msecs_to_jiffies(15000); // 15s
	unsigned long sample_period = 0;
	unsigned long sample_inst_period = 0;
	/* report cpu/period stat */
	unsigned long trace_cputime,
		trace_period = msecs_to_jiffies(1500); // 3s
	unsigned long trace_runtime;
	/* for timeout */
	unsigned long sleep_timeout;

	/* for analytic purpose */
	unsigned long hr_dram = 0, hr_nvm = 0;

	/* orig impl: see read_sum_exec_runtime() */
	trace_runtime = total_runtime = exec_runtime = t->se.sum_exec_runtime;

	trace_cputime = total_cputime = elapsed_cputime = jiffies;
	sleep_timeout = usecs_to_jiffies(2000);

	/* TODO implements per-CPU node ksamplingd by using pg_data_t */
	/* Currently uses a single CPU node(0) */
	const struct cpumask *cpumask = cpumask_of_node(0);
	if (!cpumask_empty(cpumask))
		do_set_cpus_allowed(access_sampling, cpumask);

	while (!kthread_should_stop()) {
		int cpu, event, cond = false;

		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}

		for_each_online_cpu (cpu) {
			for (event = 0; event < N_HTMMEVENTS; event++) {
				do {
					struct perf_buffer *rb;
					struct perf_event_mmap_page *up;
					struct perf_event_header *ph;
					struct htmm_event *he;
					unsigned long pg_index, offset;
					int page_shift;
					__u64 head;

					if (!mem_event[cpu][event]) {
						//continue;
						break;
					}

					__sync_synchronize();

					rb = mem_event[cpu][event]->rb;
					if (!rb) {
						printk("event->rb is NULL\n");
						return -1;
					}
					/* perf_buffer is ring buffer */
					up = READ_ONCE(rb->user_page);
					head = READ_ONCE(up->data_head);
					if (head == up->data_tail) {
						if (cpu < 16)
							nr_skip++;
						//continue;
						break;
					}

					head -= up->data_tail;
					if (head >
					    (BUFFER_SIZE *
					     ksampled_max_sample_ratio / 100)) {
						cond = true;
					} else if (head <
						   (BUFFER_SIZE *
						    ksampled_min_sample_ratio /
						    100)) {
						cond = false;
					}

					/* read barrier */
					smp_rmb();

					page_shift =
						PAGE_SHIFT + page_order(rb);
					/* get address of a tail sample */
					offset = READ_ONCE(up->data_tail);
					pg_index = (offset >> page_shift) &
						   (rb->nr_pages - 1);
					offset &= (1 << page_shift) - 1;

					ph = (void *)(rb->data_pages[pg_index] +
						      offset);
					switch (ph->type) {
					case PERF_RECORD_SAMPLE:
						he = (struct htmm_event *)ph;

						// ============================================================
						// 🆕 新增：使用 trace_printk 记录 PEBS 采样
						// Event 编号含义：
						//   0=L1_HIT, 1=L1_MISS, 2=L2_HIT, 3=L2_MISS,
						//   4=L3_HIT, 5=L3_MISS, 6=DRAMREAD, 7=NVMREAD, 8=MEMWRITE
						// ============================================================

						if (!valid_va(he->addr)) {
							break;
						}
						trace_printk(
							"[PEBS] CPU=%d Event=%d PID=%u TID=%u Addr=0x%llx IP=0x%llx Time=%llu\n",
							cpu, event, he->pid,
							he->tid, he->addr,
							he->ip, he->time);
						update_pginfo(he->pid, he->addr,
							      event, he->time);
						//count_vm_event(HTMM_NR_SAMPLED);
						nr_sampled++;

						// 暂时保持 DRAM/NVM 统计，L1/L2/L3 只计入 nr_sampled
						if (event == DRAMREAD) {
							nr_dram++;
							hr_dram++;
						} else if (event == NVMREAD) {
							nr_nvm++;
							hr_nvm++;
						} else if (event == MEMWRITE) {
							nr_write++;
						}
						// L1/L2/L3 事件暂不单独统计
						break;
					case PERF_RECORD_THROTTLE:
					case PERF_RECORD_UNTHROTTLE:
						nr_throttled++;
						break;
					case PERF_RECORD_LOST_SAMPLES:
						nr_lost++;
						break;
					default:
						nr_unknown++;
						break;
					}
					if (nr_sampled % 500000 == 0) {
						trace_printk(
							"nr_sampled: %llu, nr_dram: %llu, nr_nvm: %llu, nr_write: %llu, nr_throttled: %llu \n",
							nr_sampled, nr_dram,
							nr_nvm, nr_write,
							nr_throttled);
						nr_dram = 0;
						nr_nvm = 0;
						nr_write = 0;
					}
					/* read, write barrier */
					smp_mb();
					WRITE_ONCE(up->data_tail,
						   up->data_tail + ph->size);
				} while (cond);
			}
		}
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
		if (!ksampled_soft_cpu_quota)
			continue;

		/* sleep */
		schedule_timeout_interruptible(sleep_timeout);

		/* check elasped time */
		cur = jiffies;
		if ((cur - elapsed_cputime) >= cpucap_period) {
			u64 cur_runtime = t->se.sum_exec_runtime;
			exec_runtime = cur_runtime - exec_runtime; //ns
			elapsed_cputime =
				jiffies_to_usecs(cur - elapsed_cputime); //us
			if (!cputime) {
				u64 cur_cputime = div64_u64(exec_runtime,
							    elapsed_cputime);
				// EMA with the scale factor (0.2)
				cputime =
					((cur_cputime << 3) + (cputime << 1)) /
					10;
			} else
				cputime = div64_u64(exec_runtime,
						    elapsed_cputime);

			/* to prevent frequent updates, allow for a slight variation of +/- 0.5% */
			if (cputime > (ksampled_soft_cpu_quota + 5) &&
			    sample_period != pcount) {
				/* need to increase the sample period */
				/* only increase by 1 */
				unsigned long tmp1 = sample_period,
					      tmp2 = sample_inst_period;
				increase_sample_period(&sample_period,
						       &sample_inst_period);
				if (tmp1 != sample_period ||
				    tmp2 != sample_inst_period)
					pebs_update_period(
						get_sample_period(
							sample_period),
						get_sample_inst_period(
							sample_inst_period));
			} else if (cputime < (ksampled_soft_cpu_quota - 5) &&
				   sample_period) {
				unsigned long tmp1 = sample_period,
					      tmp2 = sample_inst_period;
				decrease_sample_period(&sample_period,
						       &sample_inst_period);
				if (tmp1 != sample_period ||
				    tmp2 != sample_inst_period)
					pebs_update_period(
						get_sample_period(
							sample_period),
						get_sample_inst_period(
							sample_inst_period));
			}
			/* does it need to prevent ping-pong behavior? */

			elapsed_cputime = cur;
			exec_runtime = cur_runtime;
		}

		/* This is used for reporting the sample period and cputime */
		if (cur - trace_cputime >= trace_period) {
			unsigned long hr = 0;
			u64 cur_runtime = t->se.sum_exec_runtime;
			trace_runtime = cur_runtime - trace_runtime;
			trace_cputime = jiffies_to_usecs(cur - trace_cputime);
			trace_cputime = div64_u64(trace_runtime, trace_cputime);

			if (hr_dram + hr_nvm == 0)
				hr = 0;
			else
				hr = hr_dram * 10000 / (hr_dram + hr_nvm);
			trace_printk(
				"sample_period: %lu || cputime: %lu  || hit ratio: %lu\n",
				get_sample_period(sample_period), trace_cputime,
				hr);

			hr_dram = hr_nvm = 0;
			trace_cputime = cur;
			trace_runtime = cur_runtime;
		}
	}

	total_runtime = (t->se.sum_exec_runtime) - total_runtime; // ns
	total_cputime = jiffies_to_usecs(jiffies - total_cputime); // us

	printk("nr_sampled: %llu, nr_throttled: %llu, nr_lost: %llu\n",
	       nr_sampled, nr_throttled, nr_lost);
	printk("total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu\n",
	       total_runtime, total_cputime, (total_runtime) / total_cputime);

	return 0;
}

static int ksamplingd_run(void)
{
	int err = 0;

	if (!access_sampling) {
		access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd");
		if (IS_ERR(access_sampling)) {
			err = PTR_ERR(access_sampling);
			access_sampling = NULL;
		}
	}
	return err;
}

int ksamplingd_init(pid_t pid, int node)
{
	int ret;

	if (access_sampling)
		return 0;

	ret = pebs_init(pid, node);
	if (ret) {
		printk("htmm__perf_event_init failure... ERROR:%d\n", ret);
		return 0;
	}

	return ksamplingd_run();
}

void ksamplingd_exit(void)
{
	if (access_sampling) {
		kthread_stop(access_sampling);
		access_sampling = NULL;
	}
	pebs_disable();
}
