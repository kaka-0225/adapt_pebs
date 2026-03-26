#!/bin/bash

######## Quick test config for development
BIN=/mnt/sas_ssd/benchmarks/gapbs
GRAPH_DIR=/mnt/sas_ssd/benchmarks/gapbs/benchmark/graphs

# Medium-sized graph (kron25: ~33M nodes) with 200 trials for 5-10min runtime
BENCH_RUN="${BIN}/pr -f ${GRAPH_DIR}/kron25.sg -i1000 -t1e-4 -n200"
BENCH_DRAM=""


# NOTE: this config uses kron25 (33M nodes, ~4GB working set) for development testing.
# 200 trials × ~0.74s + HTMM overhead ≈ 5-10 minutes total runtime.

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
