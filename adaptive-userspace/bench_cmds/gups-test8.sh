#!/bin/bash
# bench_cmd for gups-test8
# 使用新的 GUPS (带页面放置监控)，其余与 test7 结构相同

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test8/code

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
        echo "ERROR: unsupported NVM_RATIO=${NVM_RATIO}" >&2
        exit 1
        ;;
esac
