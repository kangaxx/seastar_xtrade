#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "03 验证 Boost/FMT 就绪情况"
step_info "目的：确认 /usr/local 下搬运结果可用，避免 CMake 走错版本。"

run_and_log "03_verify_boost_fmt" bash -c '
  echo "--- Boost ---"
  if [ -f /usr/local/include/boost/version.hpp ]; then
    grep -n BOOST_LIB_VERSION /usr/local/include/boost/version.hpp | head -n 1
  else
    echo "missing: /usr/local/include/boost/version.hpp"
  fi

  echo "--- FMT ---"
  if [ -f /usr/local/include/fmt/core.h ]; then
    ls -l /usr/local/include/fmt/core.h
  else
    echo "missing: /usr/local/include/fmt/core.h"
  fi

  if [ -f /usr/local/include/fmt/ostream.h ]; then
    ls -l /usr/local/include/fmt/ostream.h
  else
    echo "missing: /usr/local/include/fmt/ostream.h"
  fi

  echo "--- lib 文件 ---"
  ls -l /usr/local/lib/libboost_* 2>/dev/null | head -n 20 || true
  ls -l /usr/local/lib/libfmt* 2>/dev/null || true

  echo "--- ldconfig ---"
  ldconfig
  ldconfig -p | grep -E "libboost_|libfmt" | head -n 30 || true
'

step_ok "03 完成：Boost/FMT 验证结束。"
