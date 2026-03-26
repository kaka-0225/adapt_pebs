# Adaptive-PEBS 实验框架

## 目录结构

```
adaptive-userspace/
├── scripts/           # 核心脚本
├── bench_cmds/        # Benchmark配置
├── experiments/       # 实验脚本
│   ├── quick_compare.sh
│   └── pr_compare.sh
├── results/           # 实验结果
└── bin/               # 工具
```

## 快速开始

### 1. 轻量化测试（GUPS 7B）

```bash
cd /mnt/sas_ssd/lyh/memtis-Nomad/adaptive-userspace
sudo bash experiments/quick_compare.sh
```

### 2. PR 专项测试

```bash
sudo bash experiments/pr_compare.sh
```

## 优化目标

| 指标 | 优化前 | 目标 | Baseline |
|------|--------|------|----------|
| PR时间 | 6897s | <2000s | 461s |
| 采样数 | 5.4M | <2M | 1M |
| Promote | 131K | <10K | 18 |
