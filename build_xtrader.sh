#!/usr/bin/env bash
# Update date: 2026-11-10
# Version: 2026.11.10.1
# 在 LD/阿里云 Linux 服务器的仓库目录运行；本脚本不会连接远端或部署服务。
# Build current xtrader applications, including B5-B8 components, in Debug mode.
# Task source: user request
# Requirement: update cloud Debug/Release builds; do not run regressions.
# Signature: GitHub Copilot (gpt-6-astra)
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

JOBS="${JOBS:-1}"
STALL_INTERVAL_SECONDS="${STALL_INTERVAL_SECONDS:-60}"
RUN_HELP_CHECKS="${RUN_HELP_CHECKS:-0}"
LOG_DIR="${ROOT_DIR}/build/logs"
LOG_FILE="${LOG_DIR}/build_xtrader_$(date '+%Y%m%d_%H%M%S').log"

if ! [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
	echo "[ERROR] JOBS must be a positive integer (got: ${JOBS})" >&2
	exit 2
fi

if ! [[ "${STALL_INTERVAL_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
	echo "[ERROR] STALL_INTERVAL_SECONDS must be a positive integer (got: ${STALL_INTERVAL_SECONDS})" >&2
	exit 2
fi

if ! [[ "${RUN_HELP_CHECKS}" =~ ^[01]$ ]]; then
	echo "[ERROR] RUN_HELP_CHECKS must be 0 or 1 (got: ${RUN_HELP_CHECKS})" >&2
	exit 2
fi

mkdir -p "${LOG_DIR}"
touch "${LOG_FILE}"

log() {
	printf '%s %s\n' "[$(date '+%F %T')]" "$*" | tee -a "${LOG_FILE}"
}

run_step() {
	local title="$1"
	shift

	log "===== ${title} ====="
	log "Command: $*"

	local started_at
	local last_output_at
	local current_size
	local last_line
	local process_id
	local status
	local previous_size
	started_at="$(date +%s)"
	last_output_at="${started_at}"
	previous_size="$(wc -c < "${LOG_FILE}")"

	(
		set +e
		"$@" 2>&1 | tee -a "${LOG_FILE}"
		status="$?"
		exit "${status}"
	) &
	process_id=$!

	while kill -0 "${process_id}" 2>/dev/null; do
		sleep 1
		current_size="$(wc -c < "${LOG_FILE}")"
		if [[ "${current_size}" != "${previous_size}" ]]; then
			previous_size="${current_size}"
			last_output_at="$(date +%s)"
			continue
		fi

		if (( $(date +%s) - last_output_at >= STALL_INTERVAL_SECONDS )); then
			last_line="$(tail -n 1 "${LOG_FILE}" | tr -d '\r')"
			log "[WAIT] ${title} has produced no output for ${STALL_INTERVAL_SECONDS}s; still running."
			log "[WAIT] Elapsed: $(( $(date +%s) - started_at ))s; last output: ${last_line:-<none>}"
			last_output_at="$(date +%s)"
		fi
	done

	if wait "${process_id}"; then
		log "[OK] ${title} completed in $(( $(date +%s) - started_at ))s."
	else
		status="$?"
		log "[ERROR] ${title} failed with exit code ${status}; log: ${LOG_FILE}"
		return "${status}"
	fi
}

log "xtrader Debug build started. JOBS=${JOBS}; stall notice interval=${STALL_INTERVAL_SECONDS}s"
log "Full log: ${LOG_FILE}"
log "Build only by default; RUN_HELP_CHECKS=${RUN_HELP_CHECKS}. No regressions or deployment."
if [ -n "${CONDA_PREFIX:-}" ]; then
	log "[WARN] Conda is active (${CONDA_PREFIX}); cached or runtime dependencies may come from it."
fi

run_step "Configure Debug build" \
	env BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local \
	python3 configure.py --mode=debug --disable-dpdk

run_step "Build xtrader applications, B5-B8 components and offline metrics probe" \
	ninja -C build/debug -j"${JOBS}" app_xtrader_cli app_xtrader_demo \
		app_xtrader_replay app_xtrader_paper app_xtrader_ctp_sim app_xtrader_live \
		app_xtrader_phase6_probe

run_step "Check acceptance tool Python syntax (no execution or bytecode files)" \
	python3 -c 'import pathlib; import sys; [compile(pathlib.Path(p).read_text(encoding="utf-8-sig"), p, "exec") for p in sys.argv[1:]]; print("Python syntax OK; no acceptance checks or tests executed")' \
		tools/xtrader_acceptance.py tests/unit/xtrader_acceptance_test.py

if [[ "${RUN_HELP_CHECKS}" == "1" ]]; then
	for entry in cli demo replay paper ctp_sim live; do
		run_step "Check xtrader ${entry} help only" \
			"${ROOT_DIR}/build/debug/apps/xtrader/xtrader_${entry}" --help
	done
else
	log "[SKIP] Executable help checks disabled; set RUN_HELP_CHECKS=1 to opt in."
fi

log "[OK] xtrader Debug build and Python syntax check completed; regressions were not run."
log "Artifacts: build/debug/apps/xtrader/xtrader_{cli,demo,replay,paper,ctp_sim,live,phase6_probe}"
log "[WARN] Trading entry backends are not operational; --help is not a trading verification."
