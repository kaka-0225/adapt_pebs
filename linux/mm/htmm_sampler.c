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
#include <linux/math64.h> // 内核 64 位除法支持
#include <linux/log2.h> // ilog2() 对数函数

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
// F6-fix: 增大 heap 容量以提高 renewal 指标的灵敏度
// 10000 条内存开销：10000 × 16B × 9 events = 1.44MB（可接受）
static u32 heap_capacity = 10000;

// ============================================================================
// Phase 3.1: 自适应公式数据结构（Adaptive Metrics）
// ============================================================================

// 定点数精度：所有分数放大10000倍（0-10000表示0%-100%）
#define ADAPTIVE_SCALE 10000

// 三个维度的归一化上限
#define FLUC_MAX 2000000000000000ULL // 2×10^15，方差量级上限
#define HIT_SATURATE 100 // ilog2饱和点：avg_hit超过此值后分数不再增长

// 权重系数（定点数表示）
static const s32 WEIGHT_VIBRATE = 3000; // 0.30 * ADAPTIVE_SCALE = 30%
static const s32 WEIGHT_RENEWAL =
	2000; // 0.20 * ADAPTIVE_SCALE = 20% (F10: 0.4→0.2)
static const s32 WEIGHT_CONFIDENCE =
	5000; // 0.50 * ADAPTIVE_SCALE = 50% (F10: 0.3→0.5)

// 堆统计结构：用于计算V_renewal
struct heap_stats {
	u64 total_samples; // 本窗口总采样数
	u64 heap_insertions; // 本窗口新页插入次数
	u64 heap_replacements; // 本窗口堆顶替换次数
};

static struct heap_stats global_heap_stats[EVENT_TYPE_MAX];

// 每个Event的自适应指标
struct adaptive_metrics {
	u32 vibrate_score; // 波动性分数 [0, 10000]
	u32 renewal_score; // 堆更新率分数 [0, 10000]
	u32 confidence_score; // 对数饱和热度分数 [0, 10000]
	s32 V_raw; // 原始综合分数
	u32 V_normalized; // 归一化分数 [0, 10000]

	// Phase 3.2: Period自适应更新
	u64 target_period;
	u64 current_period;
	u64 new_period;
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

// F2-fix: 初始 Period 使用中间值，避免 MIN 的启动风暴和 MAX 的冷启动陷阱
// pebs_period_list[15] = 3001：比 MIN(199) 低 15 倍采样率，比 MAX(19997) 高 6.6 倍
#define INIT_PERIOD 3001ULL

// 全局采样预算控制（优化1.1）
#define MAX_SAMPLES_PER_WINDOW 2000000 // 10秒窗口最多200万次采样
#define SCALE_UP_FACTOR 15 // Period放大系数：1.5倍
#define SCALE_DOWN_FACTOR 10 // 分母

// 更新周期
#define ADAPTIVE_UPDATE_INTERVAL_SEC 10 // 10秒
#define ADAPTIVE_UPDATE_INTERVAL_MS                                            \
	(ADAPTIVE_UPDATE_INTERVAL_SEC * 1000) // 10000毫秒

// 定时器
static struct delayed_work adaptive_update_work;
static bool adaptive_timer_running = false;

// Bug Fix #6: 内部Period跟踪数组
// 避免读取event->attr.sample_period（可能被perf throttle篡改）
static u64 tracked_periods[EVENT_TYPE_MAX];

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
static u32 calculate_renewal_score(enum event_type type);
static u32 calculate_confidence_score(enum event_type type);
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
	case L1_MISS:
	case L2_HIT:
	case L2_MISS:
	case L3_HIT:
	case L3_MISS:
		/* GUPS_Test/test1: disable L1-L3 events, align with Baseline */
		return N_HTMMEVENTS;
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
	/* GUPS_Test/test1: align with Baseline periods
	 * DRAMREAD=199, NVMREAD=199, MEMWRITE=100003
	//  * L1-L3 disabled in get_pebs_event(), won't reach here */
	// if (type == MEMWRITE) {
	// 	attr.sample_period = get_sample_inst_period(0); // 100,003
	// } else if (type == DRAMREAD || type == NVMREAD) {
	// 	attr.sample_period = get_sample_period(0); // 199
	// } else {
	// 	attr.sample_period = get_sample_period(0); // 199 fallback
	// }
	/* F2-fix: 初始Period使用中间值INIT_PERIOD=3001
	 * 原Bug Fix #7用MIN_PERIOD=199导致启动风暴→heap秒满→renewal=0→死亡螺旋。
	 * 原代码用MAX_PERIOD=19997导致冷启动陷阱→无采样→adaptive无意义。
	 * INIT_PERIOD=3001(pebs_period_list[15])平衡两端：
	 *   10秒内~3.3万次采样，足够启动但不会淹没heap。
	 * MEMWRITE保持原始配置值。
	 */
	if (type == MEMWRITE) {
		attr.sample_period = htmm_inst_sample_period;
	} else {
		attr.sample_period = INIT_PERIOD; /* 3001: 中间值启动 */
	}

	attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR |
			   PERF_SAMPLE_TIME;
	attr.disabled = 0;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_callchain_kernel = 1;
	attr.exclude_callchain_user = 1;
	attr.precise_ip = 1;
	attr.inherit = 1;
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
	/* Bug Fix #11: 原来调用了两次fget，泄漏了第一次的文件引用计数 */
	mem_event[cpu][type] = file->private_data;
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

	// printk("pebs_init\n");

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
	// printk("pebs disable\n");

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

	// printk("pebs enable\n");
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
	/* Bug Fix #10: Welford算法中delta*delta2数学上恒非负，
	 * 但整数除法截断可能导致微小负值，
	 * 强转为u64会下溢回绕为巨大正数污染fluctuation。 */
	{
		s64 m2_delta = (delta * delta2) >> AP_SCALE_SHIFT;
		if (m2_delta > 0)
			pinfo->fluctuation += (u64)m2_delta;
	}

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
	const struct cpumask *cpumask;

	/* for analytic purpose */
	unsigned long hr_dram = 0, hr_nvm = 0;

	/* orig impl: see read_sum_exec_runtime() */
	trace_runtime = total_runtime = exec_runtime = t->se.sum_exec_runtime;

	trace_cputime = total_cputime = elapsed_cputime = jiffies;
	sleep_timeout = usecs_to_jiffies(2000);

	/* TODO implements per-CPU node ksamplingd by using pg_data_t */
	/* Currently uses a single CPU node(0) */
	cpumask = cpumask_of_node(0);
	if (!cpumask_empty(cpumask))
		do_set_cpus_allowed(access_sampling, cpumask);

	while (!kthread_should_stop()) {
		int cpu, event, cond = false;

		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}

