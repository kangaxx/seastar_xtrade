// Foreground read-only SDK worker; no TraderApi, strategies, order or account API.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include <seastar/xtrader/version.hh>
#include <ThostFtdcMdApi.h>
#include "market_capture_login.hh"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

using seastar::xtrader::cli::ui_text;

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
        (void)seastar::xtrader::cli::current_ui_language();
        std::cout << "X-Trader Seastar Trading Engine v"
                  << seastar::xtrader::get_full_version() << std::endl;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "xtrader_market_capture --config ENCRYPTED_CONFIG --key-file KEY_FILE\n"
                      << ui_text("Read-only CTP MdApi collector. Enter, SIGINT or SIGTERM stops and syncs files.\n"
                                 "Interactive account menu: 1/START connects, 2/SHOW reveals secrets without connecting, u returns.\n"
                                 "Only /data/x_trader_data is used for Tick output. No trading API is linked.\n",
                                 "CTP MdApi 只读采集器。回车、SIGINT 或 SIGTERM 可停止采集并同步文件。\n"
                                 "交互账号菜单：1/START 连接，2/SHOW 查看凭据但不连接，u 返回。\n"
                                 "Tick 输出仅使用 /data/x_trader_data，未链接交易 API。\n");
            return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--key-file") {
            throw std::invalid_argument(ui_text("invalid arguments; use --help", "参数无效，请使用 --help 查看用法"));
        }
        install_signals();
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        auto config = seastar::xtrader::load_market_capture_config(argv[2], argv[4]);
        if (!seastar::xtrader::cli::run_market_login_menu(config, std::cin, std::cout,
                ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO))) {
            return 0;
        }
        if (config.contracts.empty()) {
            if (!::isatty(STDIN_FILENO)) {
                throw std::runtime_error(ui_text("this imported configuration has no contracts; start from an interactive terminal",
                                                "该导入配置中没有合约，请从交互终端启动"));
            }
            std::string contracts;
            std::cout << ui_text("This configuration has no contracts. Enter concrete contracts "
                                 "(EXCHANGE:instrument, comma separated): ",
                                 "配置中没有合约。请输入具体合约（EXCHANGE:instrument，逗号分隔）：") << std::flush;
            if (!std::getline(std::cin, contracts)) {
                throw std::runtime_error(ui_text("contract selection cancelled", "已取消合约选择"));
            }
            config.contracts = seastar::xtrader::parse_market_capture_contracts(contracts);
            seastar::xtrader::validate_market_capture_config(config);
        }
        if (STOP_REQUESTED) { throw std::runtime_error(ui_text("market capture cancelled before connecting", "已在连接前取消行情采集")); }
        session = std::make_unique<seastar::xtrader::market_capture_session>(
            std::move(config), seastar::xtrader::market_data_paths{}, state_directory(),
            [] (const char* flow) { return CThostFtdcMdApi::CreateFtdcMdApi(flow, false, false); });
        std::cout << ui_text("[PREPARE] Checking data/state directories before connecting CTP.\n",
                             "[PREPARE] 连接 CTP 前检查数据及状态目录。\n");
        session->start();
        std::cout << ui_text("[START] Read-only CTP market capture; no orders can be submitted.\n"
                             "Press Enter to stop and sync Tick files. Waiting for login/subscription acknowledgements.\n",
                             "[START] CTP 只读行情采集，不能提交订单。\n"
                             "按回车停止并同步 Tick 文件。正在等待登录/订阅确认。\n");
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
        std::cout << ui_text("[STOPPED] Tick files synced; ticks_written=", "[STOPPED] Tick 文件已同步，ticks_written=") << session->ticks_written()
                  << ui_text(". This is not a completeness or trading acceptance result.\n", "。这不代表数据完整性或交易功能验收通过。\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        if (session) {
            try { session->stop(); }
            catch (const std::exception& stop_error) {
                std::cerr << ui_text("[ERROR] Stop/sync failed: ", "[ERROR] 停止/同步失败：") << stop_error.what() << '\n';
            }
        }
        return 2;
    }
}
