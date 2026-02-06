#!/bin/bash

# Adaptive-PEBS 验证实验: 方案1 - 最小有效集
# gapbs-pr: 稳态迭代负载（测试稳定场景下的优化）
# gapbs-bfs: 动态扩展负载（测试自适应Period调整能力）
BENCHMARKS="gapbs-pr gapbs-bfs"
NVM_RATIO="1:8"
sudo dmesg -c

for BENCH in ${BENCHMARKS};
do
    for NR in ${NVM_RATIO};
    do
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -V test
    done
done
