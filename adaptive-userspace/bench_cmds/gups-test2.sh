#!/bin/bash
# bench_cmd for gups-test2 (scaled-up workload), compatible with run_bench.sh

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test2/code

# 20 threads, 2000M updates/thread, 45000MB total, 8-byte elements
# Hot region = 20% × 45000MB = 9000MB
# Expected baseline runtime ~100s (10× more updates than test1)
BENCH_RUN="${BIN}/gups_test 20 2000000000 45000 8"
BENCH_DRAM=""

# 1:8 ratio → DRAM = 45000/9 = 5000MB
# Hot region 9000MB > DRAM 5000MB → genuine tiered memory pressure
# Promotion watermark: 5000MB×3% = 150MB headroom (vs 50MB in test1)
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
