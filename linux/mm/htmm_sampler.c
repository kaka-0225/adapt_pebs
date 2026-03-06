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

// ============================================================================
// Phase 1: Adaptive-PEBS 堆数据结构
// ============================================================================

/**
 * heap_entry - 堆中的元素
 * @pinfo: 指向Page的pginfo，用于获取Welford方差数据
 * @event_hit_count: 该Event采样该Page的次数（最小堆的Key）
 */
struct heap_entry {
	pginfo_t *pinfo;
	u32 event_hit_count;
};

/**
 * event_heap - 最小堆结构，每个Event类型维护一个
 * @entries: 动态分配的堆元素数组
 * @size: 当前堆中的元素数量
 * @capacity: 堆的最大容量（默认1000）
 * @lock: 自旋锁，保护堆的并发访问
 */
struct event_heap {
	struct heap_entry *entries;
	u32 size;
	u32 capacity;
	spinlock_t lock;
};

/**
 * event_type - Event类型枚举（对应9种PEBS事件）
 */
enum event_type {
	EVENT_L1_HIT = 0,
	EVENT_L1_MISS,
	EVENT_L2_HIT,
	EVENT_L2_MISS,
	EVENT_L3_HIT,
	EVENT_L3_MISS,
	EVENT_DRAM_READ,
	EVENT_NVM_READ,
	EVENT_MEM_WRITE,
	EVENT_TYPE_MAX = 9
};

// 全局堆数组：9个Event各维护一个最小堆
static struct event_heap global_event_heaps[EVENT_TYPE_MAX];

// 堆容量配置（后续可通过sysfs调整）
static u32 heap_capacity = 1000;

// ============================================================================
// Phase 3.1: 自适应公式数据结构（Adaptive Metrics）
// ============================================================================

// 定点数精度：所有分数放大10000倍（0-10000表示0%-100%）
#define ADAPTIVE_SCALE 10000

// 三个维度的归一化上限
// 修复 #1: FLUC_MAX 适配真实方差量级（M2/n 比 M2 小约一个数量级）
// 原值 2×10^16 是针对 M2；改用方差后上限缩小 10 倍，后续通过 trace 校准
#define FLUC_MAX 2000000000000000ULL // 2×10^15，方差量级上限
#define HIT_MAX 100 // 热度阈值（event_hit_count平均值上限）
// 修复 #3: 调大窗口上限，避免正常工作负载下迅速饱和
// 按 NVMREAD period=1007, CPU=40 估算：10s 内约 40*10*1e9/1007 ≈ 400K 次
// 设为 200000，使正常采样不会轻易打到满分
#define OVERHEAD_MAX 200000 // 开销阈值：单 Event 10s 窗口内采样数上限

// 权重系数（定点数表示）
static const s32 WEIGHT_VIBRATE = 4000; // 0.40 * ADAPTIVE_SCALE = 40%
static const s32 WEIGHT_HOTNESS = 5000; // 0.50 * ADAPTIVE_SCALE = 50%
static const s32 WEIGHT_OVERHEAD =
	-1000; // -0.10 * ADAPTIVE_SCALE = -10%（负向惩罚）

// 全局开销计数器：统计每个Event的采样次数（用于V_overhead计算）
// Phase 3.1: 去掉 static 使 mm/htmm_core.c 可以访问
atomic64_t event_sample_counts[EVENT_TYPE_MAX];

// 每个Event的自适应指标
struct adaptive_metrics {
	u32 vibrate_score; // 波动性分数 [0, 10000]
	u32 hotness_score; // 热度分数 [0, 10000]
	u32 overhead_score; // 开销分数 [0, 10000]
	s32 V_raw; // 原始综合分数（可能为负）
	u32 V_normalized; // 归一化分数 [0, 10000]

	// Phase 3.2 新增字段：Period自适应更新
	u64 target_period; // 目标Period（从V_norm映射得到）
	u64 current_period; // 当前Period（从硬件读取）
	u64 new_period; // EMA平滑后的新Period
};

// 全局自适应指标数组（9个Event）
static struct adaptive_metrics global_adaptive_metrics[EVENT_TYPE_MAX];

// ============================================================================
// Phase 3.2: Period自适应更新配置
// ============================================================================

// EMA平滑系数（α = 0.3）
#define EMA_ALPHA_NUM 3 // 分子
#define EMA_ALPHA_DEN 10 // 分母，α = 3/10 = 0.3

// Period范围（对齐 Baseline pebs_period_list 边界）
#define MIN_PERIOD                                                             \
	199ULL // 对齐 pebs_period_list[0]（Baseline 最密，NVMREAD 初始值 1007 的 1/5）
#define MAX_PERIOD                                                             \
	19997ULL // 对齐 pebs_period_list[29]（Baseline 最稀，避免超出 Baseline 曾达到的范围）

// 全局开销预算
#define GLOBAL_OVERHEAD_BUDGET 50000 // 每10秒最多采样50000次

// 更新周期
#define ADAPTIVE_UPDATE_INTERVAL_SEC 10 // 10秒
#define ADAPTIVE_UPDATE_INTERVAL_MS                                            \
	(ADAPTIVE_UPDATE_INTERVAL_SEC * 1000) // 10000毫秒

// 定时器
static struct delayed_work adaptive_update_work;
static bool adaptive_timer_running = false;

