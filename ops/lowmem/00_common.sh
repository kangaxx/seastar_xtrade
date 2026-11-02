#!/usr/bin/env bash
set -euo pipefail

SEASTAR_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${SEASTAR_ROOT}/ops/lowmem/logs"
mkdir -p "${LOG_DIR}"

step_title() {
  local title="$1"
  echo "========================================"
  echo "[STEP] ${title}"
  echo "[TIME] $(date '+%F %T')"
  echo "========================================"
}

step_info() {
  echo "[INFO] $*"
}

step_warn() {
  echo "[WARN] $*"
}

step_ok() {
  echo "[OK] $*"
}

require_root_hint() {
  if [ "$(id -u)" -ne 0 ]; then
    step_warn "当前不是 root 用户。若安装依赖失败，请改用 root 或 sudo 执行。"
  fi
}

run_and_log() {
  local name="$1"
  shift
  local log_file="${LOG_DIR}/${name}_$(date '+%Y%m%d_%H%M%S').log"
  step_info "执行命令：$*"
  step_info "日志文件：${log_file}"
  "$@" 2>&1 | tee "${log_file}"
}
