#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "01 基础环境探测（只读）"
step_info "目的：确认系统、CPU、内存、工具链、关键依赖包与头文件状态。"

run_and_log "01_probe_env" bash -c '
  echo "--- 系统信息 ---"
  whoami
  hostname
  uname -a

  echo "--- CPU/内存 ---"
  nproc || true
  free -h || true

  echo "--- 工具链版本 ---"
  gcc --version | head -n 1 || true
  g++ --version | head -n 1 || true
  cmake --version | head -n 1 || true
  ninja --version || true

  echo "--- C/C++ ABI 关键项 ---"
  ldd --version | head -n 1 || true
  strings /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep -E "^GLIBCXX_[0-9]+(\\.[0-9]+)*$" | sort -V | tail -n 1 || true
  strings /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep -E "^CXXABI_[0-9]+(\\.[0-9]+)*$" | sort -V | tail -n 1 || true

  echo "--- seastar 目录确认 ---"
  test -d /root/seastar && echo "/root/seastar exists" || echo "/root/seastar missing"

  echo "--- 关键依赖包状态(可空) ---"
  dpkg -s xfslibs-dev >/dev/null 2>&1 && echo "xfslibs-dev installed" || echo "xfslibs-dev missing"
  dpkg -s libfmt-dev >/dev/null 2>&1 && echo "libfmt-dev installed" || echo "libfmt-dev missing"
  dpkg -s doxygen >/dev/null 2>&1 && echo "doxygen installed" || echo "doxygen missing"

  echo "--- 关键头文件 ---"
  test -f /usr/include/xfs/linux.h && echo "/usr/include/xfs/linux.h exists" || echo "/usr/include/xfs/linux.h missing"
  test -f /usr/local/include/boost/version.hpp && echo "/usr/local/include/boost/version.hpp exists" || echo "/usr/local/include/boost/version.hpp missing"
  test -f /usr/local/include/fmt/ostream.h && echo "/usr/local/include/fmt/ostream.h exists" || echo "/usr/local/include/fmt/ostream.h missing"
'

step_ok "01 完成：环境探测结束。请查看日志中的 missing 项。"
