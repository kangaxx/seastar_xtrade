// Menu passes file paths only; credentials are decrypted solely in the MD worker.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include "market_profile.hh"
#include "cli_language.hh"

#include <filesystem>
#include <algorithm>
#include <charconv>
#include <exception>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace seastar::xtrader::cli {

inline void select_market_config(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors);

inline void run_market_capture_menu(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& launch) {
    std::string line;
    while (true) {
        output << ui_text("\n[Read-only CTP Market Capture]\n", "\n[CTP 只读行情采集]\n")
               << ui_text("A configuration without contracts will prompt for them before connecting.\n",
                          "若配置中没有合约，会在连接前提示输入。\n")
               << ui_text("Account: ", "账号：") << market_profile_label(config, errors) << '\n'
               << ui_text("Encrypted config: ", "加密配置：") << config.string()
               << ui_text("\nKey file: ", "\n密钥文件：") << key.string()
               << ui_text("\n1) Set encrypted config path\n2) Set key file path\n"
                          "3) Open account actions (START / SHOW; no trading)\n"
                          "4) Select imported account\n5) Set account alias\nu) Back\nSelect: ",
                          "\n1) 设置加密配置路径\n2) 设置密钥文件路径\n"
                          "3) 打开账号操作（START / SHOW；不交易）\n"
                          "4) 选择已导入账号\n5) 设置账号别名\nu) 返回\n选择：") << std::flush;
        if (!std::getline(input, line) || line == "u" || line == "back") { return; }
        if (line == "1" || line == "2") {
            const bool is_config = line == "1";
            output << ui_text("File path (Enter keeps current): ", "文件路径（回车保持当前）：") << std::flush;
            if (!std::getline(input, line)) { return; }
            if (!line.empty()) { (is_config ? config : key) = line; }
        } else if (line == "4") {
            select_market_config(config, key, input, output, errors);
        } else if (line == "5") {
            run_market_profile_alias_menu(config, key, input, output, errors);
        } else if (line == "3") {
            output << ui_text("Selected account: ", "所选账号：") << market_profile_label(config, errors)
                   << ui_text("\nEncrypted config: ", "\n加密配置：") << config.string()
                   << ui_text("\nKey file: ", "\n密钥文件：") << key.string()
                   << ui_text("\nOpening account actions; only START connects, SHOW does not.\n",
                              "\n打开账号操作；只有 START 会连接，SHOW 不会连接。\n") << std::flush;
            try {
                const auto status = launch(config, key);
                if (status != 0) {
                    errors << ui_text("[ERROR] Market collector exited with status ", "[ERROR] 行情采集器退出，状态码 ") << status << ".\n";
                } else {
                    output << ui_text("[INFO] Returned to the market capture menu.\n", "[INFO] 已返回行情采集菜单。\n");
                }
            } catch (const std::exception& error) {
                errors << ui_text("[ERROR] Cannot launch market collector: ", "[ERROR] 无法启动行情采集器：") << error.what() << '\n';
            }
        } else {
            errors << ui_text("[ERROR] Unknown market capture menu option.\n", "[ERROR] 无效的行情采集菜单选项。\n");
        }
    }
}

void open_market_capture_menu();

