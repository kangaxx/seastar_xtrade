#!/usr/bin/env bash
# 将 Release 程序注册为 PATH 命令，不复制程序、不启动服务或连接 CTP。
# 任务来源：用户要求；2026-11-18；签名：GitHub Copilot (Auto)。
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
RELEASE_DIR="$ROOT_DIR/build/release/apps/xtrader"
BIN_DIR="/usr/local/bin"
REPLACE_LINKS=0
WORK_DIR=""

usage() {
    cat <<'HELP'
用法：bash register_xtrader_release.sh [选项]

将 Release 目录中可执行的 xtrader_* 程序注册为同名软链接。
默认目标：仓库内 build/release/apps/xtrader
默认命令目录：/usr/local/bin（通常需要 sudo）

  --bin-dir DIR       命令软链接目录，例如 "$HOME/.local/bin"
  --release-dir DIR   指定其他 Release 程序目录
  --replace-links    允许替换指向其他位置的同名软链接；绝不覆盖普通文件或目录
  -h, --help         显示中文说明

可反复运行：保留正确链接，自动注册新增程序。同目录重新编译后无需再注册。
脚本不修改 PATH 或 shell 配置，不删除已经移除的程序的旧链接。
HELP
}

fail() {
    printf '[错误] %s\n' "$*" >&2
    exit 1
}

cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$WORK_DIR" ]]; then
        # 仅清理本次生成的暂存链接，不能删除目标程序。
        local link
        for link in "$WORK_DIR"/xtrader_*; do
            [[ -L "$link" ]] || continue
            if ! rm -- "$link"; then
                printf '[警告] 无法清理暂存链接：%s\n' "$link" >&2
                status=1
            fi
        done
        if ! rmdir -- "$WORK_DIR"; then
            printf '[警告] 暂存目录保留：%s\n' "$WORK_DIR" >&2
            status=1
        fi
    fi
    exit "$status"
}
trap cleanup EXIT

while (( $# )); do
    case "$1" in
        --bin-dir|--release-dir)
            (( $# >= 2 )) && [[ -n "$2" && "$2" != --* ]] || fail "$1 缺少目录参数"
            if [[ "$1" == --bin-dir ]]; then BIN_DIR="$2"; else RELEASE_DIR="$2"; fi
            shift 2
            ;;
        --replace-links) REPLACE_LINKS=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "未知参数：$1；请使用 --help 查看用法" ;;
    esac
done

[[ -d "$RELEASE_DIR" ]] || fail "Release 目录不存在：$RELEASE_DIR；请先运行 bash build_xtrader_demo_release.sh"
RELEASE_DIR="$(cd -- "$RELEASE_DIR" && pwd -P)"
[[ -f "$RELEASE_DIR/xtrader_cli" && -x "$RELEASE_DIR/xtrader_cli" ]] \
    || fail "缺少可执行的 xtrader_cli：$RELEASE_DIR；请先完成 Release 编译"
mkdir -p -- "$BIN_DIR" || fail "无法创建命令目录：$BIN_DIR；可使用 sudo 或 --bin-dir 指定用户目录"
BIN_DIR="$(cd -- "$BIN_DIR" && pwd -P)"
[[ "$BIN_DIR" != "$RELEASE_DIR" ]] || fail "命令目录不能与 Release 程序目录相同"
[[ -w "$BIN_DIR" && -x "$BIN_DIR" ]] || fail "命令目录不可写：$BIN_DIR"

shopt -s nullglob
PROGRAMS=()
for program in "$RELEASE_DIR"/xtrader_*; do
    name="${program##*/}"
    [[ "$name" =~ ^xtrader_[a-z0-9_]+$ && -f "$program" && -x "$program" ]] || continue
    PROGRAMS+=("$program")
done

check_destination() {
    local source="$1" destination="$2" existing
    if [[ -L "$destination" ]]; then
        existing="$(readlink -- "$destination")"
        [[ "$existing" == "$source" || "$REPLACE_LINKS" == 1 ]] \
            || fail "同名软链接指向其他位置：$destination；核对后可使用 --replace-links"
    elif [[ -e "$destination" ]]; then
        fail "拒绝覆盖已有普通文件或目录：$destination"
    fi
}

# 先检查所有冲突，避免因后面的命名冲突产生半套注册结果。
for program in "${PROGRAMS[@]}"; do
    check_destination "$program" "$BIN_DIR/${program##*/}"
done

printf '[说明] Release 程序目录：%s\n[说明] 命令软链接目录：%s\n' "$RELEASE_DIR" "$BIN_DIR"
WORK_DIR="$(mktemp -d "$BIN_DIR/.xtrader-register.XXXXXXXX")"
for program in "${PROGRAMS[@]}"; do
    name="${program##*/}"
    destination="$BIN_DIR/$name"
    check_destination "$program" "$destination"
    if [[ -L "$destination" && "$(readlink -- "$destination")" == "$program" ]]; then
        printf '[保留] %s -> %s\n' "$name" "$program"
        continue
    fi
    ln -s -- "$program" "$WORK_DIR/$name" || fail "无法创建软链接：$name"
    [[ -L "$WORK_DIR/$name" ]] || fail "当前系统未创建真实软链接；请在 Linux 运行本脚本"
    mv -Tf -- "$WORK_DIR/$name" "$destination" || fail "无法注册命令：$destination"
    printf '[注册] %s -> %s\n' "$name" "$program"
done

printf '[完成] 已检查并注册 %s 个命令；未启动任何程序。\n' "${#PROGRAMS[@]}"
case ":${PATH:-}:" in
    *":$BIN_DIR:"*) printf '[用法] 当前 PATH 包含命令目录，可执行：xtrader_cli\n' ;;
    *)
        printf '[提示] 当前 PATH 不包含命令目录。请在运行 CLI 的用户终端执行：\n'
        printf 'export PATH=%q:"$PATH"\n' "$BIN_DIR"
        printf '[提示] 如需永久生效，请将上行加入该用户的 shell 配置，再打开新终端。\n'
        ;;
esac
printf '[提示] 若命令曾指向其他版本，请执行 hash -r；用 type -a xtrader_cli 检查命令优先级。\n'
printf '[提示] 软链接依赖原 Release 目录和动态库，请勿移动或删除构建目录及其依赖。\n'
