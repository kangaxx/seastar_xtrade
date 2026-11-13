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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

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
            std::cerr << "[ERROR] Cannot inspect configuration root: " << ec.message() << std::endl;
            inventory.is_complete = false;
        }
        return inventory;
    }
    if (!std::filesystem::is_directory(root, ec)) {
        std::cerr << "[ERROR] Configuration root is not an accessible directory";
        if (ec) {
            std::cerr << ": " << ec.message();
        }
        std::cerr << std::endl;
        inventory.is_complete = false;
        return inventory;
    }

    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
        std::cerr << "[ERROR] Cannot list configuration root: " << ec.message() << std::endl;
        inventory.is_complete = false;
        return inventory;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const auto& entry = *it;
        const bool is_dir = entry.is_directory(ec);
        if (ec) {
            std::cerr << "[ERROR] Cannot inspect configuration directory: " << ec.message() << std::endl;
            inventory.is_complete = false;
            ec.clear();
        } else if (is_dir) {
            const auto ini_path = entry.path() / "xtrader.ini";
            if (std::filesystem::is_regular_file(ini_path, ec)) {
                inventory.envs.push_back(entry.path());
            } else if (ec && ec != std::errc::no_such_file_or_directory) {
                std::cerr << "[ERROR] Cannot inspect account configuration: " << ec.message() << std::endl;
                inventory.is_complete = false;
            }
            ec.clear();
        }
        it.increment(ec);
        if (ec) {
            std::cerr << "[ERROR] Cannot continue listing configuration root: " << ec.message() << std::endl;
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
        std::cerr << "[ERROR] Cannot read account configuration #" << index << std::endl;
    }
    std::cout << "  [" << environment << "] configuration #" << index
              << " (unverified; account details hidden)" << std::endl;
}

// ============================================================
// show_config_info（重构版）
// ============================================================
void show_config_info(const seastar::xtrader::market_data_paths& paths,
                      const std::filesystem::path& selected_data_root) {
    std::cout << "\n========== Configuration Info ==========" << std::endl;
    std::cout << "Version:    " << seastar::xtrader::version << std::endl;
    std::cout << "Build:      " << seastar::xtrader::build_type << std::endl;
    std::cout << "Git Commit: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "Git Branch: " << seastar::xtrader::git_branch << std::endl;
    std::cout << "Build Time: " << seastar::xtrader::build_timestamp << std::endl;
    std::cout << "========================================" << std::endl;

    const auto root = config_root();
    const auto inventory = scan_env_dirs(root);

    std::cout << "\n--- Account Environments ---" << std::endl;
    std::cout << "Config Root: " << root.string() << std::endl;

    if (!inventory.is_complete) {
        std::cout << "[ERROR] Configuration inventory is incomplete" << std::endl;
    } else if (inventory.envs.empty()) {
        std::cout << "(no environments found)" << std::endl;
    }
    for (size_t index = 0; index < inventory.envs.size(); ++index) {
        print_env_summary(inventory.envs[index], index + 1);
    }
    std::cout << "Found: " << inventory.envs.size() << " environment(s)" << std::endl;
    std::cout << "Account configuration creation is disabled until secure credential management is available."
              << std::endl;

    // 数据目录
    std::cout << "\n--- Data Directories ---" << std::endl;
    std::cout << "Default Data Root: " << paths.source_root().string() << std::endl;
    std::cout << "Selected Data Root: " << selected_data_root.string() << std::endl;
    std::cout << "========================================" << std::endl;
}