// ============================================================================
// 函数前向声明
// ============================================================================
static int heap_init(struct event_heap *heap, u32 capacity);
static void heap_destroy(struct event_heap *heap);
static void heap_sift_up(struct event_heap *heap, u32 idx);
static void heap_sift_down(struct event_heap *heap, u32 idx);
static int heap_find(struct event_heap *heap, pginfo_t *pinfo);
static void pebs_disable(void);

// Phase 3.1: 自适应公式函数声明
static u32 calculate_vibrate_score(enum event_type type);
static u32 calculate_hotness_score(enum event_type type);
static u32 calculate_overhead_score(enum event_type type);
static void calculate_adaptive_metrics(void);
static void adaptive_metrics_init(void);

// Phase 3.2: Period自适应更新函数声明
static u64 map_score_to_period(u32 v_normalized);
static u64 apply_ema_to_period(u64 current_period, u64 target_period);
static u64 get_current_period(enum event_type type);
static void update_pebs_event_period(enum event_type type, u64 new_period);
void adaptive_update_work_handler(struct work_struct *work);
static void adaptive_timer_init(void);
static void adaptive_timer_stop(void);

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
	// 五级采样周期：对齐 Baseline 最优值
	if (type == L1_HIT || type == L1_MISS || type == MEMWRITE) {
		attr.sample_period = get_sample_inst_period(0); // 100,003
	} else if (type == L2_HIT || type == L2_MISS) {
		attr.sample_period = L2_SAMPLE_PERIOD; // 50,000
	} else if (type == L3_HIT || type == L3_MISS) {
		attr.sample_period = 5000;
	} else if (type == DRAMREAD) {
		attr.sample_period = 3007;
	} else if (type == NVMREAD) {
		attr.sample_period = 1007;
	} else {
		attr.sample_period = get_sample_period(0); // 199 fallback
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

	// ============================================================================
	// Phase 1: 初始化Event堆
	// ============================================================================
	// trace_printk("[Heap-Init] Initializing %d event heaps, capacity=%u\n",
	// EVENT_TYPE_MAX, heap_capacity);

	for (event = 0; event < EVENT_TYPE_MAX; event++) {
		int ret = heap_init(&global_event_heaps[event], heap_capacity);
		if (ret) {
			// trace_printk(
			// "[Heap-ERROR] Failed to init heap for event %d, ret=%d\n",
			// event, ret);

			// 清理已创建的堆
			while (--event >= 0)
				heap_destroy(&global_event_heaps[event]);

			// 清理PEBS资源
			pebs_disable();
			return ret;
		}
		//
		// trace_printk(
		// "[Heap-Init] Event %d (%s) heap created, capacity=%u\n",
		// event,
		// event == EVENT_L1_HIT	 ? "L1_HIT" :
		// event == EVENT_L1_MISS	 ? "L1_MISS" :
		// event == EVENT_L2_HIT	 ? "L2_HIT" :
		// event == EVENT_L2_MISS	 ? "L2_MISS" :
		// event == EVENT_L3_HIT	 ? "L3_HIT" :
		// event == EVENT_L3_MISS	 ? "L3_MISS" :
		// event == EVENT_DRAM_READ ? "DRAM_READ" :
		// event == EVENT_NVM_READ	 ? "NVM_READ" :
		// event == EVENT_MEM_WRITE ? "MEM_WRITE" :
		// "UNKNOWN",
		// heap_capacity);
	}
	//
	// trace_printk(
	// "[Heap-Init] All %d event heaps initialized successfully\n",
	// EVENT_TYPE_MAX);

	// ============================================================================
	// Phase 3.1: 初始化自适应指标系统
	// ============================================================================
	adaptive_metrics_init();

	return 0;
}

