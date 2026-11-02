#!/usr/bin/env bash
set -euo pipefail

# Build and install Boost 1.83.0 from prepared source.
BOOST_VERSION="1_83_0"
BOOST_DIR="/tmp/boost_build_183/boost_${BOOST_VERSION}"
PREFIX="/usr/local"
# 2GB memory host: default to serial build to reduce OOM risk.
JOBS="${JOBS:-1}"

if [ "${JOBS}" -gt 2 ]; then
  echo "[warn] JOBS=${JOBS} may be too high for 2GB memory, consider JOBS=1"
fi

if [ ! -d "${BOOST_DIR}" ]; then
  echo "[error] source directory not found: ${BOOST_DIR}"
  echo "Run ./download_boost_1_83_0.sh first."
  exit 1
fi

cd "${BOOST_DIR}"

if [ ! -f "./b2" ]; then
  echo "[bootstrap] generating b2"
  ./bootstrap.sh
else
  echo "[skip] b2 already exists"
fi

if [ "$(id -u)" -eq 0 ]; then
  echo "[install] running as root"
  ./b2 -j"${JOBS}" cxxflags="-O2 -g0" install --prefix="${PREFIX}"
  ldconfig
else
  echo "[install] running with sudo"
  sudo ./b2 -j"${JOBS}" cxxflags="-O2 -g0" install --prefix="${PREFIX}"
  sudo ldconfig
fi

echo "[verify]"
grep -n "BOOST_LIB_VERSION" "${PREFIX}/include/boost/version.hpp" | head -n 1

echo "[ok] Boost 1.83.0 installed to ${PREFIX}"
