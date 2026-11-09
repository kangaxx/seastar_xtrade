#!/usr/bin/env bash
# Phase 6 standalone metrics regression/probe build; no trading connections.
# Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
set -Eeuo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/phase6"
CXX="${CXX:-g++}"
if ! command -v "${CXX}" >/dev/null 2>&1; then
    echo "[ERROR] C++20 compiler not found: ${CXX}" >&2
    exit 2
fi
mkdir -p "${BUILD_DIR}"
FLAGS=(-std=c++20 -O2 -Wall -Wextra -Werror -pedantic -pthread -I"${ROOT_DIR}/include")
SOURCES=("${ROOT_DIR}/src/xtrader/performance_metrics.cc" "${ROOT_DIR}/src/xtrader/health_check.cc")
"${CXX}" "${FLAGS[@]}" "${ROOT_DIR}/tests/unit/xtrader_phase6_metrics_test.cc" \
    "${SOURCES[@]}" -o "${BUILD_DIR}/xtrader_phase6_metrics_test"
"${BUILD_DIR}/xtrader_phase6_metrics_test"
"${CXX}" "${FLAGS[@]}" "${ROOT_DIR}/tools/xtrader_phase6_probe.cc" \
    "${SOURCES[@]}" -o "${BUILD_DIR}/xtrader_phase6_probe"
echo "[OK] Phase 6 offline regression and probe build completed."
