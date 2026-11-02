#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

# ============================================================
# 编译日志配置
# ============================================================
BUILD_TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BUILD_LOG_DIR="${ROOT_DIR}/build_logs"
mkdir -p "${BUILD_LOG_DIR}"

LOG_FILE="${BUILD_LOG_DIR}/build_${BUILD_TIMESTAMP}.log"
LOG_CMAKE="${BUILD_LOG_DIR}/cmake_${BUILD_TIMESTAMP}.log"
LOG_NINJA="${BUILD_LOG_DIR}/ninja_${BUILD_TIMESTAMP}.log"
LOG_ENV="${BUILD_LOG_DIR}/env_${BUILD_TIMESTAMP}.log"
LOG_LDD="${BUILD_LOG_DIR}/ldd_${BUILD_TIMESTAMP}.log"

# 日志文件头
echo "========================================" > "${LOG_FILE}"
echo "X-Trader Seastar Build Log" >> "${LOG_FILE}"
echo "Build Time: ${BUILD_TIMESTAMP}" >> "${LOG_FILE}"
echo "User: ${USER:-unknown}" >> "${LOG_FILE}"
echo "Host: ${HOSTNAME:-unknown}" >> "${LOG_FILE}"
echo "========================================" >> "${LOG_FILE}"
echo "" >> "${LOG_FILE}"

# 保存环境信息
echo "=== Environment Info ===" > "${LOG_ENV}"
echo "Date: $(date)" >> "${LOG_ENV}"
echo "Hostname: ${HOSTNAME:-unknown}" >> "${LOG_ENV}"
echo "User: ${USER:-unknown}" >> "${LOG_ENV}"
echo "Kernel: $(uname -r)" >> "${LOG_ENV}"
echo "" >> "${LOG_ENV}"
echo "=== Environment Variables ===" >> "${LOG_ENV}"
env | grep -E "(CC|CXX|CMAKE|Conda|LD_LIBRARY_PATH|PATH)" >> "${LOG_ENV}" 2>/dev/null || true
echo "" >> "${LOG_ENV}"
echo "=== GCC Version ===" >> "${LOG_ENV}"
gcc --version >> "${LOG_ENV}" 2>&1 || echo "gcc not found" >> "${LOG_ENV}"
echo "" >> "${LOG_ENV}"
echo "=== CMake Version ===" >> "${LOG_ENV}"
cmake --version >> "${LOG_ENV}" 2>&1 || echo "cmake not found" >> "${LOG_ENV}"
echo "" >> "${LOG_ENV}"
echo "=== Ninja Version ===" >> "${LOG_ENV}"
ninja --version >> "${LOG_ENV}" 2>&1 || echo "ninja not found" >> "${LOG_ENV}"

echo "Environment info saved to: ${LOG_ENV}" | tee -a "${LOG_FILE}"

# ============================================================
# 编译配置
# ============================================================
JOBS="${JOBS:-1}"

if [ "${JOBS}" -gt 2 ]; then
	echo "[warn] JOBS=${JOBS} may trigger OOM on 2GB memory, consider JOBS=1"
fi

if [ -n "${CONDA_PREFIX:-}" ]; then
	echo "[warn] Conda is active (${CONDA_PREFIX})."
	echo "[warn] Runtime may pick ${CONDA_PREFIX}/lib/libstdc++.so.6 and cause GLIBCXX mismatch."
fi

# ============================================================
# CMake 配置阶段
# ============================================================
echo "" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Step 1: CMake Configuration" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Command: cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "${LOG_CMAKE}" | tee -a "${LOG_FILE}"
CMAKE_EXIT=${PIPESTATUS[0]}

if [ ${CMAKE_EXIT} -ne 0 ]; then
    echo "" | tee -a "${LOG_FILE}"
    echo "[ERROR] CMake configuration failed! Exit code: ${CMAKE_EXIT}" | tee -a "${LOG_FILE}"
    echo "Full log: ${LOG_CMAKE}"
    exit 1
