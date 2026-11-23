/*
 * X-Trader CLI - Main entry point for X-Trader Seastar trading engine
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/xtrader/backtest_driver.hh>
#include <seastar/xtrader/data_importer.hh>
#include <seastar/xtrader/historical_data_manager.hh>
#include <seastar/xtrader/redis_sync_client.hh>
#include <seastar/xtrader/version.hh>

#include "data_source_menu.hh"
#include "backtest_menu.hh"
#include "market_capture_menu.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using seastar::xtrader::cli::ui_text;

std::string to_lower(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [] (unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

std::filesystem::path config_root() {
    const char* root = std::getenv("XTRADER_CLI_CONFIG_ROOT");
    return root && *root ? std::filesystem::path(root) : std::filesystem::path("/etc/xtrader");
}

struct config_inventory {
    std::vector<std::filesystem::path> envs;
    bool is_complete = true;
};

config_inventory scan_env_dirs(const std::filesystem::path& root) {
    config_inventory inventory;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot inspect configuration root: ", "[ERROR] 无法检查配置根目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
        }
        return inventory;
    }
    if (!std::filesystem::is_directory(root, ec)) {
        std::cerr << ui_text("[ERROR] Configuration root is not an accessible directory", "[ERROR] 配置根目录不可访问或不是目录");
        if (ec) {
            std::cerr << ": " << ec.message();
        }
        std::cerr << std::endl;
        inventory.is_complete = false;
        return inventory;
    }

    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
        std::cerr << ui_text("[ERROR] Cannot list configuration root: ", "[ERROR] 无法列出配置根目录：") << ec.message() << std::endl;
        inventory.is_complete = false;
        return inventory;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const auto& entry = *it;
        const bool is_dir = entry.is_directory(ec);
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot inspect configuration directory: ", "[ERROR] 无法检查配置目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
            ec.clear();
        } else if (is_dir) {
            const auto ini_path = entry.path() / "xtrader.ini";
            if (std::filesystem::is_regular_file(ini_path, ec)) {
                inventory.envs.push_back(entry.path());
            } else if (ec && ec != std::errc::no_such_file_or_directory) {
                std::cerr << ui_text("[ERROR] Cannot inspect account configuration: ", "[ERROR] 无法检查账号配置：") << ec.message() << std::endl;
                inventory.is_complete = false;
            }
            ec.clear();
        }
        it.increment(ec);
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot continue listing configuration root: ", "[ERROR] 无法继续列出配置根目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
            break;
        }
    }
    std::sort(inventory.envs.begin(), inventory.envs.end());
    return inventory;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string declared_environment(const std::filesystem::path& ini) {
    std::ifstream file(ini);
    if (!file) {
        return "UNREADABLE";
    }

    bool has_declaration = false;
    std::string environment = "UNKNOWN";
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != "account_env") {
            continue;
        }
        if (has_declaration) {
            return "UNKNOWN";
        }
        has_declaration = true;
        const auto value = to_lower(trim(line.substr(separator + 1)));
        if (value == "real") {
            environment = "REAL";
        } else if (value == "simu") {
            environment = "SIMU";
        }
    }
    return file.bad() ? "UNREADABLE" : environment;
}

void print_env_summary(const std::filesystem::path& env_dir, size_t index) {
    const auto environment = declared_environment(env_dir / "xtrader.ini");
    if (environment == "UNREADABLE") {
        std::cerr << ui_text("[ERROR] Cannot read account configuration #", "[ERROR] 无法读取账号配置 #") << index << std::endl;
    }
    std::cout << "  [" << environment << ui_text("] configuration #", "] 配置 #") << index
              << ui_text(" (unverified; account details hidden)", "（未验证；账号详情已隐藏）") << std::endl;
}

// ============================================================
// show_config_info（重构版）
// ============================================================
void show_config_info(const seastar::xtrader::market_data_paths& paths,
                      const std::filesystem::path& selected_data_root) {
    std::cout << ui_text("\n========== Configuration Info ==========", "\n========== 配置信息 ==========") << std::endl;
    std::cout << ui_text("Version:    ", "版本：") << seastar::xtrader::get_full_version() << std::endl;
    std::cout << ui_text("Build:      ", "构建模式：") << seastar::xtrader::build_type << std::endl;
    std::cout << ui_text("Git Commit: ", "Git 提交：") << seastar::xtrader::git_commit << std::endl;
    std::cout << ui_text("Git Branch: ", "Git 分支：") << seastar::xtrader::git_branch << std::endl;
    std::cout << ui_text("Build Time: ", "构建时间：") << seastar::xtrader::get_runtime_build_info().timestamp << std::endl;
    std::cout << "========================================" << std::endl;

    const auto root = config_root();
    const auto inventory = scan_env_dirs(root);

    std::cout << ui_text("\n--- Account Environments ---", "\n--- 账号环境 ---") << std::endl;
    std::cout << ui_text("Config Root: ", "配置目录：") << root.string() << std::endl;

    if (!inventory.is_complete) {
        std::cout << ui_text("[ERROR] Configuration inventory is incomplete", "[ERROR] 配置清单不完整") << std::endl;
    } else if (inventory.envs.empty()) {
        std::cout << ui_text("(no environments found)", "（未找到账号环境）") << std::endl;
    }
    for (size_t index = 0; index < inventory.envs.size(); ++index) {
        print_env_summary(inventory.envs[index], index + 1);
    }
    std::cout << ui_text("Found: ", "找到：") << inventory.envs.size() << ui_text(" environment(s)", " 个环境") << std::endl;
    std::cout << ui_text("Account configuration creation is disabled until secure credential management is available.",
                         "此处不允许创建明文账号配置，请使用独立的安全凭据管理入口。")
              << std::endl;

    // 数据目录
    std::cout << ui_text("\n--- Data Directories ---", "\n--- 数据目录 ---") << std::endl;
    std::cout << ui_text("Default Data Root: ", "默认数据目录：") << paths.source_root().string() << std::endl;
    std::cout << ui_text("Selected Data Root: ", "当前数据目录：") << selected_data_root.string() << std::endl;
    std::cout << "========================================" << std::endl;
}

void run_heartbeat_test() {
    std::cout << ui_text("\n[Heartbeat Test]", "\n[心跳测试]") << std::endl;
    auto start = std::chrono::steady_clock::now();
    seastar::xtrader::redis_sync_client redis;
    if (!redis.connect("127.0.0.1", 6379)) {
        std::cout << ui_text("[FAIL] Cannot connect to Redis", "[FAIL] 无法连接 Redis") << std::endl;
        return;
    }
    auto latency = std::chrono::steady_clock::now() - start;
    std::cout << ui_text("[OK] Connected to Redis in ", "[OK] 已连接 Redis，耗时 ")
              << std::chrono::duration<double, std::milli>(latency).count()
              << " ms" << std::endl;

    // Test SET/GET
    bool set_ok = redis.set("xtrader:heartbeat:test", "ok");
    if (set_ok) {
        std::string val = redis.get("xtrader:heartbeat:test");
        std::cout << ui_text("[OK] SET/GET test: ", "[OK] SET/GET 测试：") << val << std::endl;
    } else {
        std::cout << ui_text("[FAIL] SET failed", "[FAIL] SET 失败") << std::endl;
    }

    redis.disconnect();
}

// Run receives the configured selection but deliberately does not start a driver.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
std::string run_backtest(const seastar::xtrader::backtest_config&) {
    return seastar::xtrader::cli::backtest_placeholder_result();
}

void open_backtest_menu(const seastar::xtrader::market_data_paths& paths) {
    seastar::xtrader::cli::run_backtest_menu(
        paths, std::cin, std::cout, std::cerr,
        [] (const seastar::xtrader::cli::backtest_selection& selection) {
            seastar::xtrader::backtest_config config;
            config.data_root = selection.data_root.string();
            config.data_files = selection.data_files;
            config.product_type = selection.product_directory.filename().string();
            config.strategy_id = selection.strategy_id;
            return run_backtest(config);
        });
}

void run_root_menu() {
    // Source selection changes browsing only, not import destinations or trading modes.
    // Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
    const seastar::xtrader::market_data_paths data_paths;
    auto data_root = data_paths.source_root();
    std::string import_source_path;
    seastar::xtrader::import_source_type import_source_type = seastar::xtrader::import_source_type::taobao_csv;
    seastar::xtrader::import_mode import_mode = seastar::xtrader::import_mode::incremental;
    std::string import_config_path;
    std::string import_timeframe;
    std::string import_destination_source;

    if (::isatty(STDIN_FILENO)) {
        seastar::xtrader::cli::open_market_config_selection();
    }

    while (true) {
        std::cout << "\n========== X-Trader CLI ==========" << std::endl;
        // B1: the general-purpose CLI cannot select a trading environment.
        // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
        std::cout << ui_text("Trading modes are unavailable in this CLI; separate entries are disabled.",
                             "本 CLI 不提供交易功能；独立交易入口仍已禁用。") << std::endl;
        std::cout << ui_text("Data source: ", "数据来源：") << data_root.filename().string() << std::endl;
        std::cout << ui_text("Data root: ", "数据目录：") << data_root.string() << std::endl;
        std::cout << ui_text("3) Import", "3) 导入行情数据") << std::endl;
        std::cout << ui_text("4) Data Brief", "4) 数据概览") << std::endl;
        std::cout << ui_text("5) Config Info", "5) 配置信息") << std::endl;
        std::cout << ui_text("6) Heartbeat Test", "6) 心跳测试") << std::endl;
        std::cout << ui_text("7) Select data source", "7) 选择数据来源") << std::endl;
        std::cout << ui_text("8) Backtest setup (Run is under construction)", "8) 回测设置（执行功能尚未完成）") << std::endl;
        std::cout << ui_text("9) Read-only CTP market capture", "9) CTP 只读行情采集") << std::endl;
        std::cout << ui_text("10) Initial CTP key", "10) 初始化 CTP 密钥和账号配置") << std::endl;
        std::cout << ui_text("11) Select CTP account / set alias", "11) 选择 CTP 账号 / 设置别名") << std::endl;
        std::cout << ui_text("12) Import legacy X-Trader INI", "12) 导入旧版 X-Trader INI") << std::endl;
        std::cout << ui_text("13) Import plaintext X-Trader INI", "13) 导入明文 X-Trader INI") << std::endl;
        std::cout << "14) Language / 语言" << std::endl;
        std::cout << ui_text("q) Quit", "q) 退出") << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << ui_text("Select: ", "选择：");

        std::string input;
        if (!std::getline(std::cin, input)) {
            return;
        }
        input = to_lower(input);

        if (input == "1" || input == "2") {
            std::cerr << ui_text("[ERROR] Trading is unavailable in this CLI; no operation was started.",
                                 "[ERROR] 本 CLI 不提供交易功能，未启动任何交易操作。") << std::endl;
        } else if (input == "3") {
            // Import menu
            while (true) {
                std::cout << ui_text("\n[Import Menu]", "\n[行情导入菜单]") << std::endl;
                std::cout << ui_text("  source_path: ", "  输入路径：")
                          << (import_source_path.empty() ? ui_text("(empty)", "（未设置）") : import_source_path) << std::endl;
                std::cout << ui_text("  source_type: ", "  输入格式：")
                          << (import_source_type == seastar::xtrader::import_source_type::taobao_csv ? "taobao_csv" : "ctp_clean_csv") << std::endl;
                std::cout << ui_text("  mode: ", "  导入模式：")
                          << (import_mode == seastar::xtrader::import_mode::incremental
                              ? ui_text("incremental", "增量") : ui_text("overwrite", "覆盖")) << std::endl;
                std::cout << ui_text("  config_path: ", "  配置文件：")
                          << (import_config_path.empty() ? ui_text("(default)", "（默认）") : import_config_path) << std::endl;
                std::cout << ui_text("  timeframe: ", "  数据周期：") << (import_timeframe.empty()
                    ? ui_text("(filename/config; bars default 1m)", "（由文件名/配置决定；K 线默认 1m）") : import_timeframe) << std::endl;
                std::cout << ui_text("  destination_source: ", "  目的来源：") << (import_destination_source.empty()
                    ? ui_text("(config; taobao_csv -> taobao, canonical CSV -> historic)",
                              "（由配置决定；淘宝 -> taobao，规范 CSV -> historic）") : import_destination_source) << std::endl;
                std::cout << ui_text("  1) Set source path", "  1) 设置输入路径") << std::endl;
                std::cout << ui_text("  2) Set source type", "  2) 设置输入格式") << std::endl;
                std::cout << ui_text("  3) Set import mode", "  3) 设置导入模式") << std::endl;
                std::cout << ui_text("  4) Set config path", "  4) 设置配置文件") << std::endl;
                std::cout << ui_text("  5) Run import", "  5) 执行导入") << std::endl;
                std::cout << ui_text("  6) Set timeframe (tick / 5s / 2h / 1d / 1week / ...)",
                                     "  6) 设置周期（tick / 5s / 2h / 1d / 1week / ...）") << std::endl;
                std::cout << ui_text("  7) Set destination source", "  7) 设置目的来源") << std::endl;
                std::cout << ui_text("  u) Up", "  u) 返回") << std::endl;
                std::cout << ui_text("Select: ", "选择：");

                std::getline(std::cin, input);
                input = to_lower(input);

                if (input == "1") {
                    std::cout << ui_text("Enter source path: ", "请输入输入文件或目录路径：");
                    std::getline(std::cin, import_source_path);
                    std::cout << ui_text("source_path set to ", "输入路径已设为 ") << import_source_path << std::endl;
                } else if (input == "2") {
                    std::cout << ui_text("Source types:\n  1) taobao_csv\n  2) ctp_clean_csv\nSelect: ",
                                         "输入格式：\n  1) taobao_csv（淘宝 CSV）\n  2) ctp_clean_csv（规范 CSV）\n选择：");
                    std::getline(std::cin, input);
                    if (input == "1" || input == "taobao" || input == "taobao_csv") {
                        import_source_type = seastar::xtrader::import_source_type::taobao_csv;
                        std::cout << ui_text("source_type set to taobao_csv", "输入格式已设为 taobao_csv") << std::endl;
                    } else if (input == "2" || input == "ctp" || input == "ctp_clean_csv") {
                        import_source_type = seastar::xtrader::import_source_type::ctp_clean_csv;
                        std::cout << ui_text("source_type set to ctp_clean_csv", "输入格式已设为 ctp_clean_csv") << std::endl;
                    } else {
                        std::cerr << ui_text("[ERROR] Unknown source type", "[ERROR] 无效的输入格式") << std::endl;
                    }
                } else if (input == "3") {
                    std::cout << ui_text("Import modes:\n  1) incremental\n  2) overwrite\nSelect: ",
                                         "导入模式：\n  1) incremental（增量）\n  2) overwrite（覆盖）\n选择：");
                    std::getline(std::cin, input);
                    if (input == "1" || input == "incremental" || input == "inc") {
                        import_mode = seastar::xtrader::import_mode::incremental;
                        std::cout << ui_text("import_mode set to incremental", "导入模式已设为增量") << std::endl;
                    } else if (input == "2" || input == "overwrite" || input == "full") {
                        import_mode = seastar::xtrader::import_mode::overwrite;
                        std::cout << ui_text("import_mode set to overwrite", "导入模式已设为覆盖") << std::endl;
                    } else {
                        std::cerr << ui_text("[ERROR] Unknown import mode", "[ERROR] 无效的导入模式") << std::endl;
                    }
                } else if (input == "4") {
                    std::cout << ui_text("Enter config path (empty for default): ", "请输入配置文件路径（留空使用默认）：");
                    std::getline(std::cin, import_config_path);
                    if (import_config_path.empty()) {
                        std::cout << ui_text("config_path reset to default", "配置文件已恢复默认") << std::endl;
                    } else {
                        std::cout << ui_text("config_path set to ", "配置文件已设为 ") << import_config_path << std::endl;
                    }
                } else if (input == "5") {
                    if (import_source_path.empty()) {
                        std::cerr << ui_text("[ERROR] source_path not set", "[ERROR] 尚未设置输入路径") << std::endl;
                        continue;
                    }
                    if (!std::filesystem::exists(import_source_path)) {
                        std::cerr << ui_text("[ERROR] source_path does not exist: ", "[ERROR] 输入路径不存在：") << import_source_path << std::endl;
                        continue;
                    }

                    seastar::xtrader::import_options opts;
                    opts.source_path = import_source_path;
                    opts.source_type = import_source_type;
                    opts.mode = import_mode;
                    opts.timeframe = import_timeframe;
                    opts.destination_source = import_destination_source;
                    if (!import_config_path.empty()) {
                        opts.config_path = import_config_path;
                    }

                    std::cout << ui_text("Starting import...", "开始导入...") << std::endl;
                    seastar::xtrader::data_importer importer;
                    auto result = importer.execute(opts);

                    std::cout << (result.success ? ui_text("[OK] Import succeeded", "[OK] 导入成功")
                                                 : ui_text("[ERROR] Import failed", "[ERROR] 导入失败")) << std::endl;
                    std::cout << ui_text("  Destination: ", "  目的目录：") << result.target_root.string() << std::endl;
                    std::cout << ui_text("  Files: ", "  文件统计：") << result.scanned_files << ui_text(" scanned, ", " 个已扫描，")
                              << result.success_files << ui_text(" success, ", " 个成功，")
                              << result.skipped_files << ui_text(" skipped, ", " 个跳过，")
                              << result.failed_files << ui_text(" failed", " 个失败") << std::endl;
                    std::cout << ui_text("  Rows: ", "  数据行：") << result.rows_added << ui_text(" added, ", " 行新增，")
                              << result.rows_overwritten << ui_text(" overwritten, ", " 行覆盖，")
                              << result.rows_skipped << ui_text(" skipped", " 行跳过") << std::endl;
                    if (!result.message.empty()) {
                        std::cout << ui_text("  Message: ", "  详细信息：") << result.message << std::endl;
                    }
                    for (const auto& report : result.file_reports) {
                        std::cout << "  " << report.source_file.string() << " [" << report.timeframe
                            << "]: " << report.status << " - " << report.message << std::endl;
                    }
                } else if (input == "6") {
                    std::cout << ui_text("Enter timeframe (empty for filename/config): ", "请输入周期（留空由文件名/配置决定）：");
                    std::string value;
                    std::getline(std::cin, value);
                    try {
                        import_timeframe = value.empty() ? value
                            : seastar::xtrader::market_data_paths::normalize_timeframe(value);
                    } catch (const std::invalid_argument& error) {
                        std::cerr << "[ERROR] " << error.what() << std::endl;
                    }
                } else if (input == "7") {
                    std::cout << ui_text("Enter source name (empty for config/default): ", "请输入目的来源名（留空使用配置/默认值）：");
                    std::string value;
                    std::getline(std::cin, value);
                    try {
                        if (!value.empty()) {
                            auto registered = data_paths;
                            try {
                                (void)registered.source_root(value);
                            } catch (const std::invalid_argument&) {
                                registered = seastar::xtrader::market_data_paths(data_paths.root(), {value});
                            }
                        }
                        import_destination_source = value;
                    } catch (const std::invalid_argument& error) {
                        std::cerr << "[ERROR] " << error.what() << std::endl;
                    }
                } else if (input == "u" || input == "back" || input == "up") {
                    break;
                } else {
                    std::cerr << ui_text("[ERROR] Unknown command", "[ERROR] 无效的命令") << std::endl;
                }
            }
        } else if (input == "4") {
            // Data brief
            std::cout << ui_text("\n[Data Brief]", "\n[数据概览]") << std::endl;
            std::cout << ui_text("Data root: ", "数据目录：") << data_root.string() << std::endl;
            seastar::xtrader::historical_data_manager manager(data_root);
            std::string warning;
            auto datasets = manager.scan_datasets(&warning);
            if (!warning.empty()) {
                std::cout << "[WARN] " << warning << std::endl;
            }
            std::cout << ui_text("Found ", "找到 ") << datasets.size() << ui_text(" datasets:", " 个数据集：") << std::endl;
            for (const auto& ds : datasets) {
                std::cout << "  " << ds.exchange << "/" << ds.symbol
                          << " " << ds.timeframe
                          << ui_text(" source=", " 来源=") << ds.source
                          << ui_text(" kind=", " 类型=") << (ds.kind == seastar::xtrader::market_data_kind::tick ? "tick" : ui_text("bar", "K线"));
                if (ds.layout_version == 2) {
                    std::cout << " (" << ds.files.size() << ui_text(" daily files)", " 个日文件)") << std::endl;
                } else {
                    std::cout << ui_text(" (legacy manifest)", "（旧版 manifest）") << std::endl;
                }
            }
        } else if (input == "5") {
            show_config_info(data_paths, data_root);
        } else if (input == "6") {
            run_heartbeat_test();
        } else if (input == "7") {
            seastar::xtrader::cli::select_data_source(
                data_paths, data_root, std::cin, std::cout, std::cerr);
        } else if (input == "8") {
            open_backtest_menu(data_paths);
        } else if (input == "9") {
            seastar::xtrader::cli::open_market_capture_menu();
        } else if (input == "10") {
            seastar::xtrader::cli::open_initial_ctp_key_menu();
        } else if (input == "11") {
            seastar::xtrader::cli::open_market_config_selection();
        } else if (input == "12") {
            seastar::xtrader::cli::open_legacy_import_menu();
        } else if (input == "13") {
            seastar::xtrader::cli::open_plaintext_import_menu();
        } else if (input == "14" || input == "language") {
            seastar::xtrader::cli::run_language_menu(std::cin, std::cout, std::cerr);
        } else if (input == "q" || input == "quit" || input == "exit") {
            std::cout << ui_text("Goodbye!", "再见！") << std::endl;
            break;
        } else {
            std::cout << ui_text("Invalid option", "无效的选项") << std::endl;
        }
    }
}

} // namespace

int main(int ac, char** av) {
    try {
        (void)seastar::xtrader::cli::current_ui_language();
    } catch (const std::invalid_argument& error) {
        std::cerr << "[ERROR] " << error.what() << std::endl;
        return 2;
    }
    std::cout << "========================================" << std::endl;
    std::cout << "X-Trader Seastar Trading Engine v"
              << seastar::xtrader::get_full_version() << std::endl;
    std::cout << ui_text("Build: ", "构建：") << seastar::xtrader::build_type
              << " | Git: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "========================================" << std::endl;

    seastar::app_template app;

    return app.run(ac, av, [] {
        return seastar::async([] {
            run_root_menu();
        });
    });
}
