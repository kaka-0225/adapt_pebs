#!/usr/bin/env bash
# optimize_compare 的 kexec 恢复入口
# systemd autoresume 服务调用此脚本

set -euo pipefail

BASE="/mnt/sas_ssd/lyh/memtis-Nomad"
USERSPACE="${BASE}/adaptive-userspace"
EXP_DIR="${USERSPACE}/experiments/20260325_optimize_v1"
STATE_FILE="${EXP_DIR}/.state"
LOG_FILE="${EXP_DIR}/run.log"
START_SCRIPT="${BASE}/start.sh"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"; }

# 检查是否有未完成的实验
if [[ ! -f "${STATE_FILE}" ]]; then
    log "[resume] No .state file, nothing to resume."
    exit 0
fi

STATE=$(cat "${STATE_FILE}")
log "[resume] Resuming from state: ${STATE}"

if [[ "${STATE}" == "done" ]]; then
    log "[resume] Experiment already done."
    rm -f "${STATE_FILE}"
    exit 0
fi

# 等待系统稳定（网络、文件系统挂载等）
sleep 10

# 执行主脚本，它会读取 .state 继续
exec bash "${EXP_DIR}/optimize_compare.sh"
