#!/usr/bin/env bash
# 注册脚本隔离测试，仅使用临时假程序，不访问真实构建或系统命令目录。
# 任务来源：Release 命令注册；2026-11-18；签名：GitHub Copilot (Auto)。
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
REGISTER="$ROOT_DIR/register_xtrader_release.sh"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf -- "$WORK_DIR"' EXIT

fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }
expect_failure() {
    if bash "$REGISTER" "$@" >"$WORK_DIR/output" 2>&1; then
        fail "expected failure: $*"
    fi
}
make_program() {
    printf '#!/usr/bin/env bash\nprintf "fixture-only\\n"\n' >"$1"
    chmod +x "$1"
}

release="$WORK_DIR/release apps"
bin="$WORK_DIR/commands"
mkdir -p -- "$release" "$bin"
expect_failure --release-dir "$WORK_DIR/missing" --bin-dir "$bin"
expect_failure --release-dir "$release" --bin-dir "$bin"
make_program "$release/xtrader_cli"
make_program "$release/xtrader_demo"
printf 'preserve\n' >"$bin/xtrader_demo"
expect_failure --release-dir "$release" --bin-dir "$bin"
[[ "$(cat "$bin/xtrader_demo")" == preserve && ! -e "$bin/xtrader_cli" ]] \
    || fail 'preflight must preserve ordinary files and avoid partial registration'
expect_failure --replace-links --release-dir "$release" --bin-dir "$bin"
expect_failure --release-dir
expect_failure --release-dir "$release" --bin-dir "$release"
rm -- "$bin/xtrader_demo"
printf '[OK] Invalid arguments, missing builds and collision protection passed.\n'

if ! ln -s -- "$release/xtrader_cli" "$WORK_DIR/probe" 2>/dev/null || [[ ! -L "$WORK_DIR/probe" ]]; then
    printf '[SKIP] Real symlinks unavailable; rerun on Linux to check registration and updates.\n'
    exit 77
fi

bash "$REGISTER" --release-dir "$release" --bin-dir "$bin"
[[ -L "$bin/xtrader_cli" && "$(readlink "$bin/xtrader_cli")" == "$release/xtrader_cli" ]] \
    || fail 'CLI must be a symlink to the release executable'
[[ "$("$bin/xtrader_cli")" == fixture-only ]] || fail 'registered command must run'
bash "$REGISTER" --release-dir "$release" --bin-dir "$bin"
make_program "$release/xtrader_market_config"
bash "$REGISTER" --release-dir "$release" --bin-dir "$bin"
[[ -L "$bin/xtrader_market_config" ]] || fail 'rerun must discover newly built commands'
printf 'library\n' >"$release/xtrader_fake.so"
chmod +x "$release/xtrader_fake.so"
bash "$REGISTER" --release-dir "$release" --bin-dir "$bin"
[[ ! -e "$bin/xtrader_fake.so" ]] || fail 'libraries must not become commands'

other="$WORK_DIR/other release"
mkdir -- "$other"
make_program "$other/xtrader_cli"
expect_failure --release-dir "$other" --bin-dir "$bin"
[[ "$(readlink "$bin/xtrader_cli")" == "$release/xtrader_cli" ]] || fail 'foreign link must be preserved'
bash "$REGISTER" --replace-links --release-dir "$other" --bin-dir "$bin"
[[ "$(readlink "$bin/xtrader_cli")" == "$other/xtrader_cli" ]] || fail 'explicit retargeting must work'
rm -- "$other/xtrader_cli"
make_program "$other/xtrader_cli"
bash "$REGISTER" --release-dir "$other" --bin-dir "$bin"
printf '[OK] Registration, reruns, new programs and explicit retargeting passed.\n'
