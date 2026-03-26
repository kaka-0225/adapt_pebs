#!/bin/bash

######## CC (Connected Components) Benchmark
BIN=/mnt/sas_ssd/benchmarks/gapbs
GRAPH_DIR=/mnt/sas_ssd/benchmarks/gapbs/benchmark/graphs

# kron25: ~33M nodes, ~4GB working set
# -n64: 64 trials
# CC is write-intensive (label updates), good for testing MEMWRITE Event adaptation
BENCH_RUN="${BIN}/cc -f ${GRAPH_DIR}/kron25.sg -n64"
BENCH_DRAM=""

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
