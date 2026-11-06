#!/usr/bin/env bash
# Build and smoke-check both xtrader applications in the current repository.
# Task source: user request
# Requirement: run the VBox debug configure/build workflow and report stalled stages.
# Date: 2026-11-05
# Signature: GitHub Copilot (Auto)
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

JOBS="${JOBS:-2}"
STALL_INTERVAL_SECONDS="${STALL_INTERVAL_SECONDS:-60}"
LOG_DIR="${ROOT_DIR}/build/logs"
LOG_FILE="${LOG_DIR}/build_xtrader_$(date '+%Y%m%d_%H%M%S').log"
CURRENT_STAGE="startup"

if ! [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
	echo "[ERROR] JOBS must be a positive integer (got: ${JOBS})" >&2
	exit 2
fi

if ! [[ "${STALL_INTERVAL_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
	echo "[ERROR] STALL_INTERVAL_SECONDS must be a positive integer (got: ${STALL_INTERVAL_SECONDS})" >&2
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

	CURRENT_STAGE="${title}"
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
		status="${PIPESTATUS[0]}"
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

run_step "Configure Debug build" \
	env BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local \
	python3 configure.py --mode=debug --disable-dpdk

run_step "Build xtrader CLI and demo" \
	ninja -C build/debug -j"${JOBS}" app_xtrader_cli app_xtrader_demo

run_step "Verify xtrader CLI" \
	"${ROOT_DIR}/build/debug/apps/xtrader/xtrader_cli" --help

run_step "Verify xtrader demo" \
	"${ROOT_DIR}/build/debug/apps/xtrader/xtrader_demo" --help

CURRENT_STAGE="complete"
log "[OK] All xtrader build and verification steps completed."
log "Artifacts: build/debug/apps/xtrader/xtrader_cli and build/debug/apps/xtrader/xtrader_demo"
