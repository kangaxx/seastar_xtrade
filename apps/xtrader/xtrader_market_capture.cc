// Foreground read-only SDK worker; no TraderApi, strategies, order or account API.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include <seastar/xtrader/version.hh>
#include <ThostFtdcMdApi.h>
#include "market_capture_daemon.hh"
#include "market_capture_login.hh"
#include "market_capture_runtime.hh"
#include "market_contract_menu.hh"
#include "xtrader_log.hh"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <poll.h>
#include <unistd.h>

namespace {

using seastar::xtrader::cli::install_signals;
using seastar::xtrader::cli::state_directory;
using seastar::xtrader::cli::stop_requested;
using seastar::xtrader::cli::ui_text;

namespace xlog = seastar::xtrader::log;

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

} // namespace

int main(int argc, char** argv) {
    xlog::initialize("xtrader_market_capture");
    if (argc >= 2 && std::string(argv[1]) == "--daemon-tick") {
        return seastar::xtrader::cli::run_market_capture_daemon(argc, argv);
    }
    std::unique_ptr<seastar::xtrader::market_capture_session> session;
    try {
        (void)seastar::xtrader::cli::current_ui_language();
        std::cout << "X-Trader Seastar Trading Engine v"
                  << seastar::xtrader::get_full_version() << std::endl;
        xlog::info(std::string("start version=") + seastar::xtrader::get_full_version());
        if (argc == 2 && std::string(argv[1]) == "--help") {
            xlog::info("help requested");
            std::cout << "xtrader_market_capture --config ENCRYPTED_CONFIG --key-file KEY_FILE\n"
                      << ui_text("Read-only CTP MdApi collector. Enter, SIGINT or SIGTERM stops and syncs files.\n"
                                 "Interactive account menu: 1/START connects, 2/SHOW reveals secrets without connecting, u returns.\n"
                                 "Unconfigured contracts: browse exchanges/products or search by name/code before connecting.\n"
                                 "Only /data/x_trader_data is used for Tick output. No trading API is linked.\n",
                                 "CTP MdApi 只读采集器。回车、SIGINT 或 SIGTERM 可停止采集并同步文件。\n"
                                 "交互账号菜单：1/START 连接，2/SHOW 查看凭据但不连接，u 返回。\n"
                                 "未配置合约时：连接前可按交易所/品种浏览，或按名称/代码搜索。\n"
                                 "Tick 输出仅使用 /data/x_trader_data，未链接交易 API。\n");
            return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--key-file") {
            throw std::invalid_argument(ui_text("invalid arguments; use --help", "参数无效，请使用 --help 查看用法"));
        }
        install_signals();
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        auto config = seastar::xtrader::load_market_capture_config(argv[2], argv[4]);
        xlog::info("config=" + std::string(argv[2]) + " contracts=" + std::to_string(config.contracts.size()));
        if (!seastar::xtrader::cli::run_market_login_menu(config, std::cin, std::cout,
                ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO))) {
            xlog::info("account actions cancelled before connecting");
            return 0;
        }
        if (config.contracts.empty()) {
            if (!::isatty(STDIN_FILENO)) {
                throw std::runtime_error(ui_text("this imported configuration has no contracts; start from an interactive terminal",
                                                "该导入配置中没有合约，请从交互终端启动"));
            }
            const auto contracts = seastar::xtrader::cli::run_market_contract_menu(std::cin, std::cout, std::cerr);
            if (!contracts) {
                std::cout << ui_text("[CANCELLED] Contract selection cancelled; no connection made.\n",
                                     "[CANCELLED] 已取消合约选择，未连接服务器。\n");
                return 0;
            }
            config.contracts = *contracts;
            xlog::info("contracts selected interactively=" + std::to_string(config.contracts.size()));
            seastar::xtrader::validate_market_capture_config(config);
        }
        if (stop_requested()) { throw std::runtime_error(ui_text("market capture cancelled before connecting", "已在连接前取消行情采集")); }
        const seastar::xtrader::market_data_paths data_paths;
        const auto state_dir = state_directory();
        xlog::info("data_root=" + data_paths.source_root().string() + " state_dir=" + state_dir.string());
        session = std::make_unique<seastar::xtrader::market_capture_session>(
            std::move(config), data_paths, state_dir,
            [] (const char* flow) { return CThostFtdcMdApi::CreateFtdcMdApi(flow, false, false); });
        std::cout << ui_text("[PREPARE] Checking data/state directories before connecting CTP.\n",
                             "[PREPARE] 连接 CTP 前检查数据及状态目录。\n");
        session->start();
        xlog::info("capture started; waiting for connect/login/subscribe");
        std::cout << ui_text("[RECORDING] Automatic Tick CSV append enabled. Source directory: ",
                             "[RECORDING] 已启用 Tick CSV 自动追加写入。来源目录：")
                  << data_paths.source_root().string() << '\n'
                  << ui_text("Files are created on the first valid Tick and rotated by ActionDay.\n"
                             "Sync runs about once per second and on normal stop; no separate save action is needed.\n",
                             "首个有效 Tick 到达时创建文件，按 ActionDay 自然日期分文件。\n"
                             "约每秒同步一次，正常停止时再次同步，无需单独保存操作。\n");
        std::cout << ui_text("[START] Read-only CTP market capture; no orders can be submitted.\n"
                             "Press Enter to stop and sync Tick files. Waiting for login/subscription acknowledgements.\n",
                             "[START] CTP 只读行情采集，不能提交订单。\n"
                             "按回车停止并同步 Tick 文件。正在等待登录/订阅确认。\n");
        auto next_status = std::chrono::steady_clock::now();
        // One compact heartbeat per minute; per-second status stays on the console only.
        auto next_heartbeat = next_status + std::chrono::seconds(60);
        while (!stop_requested() && !terminal_stop()) {
            session->poll_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_status) {
                session->print_status(std::cout);
                next_status = now + std::chrono::seconds(1);
            }
            if (now >= next_heartbeat) {
                xlog::info(std::string("heartbeat ready=") + (session->is_ready() ? "1" : "0")
                    + " ticks_written=" + std::to_string(session->ticks_written()));
                next_heartbeat = now + std::chrono::seconds(60);
            }
        }
        session->stop();
        xlog::info("stopped ticks_written=" + std::to_string(session->ticks_written()));
        std::cout << ui_text("[STOPPED] Tick files synced; ticks_written=", "[STOPPED] Tick 文件已同步，ticks_written=") << session->ticks_written()
                  << ui_text(". This is not a completeness or trading acceptance result.\n", "。这不代表数据完整性或交易功能验收通过。\n");
        return 0;
    } catch (const std::exception& error) {
        xlog::error(error.what());
        std::cerr << "[ERROR] " << error.what() << '\n';
        if (session) {
            try { session->stop(); }
            catch (const std::exception& stop_error) {
                xlog::error(std::string("stop/sync failed: ") + stop_error.what());
                std::cerr << ui_text("[ERROR] Stop/sync failed: ", "[ERROR] 停止/同步失败：") << stop_error.what() << '\n';
            }
        }
        return 2;
    }
}
