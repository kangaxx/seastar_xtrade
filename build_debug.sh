#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

# 2GB memory host: default to serial compile.
JOBS="${JOBS:-1}"

if [ "${JOBS}" -gt 2 ]; then
	echo "[warn] JOBS=${JOBS} may trigger OOM on 2GB memory, consider JOBS=1"
fi

if [ -n "${CONDA_PREFIX:-}" ]; then
	echo "[warn] Conda is active (${CONDA_PREFIX})."
	echo "[warn] Runtime may pick ${CONDA_PREFIX}/lib/libstdc++.so.6 and cause GLIBCXX mismatch."
fi

cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build/debug -j"${JOBS}" app_xtrader_cli

BIN_PATH="$ROOT_DIR/build/debug/apps/xtrader/xtrader_cli"
LAUNCHER_PATH="$ROOT_DIR/build/debug/apps/xtrader/run_xtrader_cli.sh"

cat > "${LAUNCHER_PATH}" << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Avoid Conda runtime pollution.
if command -v conda >/dev/null 2>&1; then
	conda deactivate 2>/dev/null || true
fi
unset CONDA_PREFIX CONDA_DEFAULT_ENV CONDA_PROMPT_MODIFIER
unset LD_LIBRARY_PATH

# Force system runtime libraries first.
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu

exec "${SCRIPT_DIR}/xtrader_cli" "$@"
EOF

chmod +x "${LAUNCHER_PATH}"

echo "Debug build completed: ${BIN_PATH}"
echo "Run with clean runtime env: ${LAUNCHER_PATH}"

if ldd "${BIN_PATH}" 2>/dev/null | grep -q "miniconda3/lib/libstdc++.so.6"; then
	echo "[warn] ldd still resolves libstdc++ to miniconda. Please run via launcher above."
fi
