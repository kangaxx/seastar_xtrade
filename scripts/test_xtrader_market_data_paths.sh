#!/usr/bin/env bash
# Standalone path and menu checks; no reactor, broker or /data I/O.
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
MENU_BINARY="$WORK_DIR/xtrader_cli_data_sources_test"
BACKTEST_BINARY="$WORK_DIR/xtrader_backtest_menu_test"
cleanup() {
    local status=$?
    trap - EXIT
    if ! rm -f -- "$BINARY" "$MENU_BINARY" "$BACKTEST_BINARY"; then
        printf '[WARN] Could not remove test binaries in: %s\n' "$WORK_DIR" >&2
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

printf '[INFO] Compiling the CLI source-menu helper and independent cases.\n'
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
    -I "$REPO_ROOT/include" -I "$REPO_ROOT/apps/xtrader" \
    "$REPO_ROOT/src/xtrader/market_data_paths.cc" \
    "$REPO_ROOT/tests/unit/xtrader_cli_data_sources_test.cc" \
    -o "$MENU_BINARY"
TMPDIR="$WORK_DIR" "$MENU_BINARY"
printf '[OK] CLI data-source discovery and selection cases passed.\n'

printf '[INFO] Compiling backtest setup menu cases; Run is a construction placeholder.\n'
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
    -I "$REPO_ROOT/include" -I "$REPO_ROOT/apps/xtrader" \
    "$REPO_ROOT/src/xtrader/market_data_paths.cc" \
    "$REPO_ROOT/tests/unit/xtrader_backtest_menu_test.cc" \
    -o "$BACKTEST_BINARY"
TMPDIR="$WORK_DIR" "$BACKTEST_BINARY"
printf '[OK] Backtest source/product/files/strategy selection and placeholder Run passed.\n'
printf '[INFO] Full CLI build is separate; import/replay/recording remain unchanged.\n'
printf '[INFO] No trading verification performed.\n'
