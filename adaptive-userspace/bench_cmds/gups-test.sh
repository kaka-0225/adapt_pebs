#!/bin/bash
# bench_cmd for gups-test, compatible with run_bench.sh framework

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test1/code

# 20 threads, 200M updates/thread, 4500MB total, 8-byte elements
BENCH_RUN="${BIN}/gups_test 20 200000000 4500 8"
BENCH_DRAM=""

# Total memory ~4500MB, hot region = 20% = 900MB
# 1:8 ratio → DRAM = 500MB → hot region mostly in DRAM
if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="130MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="260MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="500MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="950MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="1800MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="3400MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="12000MB"
fi
