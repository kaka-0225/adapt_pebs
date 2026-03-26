#!/bin/bash
# bench_cmd for gups-test7
# 合并 test5 + test6：支持 1:2、1:4、1:8 和 1:0(NoLimit)

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test4/code

# 60B updates, 45000MB total, 8-byte elements
BENCH_RUN="${BIN}/gups_test_sp 60000000000 45000 8"
BENCH_DRAM=""

case "${NVM_RATIO}" in
    "1:2")
        BENCH_DRAM="15000MB"
        ;;
    "1:4")
        BENCH_DRAM="9000MB"
        ;;
    "1:8")
        BENCH_DRAM="5000MB"
        ;;
    "1:0")
        BENCH_DRAM="76000MB"
        ;;
    *)
        echo "ERROR: gups-test7 不支持比例 ${NVM_RATIO}（仅支持 1:2, 1:4, 1:8, 1:0）"
        exit 1
        ;;
esac