static void pebs_disable(void)
{
	int cpu, event;
	printk("pebs disable\n");

	// ========================================================================
	// Phase 3.2: 停止周期性Period自适应更新定时器
	// ========================================================================
	adaptive_timer_stop();

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

	// ============================================================================
	// Phase 3.1: 验证自适应指标计算（销毁堆前）
	// ============================================================================
	// trace_printk(
	// "[Adaptive-Trigger] Calculating metrics before heap destruction\n");
	calculate_adaptive_metrics();

	// ============================================================================
	// Phase 1: 清理Event堆
	// ============================================================================
	// trace_printk("[Heap-Destroy] Destroying %d event heaps\n",
	// EVENT_TYPE_MAX);

	for (event = 0; event < EVENT_TYPE_MAX; event++) {
		struct event_heap *heap = &global_event_heaps[event];
		//
		// trace_printk(
		// "[Heap-Destroy] Event %d (%s) heap destroyed, final_size=%u\n",
		// event,
		// event == EVENT_L1_HIT	 ? "L1_HIT" :
		// event == EVENT_L1_MISS	 ? "L1_MISS" :
		// event == EVENT_L2_HIT	 ? "L2_HIT" :
		// event == EVENT_L2_MISS	 ? "L2_MISS" :
		// event == EVENT_L3_HIT	 ? "L3_HIT" :
		// event == EVENT_L3_MISS	 ? "L3_MISS" :
		// event == EVENT_DRAM_READ ? "DRAM_READ" :
		// event == EVENT_NVM_READ	 ? "NVM_READ" :
		// event == EVENT_MEM_WRITE ? "MEM_WRITE" :
		// "UNKNOWN",
		// heap->size);

		heap_destroy(heap);
	}
	//
	// trace_printk("[Heap-Destroy] All event heaps destroyed\n");
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
		// trace_printk("[Welford-Init] pg=%px, init_time=%llu\n", pinfo,
		// now);
		return;
	}

	// ========== 第 2 步：计算间隔并缩放 ==========
	// ⚠️ FIX: PEBS 时间戳可能乱序！忽略时间倒退的样本
	if (unlikely(now <= pinfo->last_hit_time)) {
		// 时间戳倒退或相等，完全跳过本次样本
		// trace_printk(
		// "[Welford-SKIP-TIME-REWIND] pg=%px, now=%llu <= last=%llu (delta=%lld)\n",
		// pinfo, now, pinfo->last_hit_time,
		// (s64)(now - pinfo->last_hit_time));
		// 不更新 last_hit_time，不增加 adaptive_hit，完全忽略这个乱序样本
		return;
	}

	interval = now - pinfo->last_hit_time;
	x_scaled = interval << AP_SCALE_SHIFT; // 放大 1024 倍

	// 【trace_printk】：监控原始间隔值
	// 用途：判断 u64 是否会溢出，观察间隔分布
	// trace_printk(
	// "[Welford-Interval] pg=%px, raw_interval=%llu, scaled=%llu\n",
	// pinfo, interval, x_scaled);

	// ========== 第 3 步：更新时间戳 ==========
	pinfo->last_hit_time = now;

	// ========== 第 4 步：样本数递增 ==========
	n = ++pinfo->adaptive_hit;

	// 【trace_printk】：监控样本数，判断 u32 (42亿) 是否足够
	// 每达到 2^20 (约 100 万) 的倍数时打印一次里程碑
	if (unlikely((n & 0xFFFFF) == 0)) {
		// trace_printk(
		// "[Welford-Milestone] pg=%px, n=%u (every 1M samples)\n",
		// pinfo, n);
	}

	// ========== 第 5 步：Welford 方差计算 ==========

	// 步骤 5.1：delta = x - mean_{n-1}
	delta = (s64)x_scaled - (s64)pinfo->mean_interval;

	// 【trace_printk】：监控 delta1 范围
	// 用途：判断 s64 是否足够，观察是否有异常大的波动
	// trace_printk(
	// "[Welford-Delta1] pg=%px, delta=%lld, x_scaled=%llu, old_mean=%llu\n",
	// pinfo, delta, x_scaled, pinfo->mean_interval);

	// 步骤 5.2：mean_n = mean_{n-1} + delta / n
	// 注意：必须使用 div_s64 进行 64 位有符号除法
	pinfo->mean_interval += (u64)div_s64(delta, n);

	// 步骤 5.3：delta2 = x - mean_n
	delta2 = (s64)x_scaled - (s64)pinfo->mean_interval;

	// 【trace_printk】：监控 delta2 范围
	// trace_printk("[Welford-Delta2] pg=%px, delta2=%lld, new_mean=%llu\n",
	// pinfo, delta2, pinfo->mean_interval);

	// 步骤 5.4：M2_n = M2_{n-1} + delta * delta2
	// 注意：delta * delta2 会放大到 1024*1024 = 2^20 倍
	//       右移 AP_SCALE_SHIFT 恢复到 1024 倍精度
	pinfo->fluctuation += (u64)((delta * delta2) >> AP_SCALE_SHIFT);

	// ========== 【核心 trace_printk】：汇总所有字段数值 ==========
	// 这是最重要的日志，用于离线分析各字段的位数是否合适
	// trace_printk(
	// "[Welford-Summary] pg=%px | n=%u | mean=%llu | M2=%llu | var_approx=%llu | interval=%llu\n",
	// pinfo,
	// pinfo->adaptive_hit, // 样本数（u32，最大 ~42 亿）
	// pinfo->mean_interval, // 均值（u64，1024 倍缩放）
	// pinfo->fluctuation, // M2（u64，1024 倍缩放）
	// (pinfo->adaptive_hit > 1) ?
	// (pinfo->fluctuation / (pinfo->adaptive_hit - 1)) :
	// 0, // 近似标准差²
	// interval); // 原始间隔（未缩放）
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
						// trace_printk(
						// "[PEBS] CPU=%d Event=%d PID=%u TID=%u Addr=0x%llx IP=0x%llx Time=%llu\n",
						// cpu, event, he->pid,
						// he->tid, he->addr,
						// he->ip, he->time);
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
						// trace_printk(
						// "nr_sampled: %llu, nr_dram: %llu, nr_nvm: %llu, nr_write: %llu, nr_throttled: %llu \n",
						// nr_sampled, nr_dram,
						// nr_nvm, nr_write,
						// nr_throttled);
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
			/* Adaptive-PEBS: 当自适应定时器运行时，Period 由
			 * adaptive_update_work_handler 独立控制，跳过此处的
			 * CPU quota 全局覆盖，避免两个机制相互干扰。
			 */
			if (!adaptive_timer_running) {
				if (cputime > (ksampled_soft_cpu_quota + 5) &&
				    sample_period != pcount) {
					/* need to increase the sample period */
					/* only increase by 1 */
					unsigned long tmp1 = sample_period,
						      tmp2 = sample_inst_period;
					increase_sample_period(
						&sample_period,
						&sample_inst_period);
					if (tmp1 != sample_period ||
					    tmp2 != sample_inst_period)
						pebs_update_period(
							get_sample_period(
								sample_period),
							get_sample_inst_period(
								sample_inst_period));
				} else if (cputime < (ksampled_soft_cpu_quota -
						      5) &&
					   sample_period) {
					unsigned long tmp1 = sample_period,
						      tmp2 = sample_inst_period;
					decrease_sample_period(
						&sample_period,
						&sample_inst_period);
					if (tmp1 != sample_period ||
					    tmp2 != sample_inst_period)
						pebs_update_period(
							get_sample_period(
								sample_period),
							get_sample_inst_period(
								sample_inst_period));
				}
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
			// trace_printk(
			// "sample_period: %lu || cputime: %lu  || hit ratio: %lu\n",
			// get_sample_period(sample_period), trace_cputime,
			// hr);

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

// ============================================================================
// Phase 1: 堆操作函数
// ============================================================================

/**
 * heap_init - 初始化一个事件堆
 * @heap: 要初始化的堆结构
 * @capacity: 堆的最大容量
 *
 * 返回: 0表示成功，负数表示失败
 */
static int heap_init(struct event_heap *heap, u32 capacity)
{
	heap->entries =
		kmalloc_array(capacity, sizeof(struct heap_entry), GFP_KERNEL);
	if (!heap->entries) {
		// trace_printk("[Heap-ERROR] Failed to allocate %u entries\n",
		// capacity);
		return -ENOMEM;
	}

	heap->size = 0;
	heap->capacity = capacity;
	spin_lock_init(&heap->lock);

	return 0;
}

/**
 * heap_destroy - 销毁一个事件堆，释放内存
 * @heap: 要销毁的堆结构
 */
static void heap_destroy(struct event_heap *heap)
{
	if (heap->entries) {
		kfree(heap->entries);
		heap->entries = NULL;
	}
	heap->size = 0;
}

/**
 * heap_sift_up - 堆的向上调整（用于插入或增加计数后维护堆性质）
 * @heap: 目标堆
 * @idx: 需要调整的元素索引
 *
 * 最小堆性质：父节点的event_hit_count <= 子节点的event_hit_count
 */
static void heap_sift_up(struct event_heap *heap, u32 idx)
{
	struct heap_entry temp;
	u32 parent;

	while (idx > 0) {
		parent = (idx - 1) / 2;

		// 如果当前节点 >= 父节点，堆性质已满足
		if (heap->entries[idx].event_hit_count >=
		    heap->entries[parent].event_hit_count)
			break;

		// 交换父子节点
		temp = heap->entries[idx];
		heap->entries[idx] = heap->entries[parent];
		heap->entries[parent] = temp;

		idx = parent;
	}
}

/**
 * heap_sift_down - 堆的向下调整（用于替换堆顶后维护堆性质）
 * @heap: 目标堆
 * @idx: 需要调整的元素索引
 */
static void heap_sift_down(struct event_heap *heap, u32 idx)
{
	struct heap_entry temp;
	u32 child, right;

	while ((child = 2 * idx + 1) < heap->size) {
		right = child + 1;

		// 选择较小的子节点
		if (right < heap->size &&
		    heap->entries[right].event_hit_count <
			    heap->entries[child].event_hit_count) {
			child = right;
		}

		// 如果当前节点 <= 子节点，堆性质已满足
		if (heap->entries[idx].event_hit_count <=
		    heap->entries[child].event_hit_count)
			break;

		// 交换父子节点
		temp = heap->entries[idx];
		heap->entries[idx] = heap->entries[child];
		heap->entries[child] = temp;

		idx = child;
	}
}

/**
 * heap_find - 在堆中查找指定Page
 * @heap: 目标堆
 * @pinfo: 要查找的Page的pginfo指针
 *
 * 返回: 元素索引（>=0）或-1（未找到）
 * 注意: O(n)复杂度，Phase 2中可优化为哈希表加速
 */
static int heap_find(struct event_heap *heap, pginfo_t *pinfo)
{
	u32 i;

	for (i = 0; i < heap->size; i++) {
		if (heap->entries[i].pinfo == pinfo)
			return i;
	}

	return -1;
}

// 🆕 Adaptive-PEBS: 从event_id获取event_type枚举
static enum event_type get_event_type_from_id(int event_id)
{
	switch (event_id) {
	case 0:
		return EVENT_L1_HIT;
	case 1:
		return EVENT_L1_MISS;
	case 2:
		return EVENT_L2_HIT;
	case 3:
		return EVENT_L2_MISS;
	case 4:
		return EVENT_L3_HIT;
	case 5:
		return EVENT_L3_MISS;
	case 6:
		return EVENT_DRAM_READ;
	case 7:
		return EVENT_NVM_READ;
	case 8:
		return EVENT_MEM_WRITE;
	default:
		return EVENT_L1_HIT; // Fallback
	}
}

// 🆕 Adaptive-PEBS: 堆的更新或插入逻辑
static void heap_update_or_insert(struct event_heap *heap, pginfo_t *pinfo)
{
	int idx;
	unsigned long flags;

	spin_lock_irqsave(&heap->lock, flags);

	// 情况1: Page已在堆中 → 增加hit_count
	idx = heap_find(heap, pinfo);
	if (idx >= 0) {
		heap->entries[idx].event_hit_count++;
		heap_sift_up(heap, idx); // 重新排序
		// trace_printk("[Heap-Update] pinfo=%p new_hit=%u\n", pinfo,
		// heap->entries[idx].event_hit_count);
		spin_unlock_irqrestore(&heap->lock, flags);
		return;
	}

	// 情况2: 堆未满 → 直接插入
	if (heap->size < heap->capacity) {
		heap->entries[heap->size].pinfo = pinfo;
		heap->entries[heap->size].event_hit_count = 1;
		heap_sift_up(heap, heap->size);
		heap->size++;
		// trace_printk("[Heap-Insert] pinfo=%p heap_size=%d\n", pinfo,
		// heap->size);
		spin_unlock_irqrestore(&heap->lock, flags);
		return;
	}

	// 情况3: 堆已满 → 检查是否替换堆顶
	// 修复 #5: 原条件 < 1 永远为 false（新页 hit=1 永不满足）
	// 改为 <= 1：堆顶是冷页（hit ≤ 1）时用新页替换，引入新鲜血液
	if (heap->entries[0].event_hit_count <= 1) {
		// 堆顶是冷页，替换为新页
		heap->entries[0].pinfo = pinfo;
		heap->entries[0].event_hit_count = 1;
		heap_sift_down(heap, 0);
		// trace_printk("[Heap-Replace] pinfo=%p (evict cold top)\n",
		// pinfo);
	} else {
		// 堆顶已被多次命中（hit > 1），新页热度不足，丢弃
		// trace_printk(
		// "[Heap-Discard] pinfo=%p (heap full, top_hit=%u)\n",
		// pinfo, heap->entries[0].event_hit_count);
	}

	spin_unlock_irqrestore(&heap->lock, flags);
}

// 🆕 Adaptive-PEBS: 从PEBS采样更新堆（被htmm_core.c调用）
void update_event_heap_from_sample(int event_id, pginfo_t *pinfo)
{
	if (event_id < 0 || event_id >= EVENT_TYPE_MAX) {
		// trace_printk("[Heap-Error] Invalid event_id=%d\n", event_id);
		return;
	}

	heap_update_or_insert(&global_event_heaps[event_id], pinfo);
}

// ============================================================================
// Phase 3.1: 自适应公式计算函数
// ============================================================================

/**
 * calculate_vibrate_score - 计算波动性分数（基于Welford方差）
 * @type: Event类型
 * 
 * 算法：
 * 1. 遍历堆中所有页面，累加fluctuation
 * 2. 计算平均波动值 avg_fluc = sum / count
 * 3. 归一化：score = min(avg_fluc * ADAPTIVE_SCALE / FLUC_MAX, ADAPTIVE_SCALE)
 * 
 * 返回：波动性分数 [0, 10000]
 */
static u32 calculate_vibrate_score(enum event_type type)
{
	struct event_heap *heap = &global_event_heaps[type];
	u64 sum_fluctuation = 0;
	u32 count = 0;
	u32 i;
	u64 avg_fluc;
	u64 score;

	if (!heap || heap->size == 0) {
		return 0; // 空堆返回0分
	}

	// 遍历堆中所有页面，累加真实方差（M2/n）
	// 修复 #1: 原来直接累加 M2，M2 随 n 单调增，不反映真实波动性
	// 改为累加 variance = M2/n，用 div64_u64 做内核合规的 64 位除法
	for (i = 0; i < heap->size; i++) {
		struct heap_entry *entry = &heap->entries[i];
		pginfo_t *pinfo = entry->pinfo;
		if (pinfo) {
			u32 n = (pinfo->adaptive_hit > 1) ?
					pinfo->adaptive_hit :
					1;
			sum_fluctuation +=
				div64_u64(pinfo->fluctuation, (u64)n);
			count++;
		}
	}

	if (count == 0) {
		return 0;
	}

	// 计算平均方差（用 div64_u64 避免内核 64 位除法问题）
	avg_fluc = div64_u64(sum_fluctuation, (u64)count);

	// 归一化到 [0, ADAPTIVE_SCALE]
	// score = min(avg_fluc * ADAPTIVE_SCALE / FLUC_MAX, ADAPTIVE_SCALE)
	if (avg_fluc >= FLUC_MAX) {
		score = ADAPTIVE_SCALE;
	} else {
		score = div64_u64(avg_fluc * ADAPTIVE_SCALE, FLUC_MAX);
	}

	return (u32)score;
}

/**
 * calculate_hotness_score - 计算热度分数（基于hit_count + 堆密度）
 * @type: Event类型
 * 
 * 算法：
 * 1. 遍历堆中所有页面，累加event_hit_count
 * 2. 计算平均热度 avg_hit = sum / count
 * 3. 计算堆密度加成 density_bonus = (heap->size * 100) / heap->capacity
 *    - 堆越满，说明热页越多，加成越大（0-100分）
 * 4. 基础分数：base_score = min(avg_hit * ADAPTIVE_SCALE / HIT_MAX, ADAPTIVE_SCALE)
 * 5. 最终分数：score = base_score + density_bonus（上限ADAPTIVE_SCALE）
 * 
 * 返回：热度分数 [0, 10000]
 */
static u32 calculate_hotness_score(enum event_type type)
{
	struct event_heap *heap = &global_event_heaps[type];
	u64 sum_hit_count = 0;
	u32 count = 0;
	u32 i;
	u64 avg_hit;
	u32 base_score;
	u32 density_bonus;
	u32 final_score;

	if (!heap || heap->size == 0) {
		return 0; // 空堆返回0分
	}

	// 遍历堆中所有页面，累加hit_count
	for (i = 0; i < heap->size; i++) {
		struct heap_entry *entry = &heap->entries[i];
		sum_hit_count += entry->event_hit_count;
		count++;
	}

	if (count == 0) {
		return 0;
	}

	// 计算平均热度
	avg_hit = div64_u64(sum_hit_count, (u64)count);

	// 基础分数：归一化到 [0, ADAPTIVE_SCALE]
	if (avg_hit >= HIT_MAX) {
		base_score = ADAPTIVE_SCALE;
	} else {
		base_score =
			(u32)div64_u64(avg_hit * ADAPTIVE_SCALE, (u64)HIT_MAX);
	}

	// 堆密度加成：堆越满，热页越多（0-100分）
	// density_bonus = (heap->size * 100) / heap->capacity
	if (heap->capacity > 0) {
		density_bonus = (heap->size * 100) / heap->capacity;
	} else {
		density_bonus = 0;
	}

	// 最终分数 = 基础分数 + 密度加成（上限10000）
	final_score = base_score + density_bonus;
	if (final_score > ADAPTIVE_SCALE) {
		final_score = ADAPTIVE_SCALE;
	}

	return final_score;
}

/**
 * calculate_overhead_score - 计算开销分数（基于采样计数）
 * @type: Event类型
 * 
 * 算法：
 * 1. 读取event_sample_counts[type]（每次update_base_page时递增）
 * 2. 归一化：score = min(count * ADAPTIVE_SCALE / OVERHEAD_MAX, ADAPTIVE_SCALE)
 * 3. 注意：这是负向指标，最终会乘以负权重
 * 
 * 返回：开销分数 [0, 10000]
 */
static u32 calculate_overhead_score(enum event_type type)
{
	u64 sample_count;
	u64 score;

	// 读取采样计数
	sample_count = atomic64_read(&event_sample_counts[type]);

	// 归一化到 [0, ADAPTIVE_SCALE]
	if (sample_count >= OVERHEAD_MAX) {
		score = ADAPTIVE_SCALE;
	} else {
		score = div64_u64(sample_count * ADAPTIVE_SCALE,
				  (u64)OVERHEAD_MAX);
	}

	return (u32)score;
}

/**
 * calculate_adaptive_metrics - 综合计算所有Event的自适应分数
 * 
 * Phase 3.1功能：计算三维度分数并归一化到[0,10000]
 * Phase 3.2会在delayed_work中周期性调用此函数，根据分数更新Period
 * 
 * 算法：
 * 1. 遍历9个Event，分别计算三个维度分数
 * 2. 计算原始分数：V_raw = β·V_vibrate + γ·V_hotness + δ·V_overhead
 * 3. 归一化到[0, ADAPTIVE_SCALE]：
 *    - V_min = -1000 (最差情况：振动0，热度0，开销满10000)
 *    - V_max = 9000 (最好情况：振动满10000，热度满10000，开销0)
 *    - V_normalized = (V_raw - V_min) * ADAPTIVE_SCALE / (V_max - V_min)
 * 4. 通过trace_printk输出每个Event的分数（Phase 3.1验证用）
 */
static void calculate_adaptive_metrics(void)
{
	enum event_type type;
	s32 V_min = -1000; // 最小可能分数 = 0*4000 + 0*5000 + 10000*(-1000)
	s32 V_max =
		9000; // 最大可能分数 = 10000*4000 + 10000*5000 + 0*(-1000) = 90000000 / 10000 = 9000
	s32 V_range = V_max - V_min; // 10000
	//
	// trace_printk(
	// "[Adaptive-Start] ===== Calculate Adaptive Metrics =====\n");

	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		struct adaptive_metrics *metrics =
			&global_adaptive_metrics[type];
		u32 vibrate, hotness, overhead;
		s64 V_raw_calc;
		s32 V_raw;
		s64 V_norm_calc;

		// Step 1: 计算三个维度分数
		vibrate = calculate_vibrate_score(type);
		hotness = calculate_hotness_score(type);
		overhead = calculate_overhead_score(type);

		// 保存到metrics结构
		metrics->vibrate_score = vibrate;
		metrics->hotness_score = hotness;
		metrics->overhead_score = overhead;

		// Step 2: 计算原始综合分数（定点数运算）
		// V_raw = (β·V_vibrate + γ·V_hotness + δ·V_overhead) / ADAPTIVE_SCALE
		// 注意：WEIGHT_OVERHEAD是负数
		V_raw_calc = ((s64)WEIGHT_VIBRATE * vibrate +
			      (s64)WEIGHT_HOTNESS * hotness +
			      (s64)WEIGHT_OVERHEAD * overhead) /
			     ADAPTIVE_SCALE;
		V_raw = (s32)V_raw_calc;
		metrics->V_raw = V_raw;

		// Step 3: 归一化到[0, ADAPTIVE_SCALE]
		// V_normalized = (V_raw - V_min) * ADAPTIVE_SCALE / V_range
		if (V_raw <= V_min) {
			metrics->V_normalized = 0;
		} else if (V_raw >= V_max) {
			metrics->V_normalized = ADAPTIVE_SCALE;
		} else {
			V_norm_calc = ((s64)(V_raw - V_min) * ADAPTIVE_SCALE) /
				      V_range;
			metrics->V_normalized = (u32)V_norm_calc;
		}

		// Step 4: 输出调试信息（Phase 3.1验证用）
		// trace_printk(
		// "[Adaptive-Score] Event=%d Vibrate=%u Hotness=%u Overhead=%u V_raw=%d V_norm=%u\n",
		// type, vibrate, hotness, overhead, V_raw,
		// metrics->V_normalized);
	}
	//
	// trace_printk("[Adaptive-Final] ===== Calculation Complete =====\n");
}

