#!/bin/bash
# bench_cmd for gups-test4-r8 (test4: 单进程 GUPS, DRAM:NVM = 1:8)
# 单进程版本，消除多线程 inherit 问题，确保 adaptive/baseline 公平对比
# DRAM = 5625MB < 热区 9000MB → 有迁移压力，测热度识别+迁移效率

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test4/code

# 单进程, 60B updates, 45000MB total, 8-byte elements
# Hot region = 20% × 45000MB = 9000MB (exceeds DRAM → real tiering pressure)
# 60B updates ≈ 60-90 分钟（NVM 延迟），充足采样 + cooling 周期
BENCH_RUN="${BIN}/gups_test_sp 60000000000 45000 8"
BENCH_DRAM=""

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="1300MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="2650MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="5625MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="9000MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="15000MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="22500MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="50000MB"
fi