		for_each_online_cpu (cpu) {
			if (kthread_should_stop())
				break;
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

						/* Event IDs: 0=L1_HIT, 1=L1_MISS, 2=L2_HIT, 3=L2_MISS,
						 *             4=L3_HIT, 5=L3_MISS, 6=DRAMREAD, 7=NVMREAD, 8=MEMWRITE */

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
						count_vm_event(HTMM_NR_SAMPLED);
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

					/* 避免长时间独占CPU导致soft lockup */
					if (cond)
						cond_resched();
					/* Bug Fix #13: 内循环也检查stop信号，
					 * 防止ring buffer残留数据导致无限循环 */
					if (kthread_should_stop())
						break;
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
			/* Adaptive-PEBS: 当自适应定时器运行时，Period
			 * 由 adaptive_update_work_handler 控制，此处
			 * 仅作为硬上限安全网：CPU 超过 HARD_CPU_LIMIT
			 * 时强制增加 Period 防止 soft lockup。
			 * 不运行自适应定时器时保留原始双向调节逻辑。
			 */
			if (adaptive_timer_running) {
				/* F3-fix: 恢复 CPU cap 安全阀
				 * CPU 超限时放大 tracked_periods[]，
				 * adaptive timer 下次回调以新基准值做 EMA。
				 * 不直接调用 pebs_update_period() 避免与
				 * adaptive timer 的 EMA 逻辑冲突。 */
				if (cputime > (ksampled_soft_cpu_quota + 5)) {
					enum event_type etype;
					for (etype = 0; etype < EVENT_TYPE_MAX;
					     etype++) {
						u64 cur_p =
							tracked_periods[etype];
						if (cur_p == 0)
							continue;
						cur_p = cur_p *
							SCALE_UP_FACTOR /
							SCALE_DOWN_FACTOR;
						if (cur_p > MAX_PERIOD)
							cur_p = MAX_PERIOD;
						tracked_periods[etype] = cur_p;
					}
				}
			} else {
				if (cputime > (ksampled_soft_cpu_quota + 5) &&
				    sample_period != pcount) {
					/* need to increase the sample period */
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

	// printk("nr_sampled: %llu, nr_throttled: %llu, nr_lost: %llu\n",
	//        nr_sampled, nr_throttled, nr_lost);
	// printk("total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu\n",
	//        total_runtime, total_cputime, (total_runtime) / total_cputime);

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

// Adaptive-PEBS: get event_type from event_id
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

// Adaptive-PEBS: heap update/insert logic
static void heap_update_or_insert(struct event_heap *heap, pginfo_t *pinfo,
				  enum event_type type)
{
	int idx;
	unsigned long flags;

	spin_lock_irqsave(&heap->lock, flags);

	// 情况1: Page已在堆中 → 增加hit_count
	idx = heap_find(heap, pinfo);
	if (idx >= 0) {
		heap->entries[idx].event_hit_count++;
		/* Bug Fix #9: hit_count增大后应向下调整（sift_down）而非sift_up。
		 * 这是最小堆，值增大可能违反"父≤子"约束的下半部分，
		 * 原来用sift_up会立即停止，导致堆性质被破坏。 */
		heap_sift_down(heap, idx);
		spin_unlock_irqrestore(&heap->lock, flags);
		return;
	}

	// 情况2: 堆未满 → 直接插入
	if (heap->size < heap->capacity) {
		heap->entries[heap->size].pinfo = pinfo;
		heap->entries[heap->size].event_hit_count = 1;
		heap_sift_up(heap, heap->size);
		heap->size++;
		global_heap_stats[type].heap_insertions++;
		spin_unlock_irqrestore(&heap->lock, flags);
		return;
	}

	// 情况3: 堆已满 → 检查是否替换堆顶
	if (heap->entries[0].event_hit_count <= 1) {
		heap->entries[0].pinfo = pinfo;
		heap->entries[0].event_hit_count = 1;
		heap_sift_down(heap, 0);
		global_heap_stats[type].heap_replacements++;
	}

	spin_unlock_irqrestore(&heap->lock, flags);
}

// Adaptive-PEBS: update heap from PEBS sample (called by htmm_core.c)
void update_event_heap_from_sample(int event_id, pginfo_t *pinfo)
{
	enum event_type type;

	if (event_id < 0 || event_id >= EVENT_TYPE_MAX)
		return;

	type = (enum event_type)event_id;
	global_heap_stats[type].total_samples++;
	heap_update_or_insert(&global_event_heaps[type], pinfo, type);
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
 * calculate_renewal_score - 计算堆更新率分数
 * @type: Event类型
 *
 * 算法：renewal_rate = (insertions + replacements) / total_samples
 * 高更新率 = Event还在发现新热页 → 有价值
 * 低更新率 = Event反复采样已知页面 → 边际收益低
 *
 * 返回：更新率分数 [0, 10000]
 */
static u32 calculate_renewal_score(enum event_type type)
{
	struct heap_stats *stats = &global_heap_stats[type];
	u64 total = stats->total_samples;
	u64 renewals = stats->heap_insertions + stats->heap_replacements;
	struct event_heap *heap = &global_event_heaps[type];
	u32 cap = (heap->capacity > 0) ? heap->capacity : 1;
	u32 score;

	if (total == 0)
		return 0;

	/* F8-fix: 分母改为 heap_capacity（堆翻转率），而非 total_samples（采样命中率）。
	 * 旧公式在大工作集(>>heap_capacity)下 renewals≈total → score≈100%锁死。
	 * 新公式：score = renewals / capacity，受堆大小约束。 */
	score = div64_u64(renewals * ADAPTIVE_SCALE, (u64)cap);

	/* F7-fix: renewal 下限保护
	 * heap 已满且有采样活动但无新发现时，设置 5% 下限。
	 * 防止均匀访问模式下 V_norm 永久归零导致 period 冻结。
	 * 5% 贡献 V_renewal = 500×0.4 = 200，对应 period ≈ 19000 */
	if (score == 0 && total > 0) {
		if (heap->size >= heap->capacity)
			score = ADAPTIVE_SCALE / 20; /* 5% 下限 */
	}

	return min_t(u32, score, ADAPTIVE_SCALE);
}

/**
 * calculate_confidence_score - 计算对数饱和热度分数
 * @type: Event类型
 *
 * 算法：score = ilog2(avg_hit + 1) * ADAPTIVE_SCALE / ilog2(HIT_SATURATE + 1)
 * 使用对数曲线实现"够了就行"：
 * - hit=1→10: 分数快速上升（确认热度阶段）
 * - hit=50→500: 分数增长很慢（已饱和，多采无益）
 *
 * 返回：热度分数 [0, 10000]
 */
static u32 calculate_confidence_score(enum event_type type)
{
	struct event_heap *heap = &global_event_heaps[type];
	u64 sum_hit_count = 0;
	u32 count = 0;
	u32 avg_hit, score;
	int i;

	if (!heap || heap->size == 0)
		return 0;

	for (i = 0; i < heap->size; i++) {
		sum_hit_count += heap->entries[i].event_hit_count;
		count++;
	}

	if (count == 0)
		return 0;

	avg_hit = div_u64(sum_hit_count, count);

	if (avg_hit == 0)
		return 0;

	// ilog2(101) = 6, 为了更平滑使用7作为除数
	score = ilog2(avg_hit + 1) * ADAPTIVE_SCALE / 7;

	return min_t(u32, score, ADAPTIVE_SCALE);
}

/**
 * calculate_adaptive_metrics - 综合计算所有Event的自适应分数
 *
 * 新公式：V = 0.3×V_vibrate + 0.4×V_renewal + 0.3×V_confidence
 * 无负权重，V_raw范围 [0, 10000]
 */
static void calculate_adaptive_metrics(void)
{
	enum event_type type;

	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		struct adaptive_metrics *m = &global_adaptive_metrics[type];
		u32 vibrate, renewal, confidence;
		s64 V_raw_calc;

		// 计算三个维度分数
		vibrate = calculate_vibrate_score(type);
		renewal = calculate_renewal_score(type);
		confidence = calculate_confidence_score(type);

		// 保存到metrics结构
		m->vibrate_score = vibrate;
		m->renewal_score = renewal;
		m->confidence_score = confidence;

		// 综合分数（无负权重）
		V_raw_calc = ((s64)WEIGHT_VIBRATE * vibrate +
			      (s64)WEIGHT_RENEWAL * renewal +
			      (s64)WEIGHT_CONFIDENCE * confidence) /
			     ADAPTIVE_SCALE;
		m->V_raw = (s32)V_raw_calc;

		// 直接使用V_raw作为归一化分数（范围已在[0,10000]）
		m->V_normalized = min_t(u32, (u32)m->V_raw, ADAPTIVE_SCALE);

		trace_printk(
			"[Adaptive-Score] Event=%d vibrate=%u renewal=%u confidence=%u V_norm=%u\n",
			type, vibrate, renewal, confidence, m->V_normalized);
	}
}

/**
 * adaptive_metrics_init - 初始化自适应指标系统
 */
static void adaptive_metrics_init(void)
{
	enum event_type type;

	// 初始化堆统计
	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		global_heap_stats[type].total_samples = 0;
		global_heap_stats[type].heap_insertions = 0;
		global_heap_stats[type].heap_replacements = 0;
	}

	// 初始化自适应指标
	memset(global_adaptive_metrics, 0, sizeof(global_adaptive_metrics));

	// Bug Fix #6: 初始化内部Period跟踪，与pebs_init中__perf_event_open的初始值对齐
	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		if (type == EVENT_MEM_WRITE)
			tracked_periods[type] = htmm_inst_sample_period;
		else if (type == EVENT_DRAM_READ || type == EVENT_NVM_READ)
			tracked_periods[type] = INIT_PERIOD; /* F2-fix */
		else
			tracked_periods[type] = 0; /* 禁用的Event */
	}

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
 * get_current_period - 返回内部跟踪的当前Period
 * @type: Event类型
 * 
 * Bug Fix #6: 使用内部tracked_periods而非读取event->attr.sample_period。
 * perf subsystem的throttle机制可能篡改attr.sample_period，
 * 导致读到异常值（如300007、293）。
 * 内部跟踪确保返回的是我们上次主动设置的值。
 * 
 * 返回：当前Period，失败返回0
 */
static u64 get_current_period(enum event_type type)
{
	if (type >= EVENT_TYPE_MAX)
		return 0;
	return tracked_periods[type];
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

	/* Bug Fix #6: 更新内部跟踪 */
	if (type < EVENT_TYPE_MAX)
		tracked_periods[type] = new_period;

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

	// printk(KERN_INFO "[HTMM-Adaptive] Timer callback triggered\n");
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

			/* F9-fix: 老化因子从 ×0.7 改为 ×0.85
			 * ×0.7 衰减太快：hit=100 经12轮(120s)即降至1，热页难以维持。
			 * ×0.85：hit=100 经28轮(280s)降至1，热页存活所需采样频率减半。
			 * 效果：堆更稳定 → replacements↓ → renewal score↓ */
			for (heap_i = 0; heap_i < heap->size; heap_i++)
				heap->entries[heap_i].event_hit_count =
					heap->entries[heap_i].event_hit_count *
					85 / 100;

			// Floyd 建堆：从最后一个非叶子节点向上重建最小堆性质
			if (heap->size > 1) {
				for (heap_i = heap->size / 2; heap_i-- > 0;)
					heap_sift_down(heap, heap_i);
			}

			spin_unlock_irqrestore(&heap->lock, heap_flags);
		}
	}

	// 先计算指标（使用本窗口累积的统计数据）
	calculate_adaptive_metrics();

	// Bug Fix #8: 先保存总采样数，然后立即重置统计窗口
	// 原来budget超限时goto reschedule跳过了stats reset，导致统计跨窗口累积
	{
		u64 window_total_samples = 0;
		u64 cur_period, scaled;

		for (type = 0; type < EVENT_TYPE_MAX; type++)
			window_total_samples +=
				global_heap_stats[type].total_samples;

		// 立即重置统计窗口（无论是否超过预算）
		for (type = 0; type < EVENT_TYPE_MAX; type++) {
			global_heap_stats[type].total_samples = 0;
			global_heap_stats[type].heap_insertions = 0;
			global_heap_stats[type].heap_replacements = 0;
		}

		// 预算控制：如果总采样量超过预算，统一放大所有Event的Period
		if (window_total_samples > MAX_SAMPLES_PER_WINDOW) {
			for (type = 0; type < EVENT_TYPE_MAX; type++) {
				cur_period = get_current_period(type);
				if (cur_period == 0)
					continue; /* 跳过禁用的Event */
				scaled = cur_period * SCALE_UP_FACTOR /
					 SCALE_DOWN_FACTOR;
				scaled = min_t(u64, scaled, MAX_PERIOD);
				update_pebs_event_period(type, scaled);
			}
			// 预算超限，跳过本轮自适应调整
			goto reschedule;
		}
	}

	// 遍历所有Event类型
	for (type = 0; type < EVENT_TYPE_MAX; type++) {
		struct adaptive_metrics *metrics =
			&global_adaptive_metrics[type];
		u64 current_score, target_period, current_period, new_period;

		// 1. 读取当前分数
		current_score = metrics->V_normalized;

		// F1-fix: V_norm=0 时不再跳过，而是将 target 设为 MAX_PERIOD
		// EMA 平滑确保 period 逐步增大（每10秒窗口靠近30%），而非冻结
		if (current_score == 0) {
			target_period = MAX_PERIOD;
		} else {
			target_period = map_score_to_period((u32)current_score);
		}

		// 读取当前Period（0 = 禁用的Event，跳过）
		current_period = get_current_period(type);
		if (current_period == 0)
			continue;

		// EMA平滑调整
		new_period = apply_ema_to_period(current_period, target_period);

		// 更新硬件Period
		update_pebs_event_period(type, new_period);

		trace_printk(
			"[Adaptive-Period] Event=%d period=%llu->%llu target=%llu score=%llu\n",
			type, current_period, new_period, target_period,
			current_score);
	}

reschedule:
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
	/* Bug Fix #12: 必须先停止kthread，再pebs_disable。
	 * 原代码先调pebs_disable（禁用事件+销毁堆），kthread仍在运行，
	 * 导致kthread卡在内部do-while循环：ring buffer残留数据使cond持续
	 * 为true，而kthread_stop尚未被调用，循环永远无法退出。
	 * Baseline版本的顺序即为: kthread_stop → pebs_disable。 */
	if (access_sampling) {
		kthread_stop(access_sampling);
		access_sampling = NULL;
	}
	pebs_disable();
}