/**
 * adaptive_metrics_init - 初始化自适应指标系统
 * 
 * 在pebs_init时调用，初始化：
 * 1. event_sample_counts数组（开销计数器）
 * 2. global_adaptive_metrics数组（分数存储）
 */
static void adaptive_metrics_init(void)
{
	enum event_type type;

	// 初始化开销计数器
	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		atomic64_set(&event_sample_counts[type], 0);
	}

	// 初始化自适应指标
	memset(global_adaptive_metrics, 0, sizeof(global_adaptive_metrics));
	//
	// trace_printk("[Adaptive-Init] Adaptive metrics system initialized\n");
	// ========================================================================
	// Phase 3.2: 启动周期性Period自适应更新定时器
	// ========================================================================
	adaptive_timer_init();
}

// ============================================================================
// Phase 3.2: Period自适应更新函数实现
// ============================================================================

/**
 * map_score_to_period - 将归一化分数映射到目标Period
 * @v_normalized: 归一化分数 [0, 10000]
 * 
 * 返回：目标Period [MIN_PERIOD, MAX_PERIOD]
 * 
 * 算法：逆向线性映射
 *   V_norm=0 (0%)     → Period=19,997 (最低频率)
 *   V_norm=5000 (50%) → Period=10,098 (中等频率)
 *   V_norm=10000(100%)→ Period=199    (最高频率)
 * 
 * 公式：Period = MAX_PERIOD - (V_norm × range / ADAPTIVE_SCALE)
 */
