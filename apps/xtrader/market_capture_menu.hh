// Menu passes file paths only; credentials are decrypted solely in the MD worker.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <filesystem>
#include <exception>
#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace seastar::xtrader::cli {

inline void run_market_capture_menu(
        std::filesystem::path config, std::filesystem::path key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& launch) {
    std::string line;
    while (true) {
        output << "\n[Read-only CTP Market Capture]\nEncrypted config: " << config.string()
               << "\nKey file: " << key.string()
               << "\n1) Set encrypted config path\n2) Set key file path\n"
                  "3) Start live market capture (no trading)\nu) Back\nSelect: " << std::flush;
        if (!std::getline(input, line) || line == "u" || line == "back") { return; }
        if (line == "1" || line == "2") {
            const bool is_config = line == "1";
            output << "File path (Enter keeps current): " << std::flush;
            if (!std::getline(input, line)) { return; }
            if (!line.empty()) { (is_config ? config : key) = line; }
        } else if (line == "3") {
            output << "This connects to the configured market front and writes real Tick data.\n"
                      "Type START to confirm (Enter cancels): " << std::flush;
            if (!std::getline(input, line)) { return; }
            if (line != "START") { continue; }
            try {
                const auto status = launch(config, key);
                if (status != 0) {
                    errors << "[ERROR] Market collector exited with status " << status << ".\n";
                } else {
                    output << "[STOPPED] Market collector returned; inspect its subscription and tick counts.\n";
                }
            } catch (const std::exception& error) {
                errors << "[ERROR] Cannot launch market collector: " << error.what() << '\n';
            }
        } else {
            errors << "[ERROR] Unknown market capture menu option.\n";
        }
    }
}

void open_market_capture_menu();

} // namespace seastar::xtrader::cli
