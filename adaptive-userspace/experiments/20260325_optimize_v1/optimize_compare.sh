#!/usr/bin/env bash
# Adaptive-PEBS 优化验证实验 v1
# gups-quick + gapbs-pr, 1:2 + 1:4, adaptive vs baseline
# 状态机 + kexec 自动切换内核

set -euo pipefail

BASE="/mnt/sas_ssd/lyh/memtis-Nomad"
USERSPACE="${BASE}/adaptive-userspace"
EXP_DIR="${USERSPACE}/experiments/20260325_optimize_v1"
STATE_FILE="${EXP_DIR}/.state"
LOG_FILE="${EXP_DIR}/run.log"
EXP_NAME="20260325_optimize_v1"

BENCH_SCRIPT="${USERSPACE}/scripts/run_bench.sh"
START_SCRIPT="${BASE}/start.sh"
KEXEC_ADAPTIVE="${BASE}/htmm_adaptive.sh"
KEXEC_BASELINE="${BASE}/htmm_baseline.sh"

TRACE_FILE="/sys/kernel/debug/tracing/trace"

mkdir -p "${EXP_DIR}"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"; }
die() { log "FATAL: $*"; exit 1; }

get_state() {
    [[ -f "${STATE_FILE}" ]] && cat "${STATE_FILE}" || echo "init"
}

set_state() {
    echo "$1" > "${STATE_FILE}"
    log "State -> $1"
}

check_kernel() {
    local expect="$1"
    local current
    current=$(uname -r)
    log "Kernel: ${current} (expect: ${expect})"
    if [[ ! "${current}" =~ "${expect}" ]]; then
        die "Kernel mismatch: expect ${expect}, got ${current}"
    fi
}

# 收集 adaptive trace (trace_printk 输出)
collect_trace() {
    local dest="$1"
    if [[ -f "${TRACE_FILE}" ]]; then
        cat "${TRACE_FILE}" > "${dest}" 2>/dev/null || true
        echo > "${TRACE_FILE}" 2>/dev/null || true
        log "Trace saved -> ${dest}"
    fi
}

run_one() {
    local kernel="$1"    # adaptive / baseline
    local bench="$2"     # gups-quick / gapbs-pr
    local ratio="$3"     # 1:2 / 1:4
    local result_dir="${USERSPACE}/results/${bench}/${EXP_NAME}/${kernel}/${ratio}"

    log "=========================================="
    log "RUN: ${kernel} / ${bench} / ${ratio}"
    log "=========================================="

    # 清空 trace buffer
    if [[ "${kernel}" == "adaptive" ]]; then
        echo > "${TRACE_FILE}" 2>/dev/null || true
    fi

    cd "${USERSPACE}"
    sudo bash "${BENCH_SCRIPT}" -B "${bench}" -R "${ratio}" -V "${EXP_NAME}/${kernel}" 2>&1 | tee -a "${LOG_FILE}"

    # 收集 adaptive trace
    if [[ "${kernel}" == "adaptive" ]]; then
        mkdir -p "${result_dir}"
        collect_trace "${result_dir}/adaptive_trace.txt"
    fi

    log "DONE: ${kernel} / ${bench} / ${ratio}"
}

# ========== 状态机 ==========
STATE=$(get_state)
log "optimize_compare start, state=${STATE}"

case "${STATE}" in

    "init")
        log "Step: kexec -> adaptive"
        set_state "kexec_adaptive"
        sudo bash "${KEXEC_ADAPTIVE}"
        ;;

    "kexec_adaptive")
        check_kernel "adaptive"
        log "Running start.sh"
        sudo bash "${START_SCRIPT}"
        sleep 5

        run_one adaptive gups-quick 1:2
        set_state "adaptive:gups-quick:1:4"
        # 继续执行下一步（不 kexec）
        ;&

    "adaptive:gups-quick:1:4")
        check_kernel "adaptive"
        run_one adaptive gups-quick 1:4
        set_state "adaptive:gapbs-pr:1:2"
        ;&

    "adaptive:gapbs-pr:1:2")
        check_kernel "adaptive"
        run_one adaptive gapbs-pr 1:2
        set_state "adaptive:gapbs-pr:1:4"
        ;&

    "adaptive:gapbs-pr:1:4")
        check_kernel "adaptive"
        run_one adaptive gapbs-pr 1:4

        log "Step: kexec -> baseline"
        set_state "kexec_baseline"
        sudo bash "${KEXEC_BASELINE}"
        ;;

    "kexec_baseline")
        check_kernel "baseline"
        log "Running start.sh"
        sudo bash "${START_SCRIPT}"
        sleep 5

        run_one baseline gups-quick 1:2
        set_state "baseline:gups-quick:1:4"
        ;&

    "baseline:gups-quick:1:4")
        check_kernel "baseline"
        run_one baseline gups-quick 1:4
        set_state "baseline:gapbs-pr:1:2"
        ;&

    "baseline:gapbs-pr:1:2")
        check_kernel "baseline"
        run_one baseline gapbs-pr 1:2
        set_state "baseline:gapbs-pr:1:4"
        ;&

    "baseline:gapbs-pr:1:4")
        check_kernel "baseline"
        run_one baseline gapbs-pr 1:4

        log "=========================================="
        log "ALL EXPERIMENTS COMPLETE"
        log "=========================================="
        set_state "done"
        ;;

    "done")
        log "Experiment already completed."
        ;;

    *)
        die "Unknown state: ${STATE}"
        ;;
esac
