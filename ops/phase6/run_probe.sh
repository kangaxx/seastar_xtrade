#!/usr/bin/env bash
# Phase 6 synthetic evidence capture, including failures; no live acceptance claim.
# Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
set -Eeuo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="${ROOT_DIR}/build/phase6/xtrader_phase6_probe"
if [[ ! -x "${PROBE}" ]]; then
    echo "[ERROR] Build the probe first: bash ops/phase6/build_and_check.sh" >&2
    exit 2
fi
if [[ $# -eq 1 && "$1" == "--help" ]]; then
    exec "${PROBE}" --help
fi
LOG_DIR="$(mktemp -d "${ROOT_DIR}/build/phase6/evidence_$(date '+%Y%m%d_%H%M%S')_XXXXXX")"
{
    echo "scope=offline-synthetic; NOT live/CTP/reactor acceptance"
    printf 'command='; printf '%q ' "${PROBE}" "$@"; printf '\n'
    printf 'host='; hostname
    printf 'time_utc='; date -u '+%FT%TZ'
    printf 'git='; git -C "${ROOT_DIR}" rev-parse HEAD
    printf 'worktree_status:\n'; git -C "${ROOT_DIR}" status --short
    printf 'kernel='; uname -srmo
    printf 'probe_sha256='; sha256sum "${PROBE}"
} > "${LOG_DIR}/metadata.txt"
echo "[INFO] Evidence: ${LOG_DIR}"
set +e
"${PROBE}" "$@" 2>"${LOG_DIR}/stderr.log" | tee "${LOG_DIR}/metrics.jsonl"
PIPE_CODES=("${PIPESTATUS[@]}")
set -e
STATUS="${PIPE_CODES[0]}"
if (( PIPE_CODES[1] != 0 )); then STATUS="${PIPE_CODES[1]}"; fi
printf '%s\n' "${STATUS}" > "${LOG_DIR}/exit_code.txt"
if (( STATUS != 0 )); then
    cat "${LOG_DIR}/stderr.log" >&2
    echo "[ERROR] Probe failed (exit ${STATUS}); evidence preserved: ${LOG_DIR}" >&2
    exit "${STATUS}"
fi
echo "[OK] Synthetic probe passed; this is NOT Phase 6 live acceptance."
