#!/usr/bin/env bash
# Offline fake-SDK full-chain checks; never connects CTP or touches production /data.
# Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX="${CXX:-g++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    printf '[ERROR] C++20 compiler missing: %s\n' "$CXX" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d /tmp/xtrader-market-capture.XXXXXXXX)"
BINARY="$WORK_DIR/xtrader_market_capture_test"
cleanup() {
    local status=$?
    trap - EXIT
    if ! rm -f -- "$BINARY"; then
        printf '[WARN] Cannot remove test binary: %s\n' "$BINARY" >&2
    fi
    if ! rmdir -- "$WORK_DIR"; then
        printf '[WARN] Temporary evidence retained: %s\n' "$WORK_DIR" >&2
    fi
    exit "$status"
}
trap cleanup EXIT

printf '[INFO] Compiling offline capture cases with OpenSSL; real CTP libraries are NOT linked.\n'
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic -pthread \
    -I "$REPO_ROOT/include" -I "$REPO_ROOT/apps/xtrader" -I "$REPO_ROOT/src/xtrader" \
    -isystem "$REPO_ROOT/api/ctp/v6.7.2/linux64" \
    "$REPO_ROOT/src/xtrader/market_data_paths.cc" \
    "$REPO_ROOT/src/xtrader/market_capture_config.cc" \
    "$REPO_ROOT/src/xtrader/market_tick_writer.cc" \
    "$REPO_ROOT/src/xtrader/market_capture.cc" \
    "$REPO_ROOT/tests/unit/xtrader_market_capture_test.cc" \
    -lcrypto -o "$BINARY"
TMPDIR="$WORK_DIR" "$BINARY"
printf '[OK] Offline full-chain cases passed; live front/account/network remain unverified.\n'
