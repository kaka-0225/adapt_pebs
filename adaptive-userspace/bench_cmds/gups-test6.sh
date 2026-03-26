#!/bin/bash
# bench_cmd for gups-test6 (无迁移压力：DRAM=76GB >> 工作负载45GB)
# 目的：隔离 HTMM 采样/cooling/打分的纯开销

BIN=/mnt/sas_ssd/lyh/memtis-Nomad/memtis-userspace/experiment_results/GUPS_Test/test4/code

# 60B updates, 45000MB total, 8-byte elements
BENCH_RUN="${BIN}/gups_test_sp 60000000000 45000 8"

# DRAM 配额 76000MB — 远大于 45GB 工作负载，不触发迁移
# 同时明显低于历史 HTMM 内核启动后 Node0 可用空闲内存，避免 run_bench.sh 的 MemFree 检查直接 abort
BENCH_DRAM="76000MB"
