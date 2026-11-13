// Foreground read-only SDK worker; no TraderApi, strategies, order or account API.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include <ThostFtdcMdApi.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t STOP_REQUESTED = 0;

void request_stop(int) { STOP_REQUESTED = 1; }

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

bool terminal_stop() {
    if (!::isatty(STDIN_FILENO)) { return false; }
    struct pollfd input{STDIN_FILENO, POLLIN, 0};
    const auto status = ::poll(&input, 1, 0);
    if (status < 0 && errno == EINTR) { return false; }
    if (status < 0) { throw std::system_error(errno, std::generic_category(), "poll stop input"); }
    if (status && (input.revents & (POLLERR | POLLHUP | POLLNVAL))) { return true; }
    if (status && (input.revents & POLLIN)) {
        char ch = '\0';
        const auto count = ::read(STDIN_FILENO, &ch, 1);
        if (count < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "read stop input");
        }
        return count == 0 || (count == 1 && ch == '\n');
    }
    return false;
}

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

} // namespace

int main(int argc, char** argv) {
    std::unique_ptr<seastar::xtrader::market_capture_session> session;
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "xtrader_market_capture --config ENCRYPTED_CONFIG --key-file KEY_FILE\n"
                         "Read-only CTP MdApi collector. Enter, SIGINT or SIGTERM stops and syncs files.\n"
                         "Only /data/x_trader_data is used for Tick output. No trading API is linked.\n";
            return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--key-file") {
            throw std::invalid_argument("invalid arguments; use --help");
        }
        install_signals();
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        auto config = seastar::xtrader::load_market_capture_config(argv[2], argv[4]);
        if (STOP_REQUESTED) { throw std::runtime_error("market capture cancelled before connecting"); }
        session = std::make_unique<seastar::xtrader::market_capture_session>(
            std::move(config), seastar::xtrader::market_data_paths{}, state_directory(),
            [] (const char* flow) { return CThostFtdcMdApi::CreateFtdcMdApi(flow, false, false); });
        std::cout << "[START] Read-only CTP market capture; no orders can be submitted.\n"
                     "Press Enter to stop and sync Tick files. Waiting for login/subscription acknowledgements.\n";
        session->start();
        auto next_status = std::chrono::steady_clock::now();
        while (!STOP_REQUESTED && !terminal_stop()) {
            session->poll_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_status) {
                session->print_status(std::cout);
                next_status = now + std::chrono::seconds(1);
            }
        }
        session->stop();
        std::cout << "[STOPPED] Tick files synced; ticks_written=" << session->ticks_written()
                  << ". This is not a completeness or trading acceptance result.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        if (session) {
            try { session->stop(); }
            catch (const std::exception& stop_error) {
                std::cerr << "[ERROR] Stop/sync failed: " << stop_error.what() << '\n';
            }
        }
        return 2;
    }
}