static u64 map_score_to_period(u32 v_normalized)
{
	u64 period;
	u64 range = MAX_PERIOD - MIN_PERIOD; // 19798

	if (v_normalized >= ADAPTIVE_SCALE) {
		// 分数满分 → 最小Period（最高频率）
		period = MIN_PERIOD;
	} else if (v_normalized == 0) {
		// 分数为0 → 最大Period（最低频率）
		period = MAX_PERIOD;
	} else {
		// 线性映射：period = MAX - (v_norm × range / SCALE)
		period = MAX_PERIOD -
			 ((u64)v_normalized * range / ADAPTIVE_SCALE);
	}

	return period;
}

/**
 * apply_ema_to_period - 使用EMA平滑Period调整
 * @current_period: 当前Period（从硬件读取）
 * @target_period: 目标Period（从分数映射得到）
 * 
 * 返回：EMA平滑后的新Period [MIN_PERIOD, MAX_PERIOD]
 * 
 * 算法：指数移动平均（EMA, α=0.3）
 *   new_period = α × target_period + (1-α) × current_period
 *   new_period = (3 × target + 7 × current) / 10
 * 
 * 效果：Period从current逐渐调整到target，避免突变
 * 
 * 示例：
 *   current=100000, target=80000
 *   → new = (3×80000 + 7×100000)/10 = 94000
 *   → 下次: new = (3×80000 + 7×94000)/10 = 89800
 *   → 逐渐收敛到80000
 */
