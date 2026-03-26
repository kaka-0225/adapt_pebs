# Adaptive-PEBS

**基于采样信息价值的自适应 PEBS 采样策略，用于 DRAM-NVM 分层内存管理**

> 基于 [Memtis](https://github.com/casys-kaist/memtis) (SOSP 2023, Linux 5.15.19) 内核扩展

---

## 项目简介

Adaptive-PEBS 为分层内存系统（DRAM + NVM/CXL）提供自适应的 PEBS 采样策略。在 Memtis 固定采样周期的基础上，通过量化每种 PEBS Event 的**采样信息价值**，动态调整各 Event 的采样 Period，实现按需采样。

**核心观察**：不同 PEBS Event 采样到的地址分布特征存在显著差异，且随负载和运行阶段动态变化。固定 Period 无法适应这种变化——对信息已饱和的 Event 过度采样浪费 CPU，对仍在发现新热页的 Event 采样不足导致温页识别延迟。

## 方法

### 三维评分公式

每 10 秒为每种 Event 独立计算采样价值分数：

$$V_i = 0.3 \times V_{vibrate} + 0.4 \times V_{renewal} + 0.3 \times V_{confidence}$$

| 维度 | 权重 | 含义 | 数据来源 |
|------|------|------|---------|
| **V_vibrate** | 30% | 采样地址的访问时间间隔波动性 | Welford 在线方差 |
| **V_renewal** | 40% | Event 堆的更新率（新页发现速率） | 堆操作统计 |
| **V_confidence** | 30% | 热页确认的充分程度（对数饱和） | ilog2(avg_hit) |

分数通过反向线性映射转换为 Period，再经 EMA 平滑后更新硬件：

$$Period = P_{max} - V_{norm} \times (P_{max} - P_{min}) / 10000$$

### 安全机制

- **全局采样预算**：总采样超过 200 万/10s 时，所有 Period ×1.5
- **CPU 硬限制**：ksamplingd CPU > 5% 时强制增大 Period
- **安全启动**：从 MAX_PERIOD 开始，向下调节
- **Promote 速率限制**：每轮最多 5000 页，防止迁移风暴

## 系统架构

```
应用负载 → PEBS 硬件 (9种 Event, 独立 Period)
              ↓
         ksamplingd (ring buffer 消费)
         ├─ Welford 方差 (per-page)
         ├─ Event 堆 (Top-1000 per-event)
         └─ 堆统计 (insertions/replacements/total)
              ↓ 每10秒
         Adaptive 评分引擎
         ├─ V_vibrate + V_renewal + V_confidence → V_norm
         ├─ V_norm → target Period → EMA 平滑
         └─ 全局采样预算检查
              ↓
         更新 per-Event 硬件 Period
```

## 代码结构

```
linux/
├── arch/x86/include/asm/pgtable_types.h  # pginfo_t 扩展 (Welford 字段)
├── include/linux/htmm.h                  # Event 枚举 (9种)、函数声明
├── include/linux/memcontrol.h            # mem_cgroup 扩展 (promote 计数)
└── mm/
    ├── htmm_sampler.c    # 核心: 堆、Welford、评分、Period 自适应
    ├── htmm_core.c       # 采样入口、Cooling 集成
    └── htmm_migrater.c   # Promote 速率限制、动态阈值
```

## 编译与运行

### 环境要求

- Linux 5.15.19 内核源码 (基于 Memtis 修改)
- Intel CPU (支持 PEBS)
- DRAM + NVM 双层内存拓扑 (NUMA 节点)

### 编译安装

```bash
cd linux/
make -j$(nproc)
sudo make modules_install && sudo make install
```

### 切换内核

```bash
# 切换到 Adaptive-PEBS 内核
sudo bash htmm_adaptive.sh

# 切换到 Baseline (原版 Memtis) 内核
sudo bash htmm_baseline.sh

# 启动后配置 NVM 节点
sudo bash start.sh
```

### 运行实验

```bash
cd adaptive-userspace/

# 运行单个 benchmark
sudo bash scripts/run_bench.sh -B gups-quick -R 1:2 -V exp_name

# 运行完整对比实验 (adaptive + baseline, 自动 kexec 切换)
sudo bash experiments/20260325_optimize_v1/optimize_compare.sh
```

## 实验结果 (build #78)

| Benchmark | Ratio | Adaptive | Baseline (Memtis) | 变化 |
|-----------|-------|----------|-------------------|------|
| gapbs-pr | 1:2 | **1.548s**/trial | 2.206s/trial | **+29.8%** |
| gapbs-pr | 1:4 | **1.560s**/trial | 2.469s/trial | **+36.8%** |
| gups-quick | 1:2 | 0.00816 GUPS | **0.01025** GUPS | -20.4% |
| gups-quick | 1:4 | 0.00760 GUPS | **0.00858** GUPS | -11.5% |

- **gapbs-pr (PageRank)**：Adaptive 减少了 99.9% 的无效页迁移 (31 vs 2000万次)，性能提升 30-37%
- **gups-quick (随机访问)**：保守的采样策略降低了热页识别速度，性能损失 11-20%

## 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| MIN_PERIOD | 199 | 最高采样频率 |
| MAX_PERIOD | 19997 | 最低采样频率 |
| 评分更新间隔 | 10s | delayed_work 周期 |
| EMA α | 0.3 | Period 平滑系数 |
| 全局采样预算 | 200万/10s | 超出则 Period ×1.5 |
| Promote 上限 | 5000/轮 | 防止迁移风暴 |
| 堆容量 | 1000/Event | Top-K 热页追踪 |

## 参考文献

1. Memtis: Efficient Memory Tiering with Dynamic Page Classification, SOSP 2023
2. Intel 64 and IA-32 Architectures SDM, Volume 3B, Chapter 18 (PEBS)
3. Welford, B.P. (1962). "Note on a method for calculating corrected sums of squares and products"

---

**内核版本**: 5.15.19-htmm-adaptive (build #78)
**最后更新**: 2026-03-26