void run_heartbeat_test() {
    std::cout << "\n[Heartbeat Test]" << std::endl;
    auto start = std::chrono::steady_clock::now();
    seastar::xtrader::redis_sync_client redis;
    if (!redis.connect("127.0.0.1", 6379)) {
        std::cout << "[FAIL] Cannot connect to Redis" << std::endl;
        return;
    }
    auto latency = std::chrono::steady_clock::now() - start;
    std::cout << "[OK] Connected to Redis in "
              << std::chrono::duration<double, std::milli>(latency).count()
              << " ms" << std::endl;

    // Test SET/GET
    bool set_ok = redis.set("xtrader:heartbeat:test", "ok");
    if (set_ok) {
        std::string val = redis.get("xtrader:heartbeat:test");
        std::cout << "[OK] SET/GET test: " << val << std::endl;
    } else {
        std::cout << "[FAIL] SET failed" << std::endl;
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

    while (true) {
        std::cout << "\n========== X-Trader CLI ==========" << std::endl;
        // B1: the general-purpose CLI cannot select a trading environment.
        // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
        std::cout << "Trading modes are unavailable in this CLI; separate entries are disabled." << std::endl;
        std::cout << "Data source: " << data_root.filename().string() << std::endl;
        std::cout << "Data root: " << data_root.string() << std::endl;
        std::cout << "3) Import" << std::endl;
        std::cout << "4) Data Brief" << std::endl;
        std::cout << "5) Config Info" << std::endl;
        std::cout << "6) Heartbeat Test" << std::endl;
        std::cout << "7) Select data source" << std::endl;
        std::cout << "8) Backtest setup (Run is under construction)" << std::endl;
        std::cout << "q) Quit" << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << "Select: ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            return;
        }
        input = to_lower(input);

        if (input == "1" || input == "2") {
            std::cerr << "[ERROR] Trading is unavailable in this CLI; no operation was started." << std::endl;
        } else if (input == "3") {
            // Import menu
            while (true) {
                std::cout << "\n[Import Menu]" << std::endl;
                std::cout << "  source_path: " << (import_source_path.empty() ? "(empty)" : import_source_path) << std::endl;
                std::cout << "  source_type: " << (import_source_type == seastar::xtrader::import_source_type::taobao_csv ? "taobao_csv" : "ctp_clean_csv") << std::endl;
                std::cout << "  mode: " << (import_mode == seastar::xtrader::import_mode::incremental ? "incremental" : "overwrite") << std::endl;
                std::cout << "  config_path: " << (import_config_path.empty() ? "(default)" : import_config_path) << std::endl;
                std::cout << "  1) Set source path" << std::endl;
                std::cout << "  2) Set source type" << std::endl;
                std::cout << "  3) Set import mode" << std::endl;
                std::cout << "  4) Set config path" << std::endl;
                std::cout << "  5) Run import" << std::endl;
                std::cout << "  u) Up" << std::endl;
                std::cout << "Select: ";

                std::getline(std::cin, input);
                input = to_lower(input);

                if (input == "1") {
                    std::cout << "Enter source path: ";
                    std::getline(std::cin, import_source_path);
                    std::cout << "source_path set to " << import_source_path << std::endl;
                } else if (input == "2") {
                    std::cout << "Source types:\n  1) taobao_csv\n  2) ctp_clean_csv\nSelect: ";
                    std::getline(std::cin, input);
                    if (input == "1" || input == "taobao" || input == "taobao_csv") {
                        import_source_type = seastar::xtrader::import_source_type::taobao_csv;
                        std::cout << "source_type set to taobao_csv" << std::endl;
                    } else if (input == "2" || input == "ctp" || input == "ctp_clean_csv") {
                        import_source_type = seastar::xtrader::import_source_type::ctp_clean_csv;
                        std::cout << "source_type set to ctp_clean_csv" << std::endl;
                    } else {
                        std::cerr << "[ERROR] Unknown source type" << std::endl;
                    }
                } else if (input == "3") {
                    std::cout << "Import modes:\n  1) incremental\n  2) overwrite\nSelect: ";
                    std::getline(std::cin, input);
                    if (input == "1" || input == "incremental" || input == "inc") {
                        import_mode = seastar::xtrader::import_mode::incremental;
                        std::cout << "import_mode set to incremental" << std::endl;
                    } else if (input == "2" || input == "overwrite" || input == "full") {
                        import_mode = seastar::xtrader::import_mode::overwrite;
                        std::cout << "import_mode set to overwrite" << std::endl;
                    } else {
                        std::cerr << "[ERROR] Unknown import mode" << std::endl;
                    }
                } else if (input == "4") {
                    std::cout << "Enter config path (empty for default): ";
                    std::getline(std::cin, import_config_path);
                    if (import_config_path.empty()) {
                        std::cout << "config_path reset to default" << std::endl;
                    } else {
                        std::cout << "config_path set to " << import_config_path << std::endl;
                    }
                } else if (input == "5") {
                    if (import_source_path.empty()) {
                        std::cerr << "[ERROR] source_path not set" << std::endl;
                        continue;
                    }
                    if (!std::filesystem::exists(import_source_path)) {
                        std::cerr << "[ERROR] source_path does not exist: " << import_source_path << std::endl;
                        continue;
                    }

                    seastar::xtrader::import_options opts;
                    opts.source_path = import_source_path;
                    opts.source_type = import_source_type;
                    opts.mode = import_mode;
                    if (!import_config_path.empty()) {
                        opts.config_path = import_config_path;
                    }

                    std::cout << "Starting import..." << std::endl;
                    seastar::xtrader::data_importer importer;
                    auto result = importer.execute(opts);

                    std::cout << "[OK] Import " << (result.success ? "succeeded" : "failed") << std::endl;
                    std::cout << "  Files: " << result.scanned_files << " scanned, "
                              << result.success_files << " success, "
                              << result.skipped_files << " skipped, "
                              << result.failed_files << " failed" << std::endl;
                    std::cout << "  Rows: " << result.rows_added << " added, "
                              << result.rows_overwritten << " overwritten, "
                              << result.rows_skipped << " skipped" << std::endl;
                    if (!result.message.empty()) {
                        std::cout << "  Message: " << result.message << std::endl;
                    }
                } else if (input == "u" || input == "back" || input == "up") {
                    break;
                } else {
                    std::cerr << "[ERROR] Unknown command" << std::endl;
                }
            }
        } else if (input == "4") {
            // Data brief
            std::cout << "\n[Data Brief]" << std::endl;
            std::cout << "Data root: " << data_root.string() << std::endl;
            seastar::xtrader::historical_data_manager manager(data_root);
            std::string warning;
            auto datasets = manager.scan_datasets(&warning);
            if (!warning.empty()) {
                std::cout << "[WARN] " << warning << std::endl;
            }
            std::cout << "Found " << datasets.size() << " datasets:" << std::endl;
            for (const auto& ds : datasets) {
                std::cout << "  " << ds.exchange << "/" << ds.symbol
                          << " " << ds.timeframe
                          << " (" << ds.row_count << " records)" << std::endl;
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
        } else if (input == "q" || input == "quit" || input == "exit") {
            std::cout << "Goodbye!" << std::endl;
            break;
        } else {
            std::cout << "Invalid option" << std::endl;
        }
    }
}

} // namespace

int main(int ac, char** av) {
    std::cout << "========================================" << std::endl;
    std::cout << "X-Trader Seastar Trading Engine v"
              << seastar::xtrader::version << std::endl;
    std::cout << "Build: " << seastar::xtrader::build_type
              << " | Git: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "========================================" << std::endl;

    seastar::app_template app;

    return app.run(ac, av, [] {
        return seastar::async([] {
            run_root_menu();
        });
    });
}