static u64 apply_ema_to_period(u64 current_period, u64 target_period)
{
	u64 new_period;

	// EMA公式：new = (α_num×target + (α_den - α_num)×current) / α_den
	//         new = (3×target + 7×current) / 10
	new_period = (EMA_ALPHA_NUM * target_period +
		      (EMA_ALPHA_DEN - EMA_ALPHA_NUM) * current_period) /
		     EMA_ALPHA_DEN;

	// 边界限制
	if (new_period < MIN_PERIOD)
		new_period = MIN_PERIOD;
	else if (new_period > MAX_PERIOD)
		new_period = MAX_PERIOD;

	return new_period;
}

/**
 * get_current_period - 从perf_event读取当前Period
 * @type: Event类型
 * 
 * 返回：当前Period，失败返回0
 * 
 * 遍历所有CPU，找到第一个匹配的Event并读取其Period
 */
static u64 get_current_period(enum event_type type)
{
	int cpu;
	int event_idx;
	u64 period = 0;

	// 遍历所有CPU，找到第一个匹配的Event
	for_each_online_cpu (cpu) {
		if (!mem_event || !mem_event[cpu])
			continue;

		for (event_idx = 0; event_idx < N_HTMMEVENTS; event_idx++) {
			if (get_event_type_from_id(event_idx) == type) {
				struct perf_event *event =
					mem_event[cpu][event_idx];
				if (event) {
					period = event->attr.sample_period;
					return period; // 返回第一个找到的
				}
			}
		}
	}

	return period;
}

