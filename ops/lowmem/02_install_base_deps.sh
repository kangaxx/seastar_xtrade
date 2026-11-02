#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=00_common.sh
source "${SCRIPT_DIR}/00_common.sh"

step_title "02 安装基础依赖（低风险增量）"
step_info "目的：补齐已知缺失的编译依赖，避免后续编译中断。"
require_root_hint

run_and_log "02_install_base_deps" bash -c '
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    python3 \
    python3-pyelftools \
    python3-yaml \
    ragel \
    protobuf-compiler \
    libprotobuf-dev \
    libc-ares-dev \
    libfmt-dev \
    libyaml-cpp-dev \
    liblz4-dev \
    libhwloc-dev \
    libnuma-dev \
    liburing-dev \
    libsctp-dev \
    libssl-dev \
    libxml2-dev \
    libgnutls28-dev \
    libpciaccess-dev \
    systemtap-sdt-dev \
    valgrind \
    doxygen \
    xfslibs-dev
'

step_ok "02 完成：基础依赖安装结束。"
