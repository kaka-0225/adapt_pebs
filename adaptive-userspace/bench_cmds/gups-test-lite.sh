#!/bin/bash
# bench_cmd for gups-test-lite (轻量化验证负载)
# 目的：验证 ghes.disable=1 + max_sample_rate 重置后 PEBS 采样量是否恢复正常
# 前置条件：已重启到目标内核（ghes.disable=1 生效）
#
# 参数：4 线程 × 2亿次更新，4500MB 总内存，1:8 比例 → DRAM=562MB
# 热区 = 20% × 4500MB = 900MB > DRAM=562MB（有真实分层压力）
# 预期运行时间：~3-8 分钟（对比 test2 的 28 分钟/103 分钟）

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test_lite/code

# 4 threads, 200M updates/thread, 4500MB total, 8-byte elements
BENCH_RUN="${BIN}/gups_test 4 200000000 4500 8"
BENCH_DRAM=""

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="140MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="280MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="562MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="900MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="1500MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="2200MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="5000MB"
fi
