#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "04 低内存模式 CMake 配置"
step_info "目的：先只做 configure，快速暴露缺失依赖，不进入大规模编译。"

run_and_log "04_configure_debug" bash -c '
  cd /root/seastar
  cmake -S . -B build/debug -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=/usr/local
'

step_ok "04 完成：CMake 配置结束。"
