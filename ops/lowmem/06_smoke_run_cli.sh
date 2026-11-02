#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "06 最小调试验证（菜单链路）"
step_info "目的：验证 xtrader_cli 可启动，并能进入 Historical Redis 菜单再退出。"

run_and_log "06_smoke_run_cli" bash -c '
  cd /root/seastar
  printf "d\n3\nu\nq\n" | ./build/debug/apps/xtrader/xtrader_cli
'

step_ok "06 完成：最小调试验证结束。"
