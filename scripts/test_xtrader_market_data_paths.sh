#!/usr/bin/env bash
# Standalone path-class checks; no reactor, broker, existing regressions or /data I/O.
# Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX="${CXX:-g++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    printf '[FAIL] C++ compiler not found: %s. Set CXX to a C++20 compiler.\n' "$CXX" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d /tmp/xtrader-market-data-paths.XXXXXXXX)"
BINARY="$WORK_DIR/xtrader_market_data_paths_test"
cleanup() {
    local status=$?
    trap - EXIT
    if ! rm -f -- "$BINARY"; then
        printf '[WARN] Could not remove test binary: %s\n' "$BINARY" >&2
    fi
    if ! rmdir -- "$WORK_DIR"; then
        printf '[WARN] Temporary evidence retained in: %s\n' "$WORK_DIR" >&2
    fi
    exit "$status"
}
trap cleanup EXIT

printf '[INFO] Standalone market_data_paths validation; production data is not accessed.\n'
printf '[INFO] Repository: %s\n' "$REPO_ROOT"
printf '[INFO] Compiler: %s\n' "$CXX"
"$CXX" --version
printf '[INFO] Compiling only the path class and its independent test.\n'
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
    -I "$REPO_ROOT/include" \
    "$REPO_ROOT/src/xtrader/market_data_paths.cc" \
    "$REPO_ROOT/tests/unit/xtrader_market_data_paths_test.cc" \
    -o "$BINARY"
printf '[OK] Standalone compilation completed.\n'

# Fixtures stay inside the disposable workspace, independent of caller TMPDIR.
printf '[INFO] Running layout, parsing, invalid-input and filesystem-boundary cases.\n'
TMPDIR="$WORK_DIR" "$BINARY"
printf '[OK] market_data_paths compilation and standalone cases passed.\n'
printf '[INFO] Existing readers/writers remain unchanged; no trading verification performed.\n'