// Paths are adopted by the current CLI session only after initialization succeeds.
// Source: Initial CTP key request, 2026-11-13. Signature: GitHub Copilot (Auto).
inline void run_initial_ctp_key_menu(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& initialize) {
    auto selected_config = config;
    auto selected_key = key;
    std::string line;
    output << ui_text("\n[Initial CTP key]\nCreate an encrypted market configuration; no CTP connection.\n"
                      "Use a .enc config and keep its key outside Git. Existing files are never overwritten.\n"
                      "Encrypted config file [",
                      "\n[初始化 CTP 密钥]\n创建加密行情配置，不连接 CTP。\n"
                      "请使用 .enc 配置，并将密钥保存在 Git 之外。不会覆盖已有文件。\n加密配置文件 [")
           << config.string() << ui_text("] (Enter keeps current, u cancels): ", "]（回车保持当前，u 取消）：") << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_config = line; }
    output << ui_text("Key file [", "密钥文件 [") << key.string()
           << ui_text("] (Enter keeps current, u cancels): ", "]（回车保持当前，u 取消）：") << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_key = line; }
    output << ui_text("You will enter market front (IP/host and port), BrokerID, UserID, hidden password and contracts.\n"
                      "An existing key is reused, not replaced. Type INITIALIZE to continue: ",
                      "接下来输入行情前置（IP/主机和端口）、BrokerID、UserID、隐藏密码和合约。\n"
                      "已有密钥会复用，不会替换。输入 INITIALIZE 继续：") << std::flush;
    if (!std::getline(input, line) || line != "INITIALIZE") { return; }
    try {
        const auto status = initialize(selected_config, selected_key);
        if (status != 0) {
            errors << ui_text("[ERROR] CTP initialization exited with status ", "[ERROR] CTP 初始化退出，状态码 ") << status
                   << ui_text("; selected paths unchanged.\n", "；所选路径未改变。\n");
            return;
        }
        config = std::move(selected_config);
        key = std::move(selected_key);
        output << ui_text("[OK] Encrypted CTP configuration initialized. Menu 9 uses these paths in this session.\n",
                          "[OK] 已初始化加密 CTP 配置。本次会话的菜单 9 将使用这些路径。\n");
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << ui_text("[ERROR] Cannot initialize CTP configuration: ", "[ERROR] 无法初始化 CTP 配置：") << error.what() << '\n';
    }
}

void open_initial_ctp_key_menu();

inline void select_market_config(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors) {
    std::vector<std::filesystem::path> choices;
    try {
        choices = list_market_configs(config);
    } catch (const std::exception& error) {
        errors << ui_text("[ERROR] Cannot list encrypted configurations: ", "[ERROR] 无法列出加密配置：") << error.what() << '\n';
        return;
    }
    output << ui_text("\n[Select imported CTP account]\nCurrent account: ", "\n[选择已导入 CTP 账号]\n当前账号：")
           << market_profile_label(config, errors)
           << ui_text("\nCurrent: ", "\n当前配置：") << config.string() << ui_text("\nKey: ", "\n密钥：") << key.string()
           << ui_text("\n0) Keep current\n", "\n0) 保持当前\n");
    for (std::size_t i = 0; i < choices.size(); ++i) {
        output << i + 1 << ") " << market_profile_label(choices[i], errors)
               << " [" << choices[i].filename().string() << "]\n";
    }
    output << ui_text("p) Enter another config path\nr) Rename current account alias\n"
                      "Select (Enter keeps current, u cancels): ",
                      "p) 输入其他配置路径\nr) 修改当前账号别名\n选择（回车保持当前，u 取消）：") << std::flush;
    std::string line;
    if (!std::getline(input, line) || line.empty() || line == "0" || line == "u") { return; }
    if (line == "r") {
        run_market_profile_alias_menu(config, key, input, output, errors);
        return;
    }
    auto selected = config;
    if (line == "p") {
        output << ui_text("Encrypted .enc path (u cancels): ", "加密 .enc 配置路径（u 取消）：") << std::flush;
        if (!std::getline(input, line) || line.empty() || line == "u") { return; }
        selected = line;
    } else {
        std::size_t index = 0;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), index);
        if (result.ec != std::errc{} || result.ptr != line.data() + line.size()
                || index == 0 || index > choices.size()) {
            errors << ui_text("[ERROR] Invalid configuration selection; current paths unchanged.\n",
                              "[ERROR] 配置选择无效，当前路径未改变。\n");
            return;
        }
        selected = choices[index - 1];
    }
    if (selected.extension() != ".enc") {
        errors << ui_text("[ERROR] Select an .enc configuration; current paths unchanged.\n",
                          "[ERROR] 请选择 .enc 配置，当前路径未改变。\n");
        return;
    }
    try {
        const auto profile = load_market_profile(selected);
        auto selected_key = key;
        if (profile) {
            selected_key = profile->key;
        } else {
            output << ui_text("Key file [", "密钥文件 [") << key.string()
                   << ui_text("] (Enter keeps current, u cancels): ", "]（回车保持当前，u 取消）：") << std::flush;
            if (!std::getline(input, line) || line == "u") { return; }
            if (!line.empty()) { selected_key = line; }
        }
        if (!std::filesystem::is_regular_file(selected)
                || !std::filesystem::is_regular_file(selected_key)
                || std::filesystem::equivalent(selected, selected_key)) {
            throw std::runtime_error(ui_text("configuration and key must be existing, separate files",
                                            "配置和密钥必须是两个不同的已存在文件"));
        }
        config = std::move(selected);
        key = std::move(selected_key);
        output << ui_text("[OK] Account selected: ", "[OK] 已选择账号：") << market_profile_label(config, errors)
               << ui_text(". Menu 9 uses this configuration and key.\n"
                          "[INFO] Local account selection only; login has NOT been attempted. This is not a login failure.\n"
                          "[INFO] Use Menu 9 -> 3 for START (connect) or SHOW (view account details).\n",
                          "。菜单 9 将使用该配置和密钥。\n"
                          "[INFO] 这里只选择了本地账号，尚未尝试登录；这不是登录失败。\n"
                          "[INFO] 通过菜单 9 -> 3 选择 START（连接）或 SHOW（查看账号详情）。\n");
    } catch (const std::exception& error) {
        errors << ui_text("[ERROR] Cannot select account; current paths unchanged: ", "[ERROR] 无法选择账号，当前路径未改变：") << error.what() << '\n';
    }
}