fi

echo "" | tee -a "${LOG_FILE}"
echo "CMake configuration completed successfully" | tee -a "${LOG_FILE}"
echo "CMake log saved to: ${LOG_CMAKE}" | tee -a "${LOG_FILE}"

# ============================================================
# Ninja 编译阶段
# ============================================================
echo "" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Step 2: Ninja Build (JOBS=${JOBS})" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Command: ninja -C build/release -j${JOBS} app_xtrader_cli" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

# 编译开始时间
BUILD_START=$(date +%s)

ninja -C build/release -j"${JOBS}" app_xtrader_cli 2>&1 | tee "${LOG_NINJA}" | tee -a "${LOG_FILE}"
NINJA_EXIT=${PIPESTATUS[0]}

# 编译结束时间
BUILD_END=$(date +%s)
BUILD_DURATION=$((BUILD_END - BUILD_START))

if [ ${NINJA_EXIT} -ne 0 ]; then
    echo "" | tee -a "${LOG_FILE}"
    echo "[ERROR] Ninja build failed! Exit code: ${NINJA_EXIT}" | tee -a "${LOG_FILE}"
    echo "Full log: ${LOG_NINJA}"
    echo "Build duration: ${BUILD_DURATION} seconds"
    exit 1
fi

echo "" | tee -a "${LOG_FILE}"
echo "Ninja build completed successfully" | tee -a "${LOG_FILE}"
echo "Build duration: ${BUILD_DURATION} seconds" | tee -a "${LOG_FILE}"
echo "Ninja log saved to: ${LOG_NINJA}" | tee -a "${LOG_FILE}"

# ============================================================
# 生成启动脚本
# ============================================================
BIN_PATH="$ROOT_DIR/build/release/apps/xtrader/xtrader_cli"
LAUNCHER_PATH="$ROOT_DIR/build/release/apps/xtrader/run_xtrader_cli.sh"

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

# ============================================================
# LDD 检查
# ============================================================
echo "" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Step 3: LDD Check" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"

ldd "${BIN_PATH}" 2>&1 | tee "${LOG_LDD}" | tee -a "${LOG_FILE}"

if ldd "${BIN_PATH}" 2>/dev/null | grep -q "miniconda3/lib/libstdc++.so.6"; then
	echo "[warn] ldd still resolves libstdc++ to miniconda. Please run via launcher above."
fi

# ============================================================
# 完成汇总
# ============================================================
echo "" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Build Summary" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"
echo "Status: SUCCESS" | tee -a "${LOG_FILE}"
echo "Build Time: ${BUILD_TIMESTAMP}" | tee -a "${LOG_FILE}"
echo "Duration: ${BUILD_DURATION} seconds" | tee -a "${LOG_FILE}"
echo "Binary: ${BIN_PATH}" | tee -a "${LOG_FILE}"
echo "Launcher: ${LAUNCHER_PATH}" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"
echo "Log Files:" | tee -a "${LOG_FILE}"
echo "  - Full log:   ${LOG_FILE}" | tee -a "${LOG_FILE}"
echo "  - CMake log:  ${LOG_CMAKE}" | tee -a "${LOG_FILE}"
echo "  - Ninja log:  ${LOG_NINJA}" | tee -a "${LOG_FILE}"
echo "  - Env log:    ${LOG_ENV}" | tee -a "${LOG_FILE}"
echo "  - LDD log:    ${LOG_LDD}" | tee -a "${LOG_FILE}"
echo "========================================" | tee -a "${LOG_FILE}"

echo ""
echo "Release build completed: ${BIN_PATH}"
echo "Run with clean runtime env: ${LAUNCHER_PATH}"
echo "Full build log: ${LOG_FILE}"
echo ""
echo "All log files are saved in: ${BUILD_LOG_DIR}/"
