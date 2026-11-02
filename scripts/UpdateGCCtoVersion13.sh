#!/bin/bash
#
# ! Usage: sudo ./UpdateGCCtoVersion13.sh [VERSION]
# !
# ! Upgrade GCC to version 13 or higher on Ubuntu.
# ! This script is required for building Seastar framework which needs GCC 13+.
# !
# ! Examples:
# !   ./UpdateGCCtoVersion13.sh        # Install GCC 13 (default)
# !   ./UpdateGCCtoVersion13.sh 14     # Install GCC 14
# !

set -e

TARGET_VERSION=${1:-13}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="${SCRIPT_DIR}/gcc_update_$(date +%Y%m%d_%H%M%S).log"

usage() {
    cat "$0" | grep ^"# !" | cut -d"!" -f2-
}

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

error() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: $1" | tee -a "$LOG_FILE" >&2
    exit 1
}

check_root() {
    if [ "$EUID" -ne 0 ]; then
        error "This script must be run as root (use sudo)"
    fi
}

check_ubuntu() {
    if [ ! -f /etc/os-release ]; then
        error "Cannot detect OS (not Ubuntu)"
    fi
    . /etc/os-release
    if [[ "$ID" != "ubuntu" ]]; then
        error "This script is designed for Ubuntu, detected: $ID"
    fi
    log "Detected Ubuntu $VERSION_ID ($NAME)"
}

get_current_gcc_version() {
    local gcc_cmd=${1:-gcc}
    if command -v "$gcc_cmd" &> /dev/null; then
        $gcc_cmd --version | head -n1 | sed -n 's/.*\([0-9]\+\.[0-9]\+\.[0-9]\+\).*/\1/p'
    else
        echo "not installed"
    fi
}

install_gcc() {
    local version=$1
    local package="g++-${version}"

    log "Checking if GCC ${version} is available..."
    if dpkg -l | grep -q "^ii  ${package} "; then
        log "GCC ${version} is already installed"
        return 0
    fi

    log "Installing GCC ${version}..."
    export DEBIAN_FRONTEND=noninteractive

    # Add GCC packages repository for Ubuntu 22.04 (jammy)
    if [[ "$VERSION_ID" == "22.04" ]]; then
        log "Adding GCC toolchain PPA for Ubuntu 22.04..."
        add-apt-repository -y ppa:ubuntu-toolchain-r/test 2>&1 | tee -a "$LOG_FILE"
    fi

    # Install GCC
    apt-get update -qq 2>&1 | tee -a "$LOG_FILE"
    apt-get install -y "$package" 2>&1 | tee -a "$LOG_FILE"

    if [ $? -eq 0 ]; then
        log "GCC ${version} installed successfully"
    else
        error "Failed to install GCC ${version}"
    fi
}

set_default_gcc() {
    local version=$1
    local gcc_path="/usr/bin/gcc-${version}"
    local gpp_path="/usr/bin/g++-${version}"

    if [ ! -f "$gcc_path" ]; then
        error "GCC ${version} not found at $gcc_path"
    fi

    log "Setting GCC ${version} as default..."

    update-alternatives --install /usr/bin/gcc gcc "$gcc_path" "${version}0" 2>&1 | tee -a "$LOG_FILE"
    update-alternatives --install /usr/bin/g++ g++ "$gpp_path" "${version}0" 2>&1 | tee -a "$LOG_FILE"

    update-alternatives --set gcc "$gcc_path" 2>&1 | tee -a "$LOG_FILE"
    update-alternatives --set g++ "$gpp_path" 2>&1 | tee -a "$LOG_FILE"

    log "GCC ${version} set as default"
}

verify_gcc() {
    local expected_version=$1
    local actual_version

    actual_version=$(get_current_gcc_version gcc)
    log "Current system GCC version: ${actual_version}"

    # Extract major version
    local major_version
    major_version=$(echo "$actual_version" | cut -d. -f1)

    if [ "$major_version" -ge "$expected_version" ]; then
        log "SUCCESS: GCC ${actual_version} meets requirement (>= ${expected_version})"
        log "GCC location: $(which gcc)"
        log "G++ location: $(which g++)"
        return 0
    else
        error "GCC version ${actual_version} is below required ${expected_version}"
    fi
}

main() {
    if [ $# -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
        usage
        exit 0
    fi

    log "=========================================="
    log "Seastar GCC Upgrade Script"
    log "Target version: GCC ${TARGET_VERSION}+"
    log "Log file: $LOG_FILE"
    log "=========================================="

    check_root
    check_ubuntu

    local current_version
    current_version=$(get_current_gcc_version gcc)
    log "Current GCC version: ${current_version}"

    # Check if already at target version
    local current_major
    current_major=$(echo "$current_version" | cut -d. -f1)
    if [ "$current_major" -ge "$TARGET_VERSION" ]; then
        log "GCC ${current_version} already meets requirement (>= ${TARGET_VERSION})"
        log "No upgrade needed."
        exit 0
    fi

    install_gcc "$TARGET_VERSION"
    set_default_gcc "$TARGET_VERSION"

    log "Updating library cache..."
    ldconfig 2>&1 | tee -a "$LOG_FILE"

    verify_gcc "$TARGET_VERSION"

    log "=========================================="
    log "GCC upgrade completed successfully!"
    log "You may need to restart your shell session."
    log "=========================================="
}

main "$@"