/**
 * update_pebs_event_period - 更新所有CPU的Event Period
 * @type: Event类型
 * @new_period: 新的Period值
 * 
 * 遍历所有CPU，更新匹配的Event的Period
 * 必须先disable，修改后再enable
 */
static void update_pebs_event_period(enum event_type type, u64 new_period)
{
	int cpu, event_idx;

	// 遍历所有CPU
	for_each_online_cpu (cpu) {
		if (!mem_event || !mem_event[cpu])
			continue;

		for (event_idx = 0; event_idx < N_HTMMEVENTS; event_idx++) {
			if (get_event_type_from_id(event_idx) == type) {
				struct perf_event *event =
					mem_event[cpu][event_idx];
				if (event) {
					// 禁用 → 修改 → 启用
					perf_event_disable(event);

					event->attr.sample_period = new_period;
					event->hw.sample_period = new_period;
					local64_set(&event->hw.period_left,
						    new_period);

					perf_event_enable(event);
				}
			}
		}
	}
}

/**
 * adaptive_update_work_handler - 定时器回调，执行Period自适应更新
 * @work: delayed_work结构体
 * 
 * 每10秒执行一次，遍历所有Event类型：
 * 1. 读取当前分数
 * 2. 计算目标Period
 * 3. EMA平滑调整
 * 4. 更新硬件Period
 * 5. 重新调度定时器
 */
