#!/bin/bash

######## BFS Benchmark for Adaptive-PEBS Testing
BIN=/mnt/sas_ssd/benchmarks/gapbs
GRAPH_DIR=/mnt/sas_ssd/benchmarks/gapbs/benchmark/graphs

# kron25: ~33M nodes, ~4GB working set
# -n64: 64 trials for statistical significance
# BFS has dynamic access patterns (good for testing adaptive Period)
BENCH_RUN="${BIN}/bfs -f ${GRAPH_DIR}/kron25.sg -n64"
BENCH_DRAM=""

# Memory tier configuration based on NVM ratio
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

export BENCH_RUN
export BENCH_DRAM
