#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "05 单线程低内存编译 xtrader_cli"
step_info "目的：在 2G 内存机器上以最稳妥方式编译。"
step_info "策略：JOBS=1 串行编译，必要时保留已有对象增量重试。"

JOBS="${JOBS:-1}"
if [ "${JOBS}" -gt 2 ]; then
  step_warn "JOBS=${JOBS} 对 2G 机器风险较高，建议 JOBS=1"
fi

run_and_log "05_build_debug_lowmem" bash -c '
  cd /root/seastar
  ninja -C build/debug -j"'"${JOBS}"'" app_xtrader_cli
'

step_ok "05 完成：低内存编译结束。"
