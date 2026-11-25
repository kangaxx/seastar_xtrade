#include "market_capture_runtime.hh"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/prctl.h>
#include <unistd.h>

namespace seastar::xtrader::cli {
namespace {

volatile std::sig_atomic_t STOP_REQUESTED = 0;

void request_stop(int signal_number) {
    if (STOP_REQUESTED != 0) {
        ::signal(signal_number, SIG_DFL);
        ::raise(signal_number);
        ::_exit(128 + signal_number);
    }
    STOP_REQUESTED = 1;
}

} // namespace

bool stop_requested() noexcept { return STOP_REQUESTED != 0; }

void install_signals() {
    struct sigaction action{};
    action.sa_handler = request_stop;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0
            || ::sigaction(SIGHUP, &action, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "install stop signal handlers");
    }
    action.sa_handler = SIG_IGN;
    if (::sigaction(SIGPIPE, &action, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "configure output failure handling");
    }
    const auto parent = ::getppid();
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        throw std::system_error(errno, std::generic_category(), "watch launcher lifetime");
    }
    if (::getppid() != parent) { STOP_REQUESTED = 1; }
}

std::filesystem::path state_directory() {
    if (const char* path = std::getenv("XTRADER_MARKET_STATE_DIR"); path && *path) { return path; }
    if (const char* path = std::getenv("XDG_STATE_HOME"); path && *path) {
        return std::filesystem::path(path) / "xtrader" / "market";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "xtrader" / "market";
    }
    throw std::runtime_error("set HOME, XDG_STATE_HOME or XTRADER_MARKET_STATE_DIR");
}

} // namespace seastar::xtrader::cli
