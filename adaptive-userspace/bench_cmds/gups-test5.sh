#!/bin/bash
# bench_cmd for gups-test5 (修复版单进程 GUPS)
# 修复 test4 bug: 纯 Node 0 CPU, SRC 隔离, 一致的 DRAM:NVM 比例
# 支持 1:2, 1:4, 1:8 三种比例

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test4/code

# 60B updates, 45000MB total, 8-byte elements
BENCH_RUN="${BIN}/gups_test_sp 60000000000 45000 8"
BENCH_DRAM=""

# DRAM:NVM = 1:N → DRAM = 45000 / (1+N)
if [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="15000MB"    # 45000/(1+2) — DRAM > 热区 9GB
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="9000MB"     # 45000/(1+4) — DRAM ≈ 热区 9GB (边界)
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="5000MB"     # 45000/(1+8) — DRAM < 热区 9GB
else
    echo "ERROR: gups-test5 不支持比例 ${NVM_RATIO}（仅支持 1:2, 1:4, 1:8）"
    exit 1
fi
