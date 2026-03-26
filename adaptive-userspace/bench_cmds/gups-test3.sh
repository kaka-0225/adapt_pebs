#!/bin/bash
# bench_cmd for gups-test3 (正式热度准确率对比实验)
# 前置条件：ghes.disable=1 已生效（已 update-grub + 重启）
# 参数：与 test2 相同（20线程 × 2B更新，45000MB，1:8 → DRAM=5000MB）
# 目的：在 PEBS 采样充足的条件下，比较 adaptive vs baseline 的热度识别效果

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test3/code

# 20 threads, 2000M updates/thread, 45000MB total, 8-byte elements
# Hot region = 20% × 45000MB = 9000MB
# DRAM = 5000MB (< hot region → 真实分层压力，需要热度识别才能最优)
BENCH_RUN="${BIN}/gups_test 20 2000000000 45000 8"
BENCH_DRAM=""

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="1300MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="2650MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="5000MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="9000MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="15000MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="22000MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="50000MB"
fi
