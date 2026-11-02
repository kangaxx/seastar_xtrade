#!/usr/bin/env bash
set -euo pipefail

# Download and extract Boost 1.83.0 source package.
BOOST_VERSION="1_83_0"
BOOST_DIR="boost_${BOOST_VERSION}"
ARCHIVE="${BOOST_DIR}.tar.gz"
URL_PRIMARY="https://archives.boost.io/release/1.83.0/source/${ARCHIVE}"
URL_FALLBACK="https://sourceforge.net/projects/boost/files/boost/1.83.0/${ARCHIVE}/download"
DEST_DIR="/tmp/boost_build_183"

download_archive() {
  local url="$1"
  echo "[download] ${url}"
  curl -fL --retry 5 --retry-delay 3 -o "${ARCHIVE}" "${url}"
}

archive_is_valid() {
  tar -tzf "${ARCHIVE}" >/dev/null 2>&1
}

mkdir -p "${DEST_DIR}"
cd "${DEST_DIR}"

if [ ! -f "${ARCHIVE}" ]; then
  download_archive "${URL_PRIMARY}" || download_archive "${URL_FALLBACK}"
else
  echo "[skip] archive already exists: ${DEST_DIR}/${ARCHIVE}"
fi

if ! archive_is_valid; then
  echo "[warn] archive is corrupted or incomplete, redownloading"
  rm -f "${ARCHIVE}"
  download_archive "${URL_PRIMARY}" || download_archive "${URL_FALLBACK}"
fi

if ! archive_is_valid; then
  echo "[error] archive integrity check failed after redownload"
  exit 1
fi

if [ ! -d "${BOOST_DIR}" ]; then
  echo "[extract] ${ARCHIVE}"
  tar -xzf "${ARCHIVE}"
else
  echo "[skip] source already extracted: ${DEST_DIR}/${BOOST_DIR}"
fi

if [ ! -f "${BOOST_DIR}/bootstrap.sh" ]; then
  echo "[warn] extracted tree misses bootstrap.sh, forcing re-extract"
  rm -rf "${BOOST_DIR}"
  tar -xzf "${ARCHIVE}"
fi

if [ ! -f "${BOOST_DIR}/bootstrap.sh" ]; then
  echo "[error] bootstrap.sh still missing after re-extract"
  exit 1
fi

echo "[ok] Boost source prepared at ${DEST_DIR}/${BOOST_DIR}"