inline void run_legacy_import_menu(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& importer) {
    auto selected_config = config;
    auto selected_key = key;
    std::string line;
    output << ui_text("\n[Import legacy X-Trader INI]\n"
                      "Create a new encrypted configuration; existing files are not overwritten.\nEncrypted config file [",
                      "\n[导入旧版 X-Trader INI]\n创建新的加密配置，不会覆盖已有文件。\n加密配置文件 [")
           << config.string() << ui_text("] (Enter keeps current, u cancels): ", "]（回车保持当前，u 取消）：") << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_config = line; }
    output << ui_text("Key file [", "密钥文件 [") << key.string()
           << ui_text("] (Enter keeps current, u cancels): ", "]（回车保持当前，u 取消）：") << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_key = line; }
    output << ui_text("Type IMPORT to continue to legacy INI and secret entry: ",
                      "输入 IMPORT，继续填写旧版 INI 和解密口令：") << std::flush;
    if (!std::getline(input, line) || line != "IMPORT") { return; }
    try {
        const auto status = importer(selected_config, selected_key);
        if (status != 0) {
            errors << ui_text("[ERROR] Legacy import exited with status ", "[ERROR] 旧版导入退出，状态码 ") << status
                   << ui_text("; selected paths unchanged.\n", "；所选路径未改变。\n");
            return;
        }
        config = std::move(selected_config);
        key = std::move(selected_key);
        output << ui_text("[OK] Imported configuration selected for this CLI session.\n", "[OK] 已为本次会话选中导入的配置。\n");
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << ui_text("[ERROR] Cannot import legacy configuration: ", "[ERROR] 无法导入旧版配置：") << error.what() << '\n';
    }
}

void open_market_config_selection();
void open_legacy_import_menu();

inline void run_plaintext_import_menu(
        std::filesystem::path& config, const std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<std::filesystem::path(const std::filesystem::path&)>& importer) {
    output << ui_text("\n[Import plaintext X-Trader INI]\n"
                      "The source contains credentials. It must be a readable regular file.\n"
                      "Enter the complete plaintext INI path (u cancels): ",
                      "\n[导入明文 X-Trader INI]\n源文件含有凭据，必须是可读取的普通文件。\n"
                      "请输入明文 INI 的完整路径（u 取消）：") << std::flush;
    std::string line;
    if (!std::getline(input, line) || line.empty() || line == "u") { return; }
    try {
        auto imported = importer(std::filesystem::path(line));
        if (imported.empty()) {
            errors << ui_text("[ERROR] Plaintext INI import did not produce an encrypted configuration.\n",
                              "[ERROR] 明文 INI 导入未生成加密配置。\n");
            return;
        }
        config = std::move(imported);
        output << ui_text("[OK] Encrypted configuration created and selected for this CLI session.\n",
                          "[OK] 已创建加密配置，并为本次会话选中。\n");
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << ui_text("[ERROR] Cannot import plaintext INI: ", "[ERROR] 无法导入明文 INI：") << error.what() << '\n';
    }
}

void open_plaintext_import_menu();

} // namespace seastar::xtrader::cli
