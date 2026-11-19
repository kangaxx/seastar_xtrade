#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
	echo "Usage: $0 <debug|release> <artifact-directory> <build-info-directory>" >&2
	exit 2
}

if [ "$#" -ne 3 ]; then
	usage
fi

VARIANT="$1"
ARTIFACT_DIRECTORY="$2"
BUILD_INFO_DIRECTORY="$3"

if [ "${VARIANT}" != "debug" ] && [ "${VARIANT}" != "release" ]; then
	usage
fi
if [ ! -d "${ARTIFACT_DIRECTORY}" ]; then
	echo "[ERROR] Artifact directory does not exist: ${ARTIFACT_DIRECTORY}" >&2
	exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="${ROOT_DIR}/apps/xtrader/VERSION"
INFO_FILE="${BUILD_INFO_DIRECTORY}/xtrader-build-info.txt"

if [ ! -f "${VERSION_FILE}" ]; then
	echo "[ERROR] Static version file does not exist: ${VERSION_FILE}" >&2
	exit 2
fi

STATIC_VERSION="$(tr -d '[:space:]' < "${VERSION_FILE}")"
if ! [[ "${STATIC_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	echo "[ERROR] Invalid static version: ${STATIC_VERSION}" >&2
	exit 2
fi

mkdir -p "${BUILD_INFO_DIRECTORY}"
ARTIFACT_DIRECTORY="$(cd "${ARTIFACT_DIRECTORY}" && pwd -P)"
BUILD_INFO_DIRECTORY="$(cd "${BUILD_INFO_DIRECTORY}" && pwd -P)"
case "${BUILD_INFO_DIRECTORY}/" in
	"${ARTIFACT_DIRECTORY}/"*)
		echo "[ERROR] Build information must be outside the artifact directory" >&2
		exit 2
		;;
esac
INFO_FILE="${BUILD_INFO_DIRECTORY}/xtrader-build-info.txt"
TMP_MANIFEST="$(mktemp)"
TMP_FILES="$(mktemp)"
TMP_INFO=""

cleanup() {
	rm -f "${TMP_MANIFEST}" "${TMP_FILES}"
	if [ -n "${TMP_INFO}" ]; then rm -f "${TMP_INFO}"; fi
}
trap cleanup EXIT

find "${ARTIFACT_DIRECTORY}" -type d -name CMakeFiles -prune -o \
	-type f ! -name cmake_install.cmake ! -name CTestTestfile.cmake ! -name Makefile -print0 \
	| sort -z > "${TMP_FILES}"
while IFS= read -r -d '' file; do
	relative="${file#"${ARTIFACT_DIRECTORY}/"}"
	case "${relative}" in
		*.sh|*.log|*.txt|*.json|*.ini|*.enc)
			continue
			;;
	esac
	size="$(stat -c '%s' "${file}")"
	hash="$(sha256sum "${file}" | awk '{print $1}')"
	printf '%s\t%s\t%s\n' "${relative}" "${size}" "${hash}" >> "${TMP_MANIFEST}"
done < "${TMP_FILES}"

if [ ! -s "${TMP_MANIFEST}" ]; then
	echo "[ERROR] No build artifacts found in ${ARTIFACT_DIRECTORY}" >&2
	exit 2
fi

sort "${TMP_MANIFEST}" -o "${TMP_MANIFEST}"
CURRENT_HASH="$(sha256sum "${TMP_MANIFEST}" | awk '{print $1}')"

CURRENT_SERIAL=0
LATEST_HASH=""
declare -a HISTORY_SERIALS=()
declare -a HISTORY_HASHES=()
declare -a HISTORY_TIMES=()

if [ -f "${INFO_FILE}" ]; then
	while IFS='=' read -r key value; do
		case "${key}" in
			current_serial) CURRENT_SERIAL="${value}";;
			latest_sha256) LATEST_HASH="${value}";;
		esac
	done < "${INFO_FILE}"

	if ! [[ "${CURRENT_SERIAL}" =~ ^[0-9]+$ ]]; then
		echo "[ERROR] Invalid current_serial in ${INFO_FILE}" >&2
		exit 2
	fi

	in_history=false
	while IFS=$'\t' read -r serial hash timestamp; do
		if [ "${serial}" = "[history]" ]; then
			in_history=true
			continue
		fi
		if [ "${in_history}" = true ] && [[ "${serial}" =~ ^[0-9]+$ ]] && [ -n "${hash}" ]; then
			HISTORY_SERIALS+=("${serial}")
			HISTORY_HASHES+=("${hash}")
			HISTORY_TIMES+=("${timestamp:-}")
		fi
	done < "${INFO_FILE}"
fi

if [ "${LATEST_HASH}" != "${CURRENT_HASH}" ]; then
	CURRENT_SERIAL=$((CURRENT_SERIAL + 1))
fi

if [ "${LATEST_HASH}" != "${CURRENT_HASH}" ] || [ "${CURRENT_SERIAL}" -eq 0 ]; then
	TIMESTAMP="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	HISTORY_SERIALS+=("${CURRENT_SERIAL}")
	HISTORY_HASHES+=("${CURRENT_HASH}")
	HISTORY_TIMES+=("${TIMESTAMP}")
else
	TIMESTAMP=""
fi

count="${#HISTORY_SERIALS[@]}"
if [ "${count}" -gt 500 ]; then
	start=$((count - 500))
	HISTORY_SERIALS=("${HISTORY_SERIALS[@]:start}")
	HISTORY_HASHES=("${HISTORY_HASHES[@]:start}")
	HISTORY_TIMES=("${HISTORY_TIMES[@]:start}")
fi

TMP_INFO="$(mktemp "${BUILD_INFO_DIRECTORY}/.xtrader-build-info.XXXXXXXX")"
{
	printf 'static_version=%s\n' "${STATIC_VERSION}"
	printf 'variant=%s\n' "${VARIANT}"
	printf 'current_serial=%s\n' "${CURRENT_SERIAL}"
	printf 'latest_sha256=%s\n' "${CURRENT_HASH}"
	printf '[history]\n'
	count="${#HISTORY_SERIALS[@]}"
	for ((i=0; i<count; i++)); do
		printf '%s\t%s\t%s\n' "${HISTORY_SERIALS[i]}" "${HISTORY_HASHES[i]}" "${HISTORY_TIMES[i]}"
	done
} > "${TMP_INFO}"

chmod 0644 "${TMP_INFO}"
mv -f "${TMP_INFO}" "${INFO_FILE}"

printf '%s.%s.%s\n' "${STATIC_VERSION}" "${CURRENT_SERIAL}" "${VARIANT}"
