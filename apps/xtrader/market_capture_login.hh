// One account action menu: START connects; SHOW reveals fields without connecting.
// Source: user menu simplification request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include <seastar/xtrader/market_capture_config.hh>

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
        if (value.empty()) { return "(not configured)"; }
        return show_secrets ? std::string_view(value) : std::string_view("[hidden; type SHOW to review]");
    };
    output << "\n[CTP account review: no connection made]\n"
              "These fields come from the decrypted .enc configuration, not the encryption key.\n"
              "[Market: used for this MdApi login]\n"
           << "market.market_front: " << std::quoted(config.market_front)
           << "\nmarket.broker_id: " << std::quoted(config.broker_id)
           << "\nmarket.user_id: " << std::quoted(config.user_id)
           << "\nmarket.password: " << std::quoted(secret(config.password))
           << "\nmarket.contracts:";
    if (config.contracts.empty()) {
        output << " (not configured; you will be prompted before connecting)";
    } else {
        for (const auto& contract : config.contracts) {
            output << ' ' << contract.exchange << ':' << contract.instrument;
        }
    }
    if (config.trader) {
        const auto& trader = *config.trader;
        output << "\n[Trader: stored only; NOT used for this login; trading disabled]\n"
               << "trader.trade_front: " << std::quoted(trader.trade_front)
               << "\ntrader.broker_id: " << std::quoted(trader.broker_id)
               << "\ntrader.user_id: " << std::quoted(trader.user_id)
               << "\ntrader.password: " << std::quoted(secret(trader.password))
               << "\ntrader.app_id: " << std::quoted(trader.app_id)
               << "\ntrader.auth_code: " << std::quoted(secret(trader.auth_code))
               << "\ntrader.user_product_info: " << std::quoted(trader.user_product_info);
    } else {
        output << "\n[Trader: not configured; trading disabled]";
    }
    output << "\nDecryption succeeded; the server has NOT verified this account or environment.\n"
           << std::flush;
}

inline bool run_market_login_menu(
        const market_capture_config& config, std::istream& input, std::ostream& output,
        bool interactive_terminal) {
    validate_market_capture_config(config);
    if (!interactive_terminal) {
        print_market_login_info(config, output);
        output << "[INFO] Non-interactive capture: secrets stay hidden; no plaintext review is available.\n";
        return true;
    }
    std::string command;
    while (true) {
        output << "\n[CTP Account Actions - not connected]\n"
               << "Market front: " << std::quoted(config.market_front)
               << "\nBrokerID: " << std::quoted(config.broker_id)
               << "  UserID: " << std::quoted(config.user_id)
               << "\nContracts: " << config.contracts.size();
        if (config.contracts.empty()) { output << " (enter contracts after START)"; }
        output << "\n1) START - Start read-only market capture\n"
                  "2) SHOW  - Show account details and full passwords (no connection)\n"
                  "u) Back\nSelect [1/START, 2/SHOW, u]: " << std::flush;
        if (!std::getline(input, command)) { command.clear(); }
        std::transform(command.begin(), command.end(), command.begin(), [] (unsigned char ch) {
            return static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
        });
        if (command == "1" || command == "START") { return true; }
        if (command == "2" || command == "SHOW") {
            output << "[WARN] Full passwords and AuthCode follow; terminal scrollback or recording "
                      "may retain them. Do not share this screen.\n";
            print_market_login_info(config, output, true);
        } else if (command.empty() || command == "U" || command == "BACK" || command == "0") {
            output << "[CANCELLED] Account actions closed; no connection made.\n" << std::flush;
            return false;
        } else {
            output << "[ERROR] Unknown account action; choose 1/START, 2/SHOW or u.\n";
        }
    }
}

} // namespace seastar::xtrader::cli