void adaptive_update_work_handler(struct work_struct *work)
{
	enum event_type type;

	printk(KERN_INFO "[HTMM-Adaptive] Timer callback triggered\n");
	// trace_printk("[Adaptive-Update] ===== Periodic update triggered =====\n");

	// -----------------------------------------------------------------------
	// 堆老化：对所有 Event 堆的 hit_count 进行指数衰减（每轮 ×0.7）
	// 目的：使长期不被采样的页面 hit_count 自然衰减趋向 0/1，
	//       配合修复#5 的替换条件（<=1），确保新热页能持续进入堆。
	// 衰减后用 Floyd 算法（从最后非叶子节点向上 sift_down）重建堆性质。
	// 必须在 calculate_adaptive_metrics() 之前执行，保证分数使用最新堆状态。
	// -----------------------------------------------------------------------
	{
		u32 heap_i;
		unsigned long heap_flags;

		for (type = 0; type < EVENT_TYPE_MAX; type++) {
			struct event_heap *heap = &global_event_heaps[type];

			spin_lock_irqsave(&heap->lock, heap_flags);

			// 对每个页面的 hit_count 乘以 7/10（衰减 30%）
			for (heap_i = 0; heap_i < heap->size; heap_i++)
				heap->entries[heap_i].event_hit_count =
					heap->entries[heap_i].event_hit_count *
					7 / 10;

			// Floyd 建堆：从最后一个非叶子节点向上重建最小堆性质
			if (heap->size > 1) {
				for (heap_i = heap->size / 2; heap_i-- > 0;)
					heap_sift_down(heap, heap_i);
			}

			spin_unlock_irqrestore(&heap->lock, heap_flags);
		}
	}

	// 计算最新的自适应分数
	// trace_printk("[Adaptive-Update] Calculating current metrics...\n");
	calculate_adaptive_metrics();

	// 修复 #3：重置开销计数器，使 overhead 反映当前 10s 窗口的实时采样率
	// 必须在 calculate_adaptive_metrics() 之后重置，避免本轮分数丢失
	for (type = 0; type < EVENT_TYPE_MAX; type++)
		atomic64_set(&event_sample_counts[type], 0);

	// 遍历所有Event类型
	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		struct adaptive_metrics *metrics =
			&global_adaptive_metrics[type];
		u64 current_score, target_period, current_period, new_period;

		// 1. 读取当前分数
		current_score = metrics->V_normalized;
		if (current_score == 0) {
			// trace_printk(
			// "[Adaptive-Update] Event %d: score=0, skipping\n",
			// type);
			continue;
		}

		// 2. 计算目标Period
		target_period = map_score_to_period((u32)current_score);

		// 3. 读取当前Period
		current_period = get_current_period(type);
		if (current_period == 0) {
			// trace_printk(
			// "[Adaptive-Update] Event %d: failed to get current period\n",
			// type);
			continue;
		}

		// 4. EMA平滑调整
		new_period = apply_ema_to_period(current_period, target_period);

		// 5. 更新硬件Period
		update_pebs_event_period(type, new_period);
		//
		// trace_printk(
		// "[Adaptive-Period] Event %d: Period updated %llu → %llu (EMA smoothed)\n",
		// type, current_period, new_period);
	}
	//
	// trace_printk("[Adaptive-Update] ===== Update completed =====\n");

	// 重新调度下一次定时器 (10秒后)
	if (adaptive_timer_running) {
		schedule_delayed_work(
			&adaptive_update_work,
			msecs_to_jiffies(ADAPTIVE_UPDATE_INTERVAL_MS));
	}
}
EXPORT_SYMBOL(adaptive_update_work_handler);

/**
 * adaptive_timer_init - 启动周期性Period自适应更新定时器
 * 
 * 在pebs_init时调用，启动10秒周期的定时器
 */
static void adaptive_timer_init(void)
{
	// trace_printk("[Adaptive-Timer] Initializing periodic update timer (interval=%d ms)\n",
	// ADAPTIVE_UPDATE_INTERVAL_MS);

	adaptive_timer_running = true;
	INIT_DELAYED_WORK(&adaptive_update_work, adaptive_update_work_handler);
	schedule_delayed_work(&adaptive_update_work,
			      msecs_to_jiffies(ADAPTIVE_UPDATE_INTERVAL_MS));
	//
	// trace_printk("[Adaptive-Timer] Timer started successfully\n");
}

/**
 * adaptive_timer_stop - 停止周期性Period自适应更新定时器
 * 
 * 在pebs_disable时调用，停止定时器
 */
static void adaptive_timer_stop(void)
{
	// trace_printk("[Adaptive-Timer] Stopping periodic update timer\n");

	adaptive_timer_running = false;
	cancel_delayed_work_sync(&adaptive_update_work);
	//
	// trace_printk("[Adaptive-Timer] Timer stopped successfully\n");
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
