// One account action menu: START connects; SHOW reveals fields without connecting.
// Source: user menu simplification request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include <seastar/xtrader/market_capture_config.hh>
#include "cli_language.hh"

#include <algorithm>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace seastar::xtrader::cli {

inline void print_market_login_info(
        const market_capture_config& config, std::ostream& output, bool show_secrets = false) {
    validate_market_capture_config(config);
    const auto secret = [show_secrets] (const std::string& value) -> std::string_view {
        if (value.empty()) { return ui_text("(not configured)", "（未配置）"); }
        return show_secrets ? std::string_view(value)
            : std::string_view(ui_text("[hidden; type SHOW to review]", "[已隐藏；输入 SHOW 查看]"));
    };
    output << ui_text("\n[CTP account review: no connection made]\n"
                      "These fields come from the decrypted .enc configuration, not the encryption key.\n"
                      "[Market: used for this MdApi login]\n",
                      "\n[CTP 账号核对：尚未连接]\n以下字段来自解密后的 .enc 配置，而不是加密密钥。\n"
                      "[行情：本次 MdApi 登录使用的字段]\n")
           << "market.market_front: " << std::quoted(config.market_front)
           << "\nmarket.broker_id: " << std::quoted(config.broker_id)
           << "\nmarket.user_id: " << std::quoted(config.user_id)
           << "\nmarket.password: " << std::quoted(secret(config.password))
           << "\nmarket.contracts:";
    if (config.contracts.empty()) {
        output << ui_text(" (not configured; you will be prompted before connecting)", "（未配置；连接前会提示输入）");
    } else {
        for (const auto& contract : config.contracts) {
            output << ' ' << contract.exchange << ':' << contract.instrument;
        }
    }
    if (config.trader) {
        const auto& trader = *config.trader;
        output << ui_text("\n[Trader: stored only; NOT used for this login; trading disabled]\n",
                          "\n[交易：仅存储，本次登录不使用；交易已禁用]\n")
               << "trader.trade_front: " << std::quoted(trader.trade_front)
               << "\ntrader.broker_id: " << std::quoted(trader.broker_id)
               << "\ntrader.user_id: " << std::quoted(trader.user_id)
               << "\ntrader.password: " << std::quoted(secret(trader.password))
               << "\ntrader.app_id: " << std::quoted(trader.app_id)
               << "\ntrader.auth_code: " << std::quoted(secret(trader.auth_code))
               << "\ntrader.user_product_info: " << std::quoted(trader.user_product_info);
    } else {
        output << ui_text("\n[Trader: not configured; trading disabled]", "\n[交易：未配置；交易已禁用]");
    }
    output << ui_text("\nDecryption succeeded; the server has NOT verified this account or environment.\n",
                      "\n解密成功；服务器尚未验证该账号或环境。\n")
           << std::flush;
}

inline bool run_market_login_menu(
        const market_capture_config& config, std::istream& input, std::ostream& output,
        bool interactive_terminal) {
    validate_market_capture_config(config);
    if (!interactive_terminal) {
        print_market_login_info(config, output);
        output << ui_text("[INFO] Non-interactive capture: secrets stay hidden; no plaintext review is available.\n",
                          "[INFO] 非交互采集：凭据保持隐藏，不提供明文核对。\n");
        return true;
    }
    std::string command;
    while (true) {
        output << ui_text("\n[CTP Account Actions - not connected]\n", "\n[CTP 账号操作 - 尚未连接]\n")
               << ui_text("Market front: ", "行情前置：") << std::quoted(config.market_front)
               << "\nBrokerID: " << std::quoted(config.broker_id)
               << "  UserID: " << std::quoted(config.user_id)
               << ui_text("\nContracts: ", "\n合约数：") << config.contracts.size();
        if (config.contracts.empty()) { output << ui_text(" (enter contracts after START)", "（选择 START 后输入合约）"); }
        output << ui_text("\n1) START - Start read-only market capture\n"
                          "2) SHOW  - Show account details and full passwords (no connection)\n"
                          "u) Back\nSelect [1/START, 2/SHOW, u]: ",
                          "\n1) START - 启动只读行情采集\n2) SHOW  - 查看账号详情和完整密码（不连接）\n"
                          "u) 返回\n选择 [1/START, 2/SHOW, u]：") << std::flush;
        if (!std::getline(input, command)) { command.clear(); }
        std::transform(command.begin(), command.end(), command.begin(), [] (unsigned char ch) {
            return static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
        });
        if (command == "1" || command == "START") { return true; }
        if (command == "2" || command == "SHOW") {
            output << ui_text("[WARN] Full passwords and AuthCode follow; terminal scrollback or recording "
                              "may retain them. Do not share this screen.\n",
                              "[WARN] 接下来显示完整密码和 AuthCode；终端回滚或录屏可能保存这些内容，请勿分享此画面。\n");
            print_market_login_info(config, output, true);
        } else if (command.empty() || command == "U" || command == "BACK" || command == "0") {
            output << ui_text("[CANCELLED] Account actions closed; no connection made.\n",
                              "[CANCELLED] 已关闭账号操作，未连接服务器。\n") << std::flush;
            return false;
        } else {
            output << ui_text("[ERROR] Unknown account action; choose 1/START, 2/SHOW or u.\n",
                              "[ERROR] 无效的账号操作，请选择 1/START、2/SHOW 或 u。\n");
        }
    }
}

} // namespace seastar::xtrader::cli
