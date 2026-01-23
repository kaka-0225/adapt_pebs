# Adaptive-PEBS: 基于物理地址驱动与不确定性感知的自适应内存分层系统

## 📖 项目简介

**Adaptive-PEBS** 是一个针对分层内存系统 (Tiered Memory System, 如 DRAM + CXL/PMem) 设计的高效、精准的页面热度感知与迁移系统。

本项目旨在解决传统基于硬件事件采样 (PEBS) 在大内存场景下面临的采样倾斜、开销过高以及自适应性不足等问题。我们通过**纯物理地址驱动**和**基于采样收益的动态闭环控制**，实现了在极低 CPU 开销下对“温数据”的高敏锐捕获。

> **注：** 本项目基于 [Memtis](https://github.com/...) 内核模块进行二次开发与重构。

---

## ⚠️ 现有技术的痛点 (Motivation)

![tiered memory system architecture](./images/architecture.png)
*(注：建议在此处插入分层内存架构图)*

在现代数据中心负载中，现有的热度感知技术（以 AutoNUMA, TPP, 原版 Memtis 为代表）存在以下四个核心缺陷：

* **感知盲区（采集倾斜）：** 物理地址空间的访问呈现严重的“马太效应”。Ring Buffer 容易被少数极热页面填满，导致系统忽略了正在变热的大规模“温数据”。
* **死板的静态配置：** 现有的静态采样周期 (Period) 和允许滑移的配置 (`precise_ip=1`) 无法适应动态变化的负载，且在密集指令流中会导致归因地址偏离。
* **虚拟地址转换开销极高：** 默认基于虚拟地址的采样，迫使内核频繁执行昂贵的页表遍历，加剧 TLB Miss，与业务主线程争抢资源。
* **热度统计偏差：** 传统的“采样命中即 +1”的方式，忽略了动态采样周期的权重，导致高频采样下的冷页得分可能超过低频采样下的热页。

---

## 🚀 核心特性与技术突破 (Core Innovations)

### 1. 零开销的物理地址驱动 (Physical-Index Driven)

* **硬件零滑移：** 强制开启硬件 `precise_ip=2` (PEBS Assist) 和 `PERF_SAMPLE_PHYS_ADDR`，确保记录的物理地址与触发事件的指令 100% 对应。
* **O(1) 线性索引：** 利用 Linux 内核 `vmemmap` 线性映射机制，直接使用物理页帧号 (PFN) 寻址 `struct page`，彻底消除了页表遍历开销。

### 2. 基于 "采样收益" 的全局资源调度 (ROI-based Adaptive Control)

系统将 PMU 中断配额视为稀缺资源，基于多维收益评估动态分配采样周期：

* **空间维度 (离散度 $S_{disp}$)：** 奖励空间覆盖率高的发散型事件（如 LLC_MISS），惩罚陷入局部死循环的密集型事件（如 LOAD）。
* **时间维度 (抖动度 $\tilde{S}_{Vibrate}$)：** 移植 TCP 的 Jacobson/Karels 算法，量化页面的抖动强度。**核心洞察**：高抖动页面意味着数据正在变热（非稳态），系统自动向其倾斜采样资源，加速温数据的捕获。
* **综合收益模型：** $$V(E_{i})=\alpha\cdot S_{disp}(E_{i})+\beta\cdot\tilde{S}_{Vibrate}(E_{i})+\tilde{S}_{hotness}(E_{i})*c+\tilde{S}_{overhead}(E_{i})$$

### 3. 紧凑型无锁元数据 (Compact Lock-free Metadata)

利用 Linux 的 `page_ext` 机制，将页面状态高度压缩进 64-bit 整数中，单页仅增加 8 字节开销。结合 CAS 原语，实现了 NMI 上下文安全的无锁 (Lock-free) 并发更新。

* **64-bit 布局：** `[Fluctuation (8b) | Interval (8b) | Last_Hit (16b) | Reserved (12b) | Hit_Count (20b)]`。

### 4. 统计修正的归一化迁移 (Normalized Migration)

在页面迁移扫描时，执行读时归一化，消除动态采样频率带来的偏差，还原真实的物理访问强度。

* **修正公式：** $Real\_Hotness = Hit\_Count \times Period_{current}$
* **冷却机制：** 后台线程周期性执行右移位移（指数衰减）操作，清除历史热度噪声。

---

## ⚙️ 系统架构 (Architecture)
