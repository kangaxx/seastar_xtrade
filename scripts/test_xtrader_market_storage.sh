#!/usr/bin/env bash
# Shared storage readback; no real CTP library, network or production /data.
# Source: user request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
set -euo pipefail
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX="${CXX:-g++}"
WORK_DIR="$(mktemp -d /tmp/xtrader-market-storage.XXXXXXXX)"
BINARY="$WORK_DIR/xtrader_market_storage_test"
cleanup() {
    local status=$?
    trap - EXIT
    rm -f -- "$BINARY"
    rmdir -- "$WORK_DIR"
    exit "$status"
}
trap cleanup EXIT
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
    -I "$REPO_ROOT/include" -I "$REPO_ROOT/src/xtrader" \
    -isystem "$REPO_ROOT/api/ctp/v6.7.2/linux64" \
    "$REPO_ROOT/src/xtrader/market_data_paths.cc" \
    "$REPO_ROOT/src/xtrader/market_data_writer.cc" \
    "$REPO_ROOT/src/xtrader/market_tick_writer.cc" \
    "$REPO_ROOT/src/xtrader/data_importer.cc" \
    "$REPO_ROOT/src/xtrader/historical_data_manager.cc" \
    "$REPO_ROOT/tests/unit/xtrader_market_storage_test.cc" \
    -lcrypto -o "$BINARY"
TMPDIR="$WORK_DIR" "$BINARY"
printf '[OK] Shared market CSV writer/import/readback cases passed.\n'
