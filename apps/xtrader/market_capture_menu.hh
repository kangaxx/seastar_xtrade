// Menu passes file paths only; credentials are decrypted solely in the MD worker.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include "market_profile.hh"

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
        output << "\n[Read-only CTP Market Capture]\n"
               << "A configuration without contracts will prompt for them before connecting.\n"
               << "Account: " << market_profile_label(config, errors) << '\n'
               << "Encrypted config: " << config.string()
               << "\nKey file: " << key.string()
               << "\n1) Set encrypted config path\n2) Set key file path\n"
                  "3) Start live market capture (no trading)\n"
                  "4) Select imported account\n5) Set account alias\nu) Back\nSelect: " << std::flush;
        if (!std::getline(input, line) || line == "u" || line == "back") { return; }
        if (line == "1" || line == "2") {
            const bool is_config = line == "1";
            output << "File path (Enter keeps current): " << std::flush;
            if (!std::getline(input, line)) { return; }
            if (!line.empty()) { (is_config ? config : key) = line; }
        } else if (line == "4") {
            select_market_config(config, key, input, output, errors);
        } else if (line == "5") {
            run_market_profile_alias_menu(config, key, input, output, errors);
        } else if (line == "3") {
            output << "Selected account: " << market_profile_label(config, errors)
                   << "\nEncrypted config: " << config.string() << "\nKey file: " << key.string()
                   << "\nThis connects to the configured market front and writes real Tick data.\n"
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

// Paths are adopted by the current CLI session only after initialization succeeds.
// Source: Initial CTP key request, 2026-11-13. Signature: GitHub Copilot (Auto).
inline void run_initial_ctp_key_menu(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& initialize) {
    auto selected_config = config;
    auto selected_key = key;
    std::string line;
    output << "\n[Initial CTP key]\nCreate an encrypted market configuration; no CTP connection.\n"
              "Use a .enc config and keep its key outside Git. Existing files are never overwritten.\n"
              "Encrypted config file [" << config.string() << "] (Enter keeps current, u cancels): " << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_config = line; }
    output << "Key file [" << key.string() << "] (Enter keeps current, u cancels): " << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_key = line; }
    output << "You will enter market front (IP/host and port), BrokerID, UserID, hidden password and contracts.\n"
              "An existing key is reused, not replaced. Type INITIALIZE to continue: " << std::flush;
    if (!std::getline(input, line) || line != "INITIALIZE") { return; }
    try {
        const auto status = initialize(selected_config, selected_key);
        if (status != 0) {
            errors << "[ERROR] CTP initialization exited with status " << status << "; selected paths unchanged.\n";
            return;
        }
        config = std::move(selected_config);
        key = std::move(selected_key);
        output << "[OK] Encrypted CTP configuration initialized. Menu 9 uses these paths in this session.\n";
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot initialize CTP configuration: " << error.what() << '\n';
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
        errors << "[ERROR] Cannot list encrypted configurations: " << error.what() << '\n';
        return;
    }
    output << "\n[Select imported CTP account]\nCurrent account: " << market_profile_label(config, errors)
           << "\nCurrent: " << config.string() << "\nKey: " << key.string() << "\n0) Keep current\n";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        output << i + 1 << ") " << market_profile_label(choices[i], errors)
               << " [" << choices[i].filename().string() << "]\n";
    }
    output << "p) Enter another config path\nr) Rename current account alias\n"
              "Select (Enter keeps current, u cancels): " << std::flush;
    std::string line;
    if (!std::getline(input, line) || line.empty() || line == "0" || line == "u") { return; }
    if (line == "r") {
        run_market_profile_alias_menu(config, key, input, output, errors);
        return;
    }
    auto selected = config;
    if (line == "p") {
        output << "Encrypted .enc path (u cancels): " << std::flush;
        if (!std::getline(input, line) || line.empty() || line == "u") { return; }
        selected = line;
    } else {
        std::size_t index = 0;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), index);
        if (result.ec != std::errc{} || result.ptr != line.data() + line.size()
                || index == 0 || index > choices.size()) {
            errors << "[ERROR] Invalid configuration selection; current paths unchanged.\n";
            return;
        }
        selected = choices[index - 1];
    }
    if (selected.extension() != ".enc") {
        errors << "[ERROR] Select an .enc configuration; current paths unchanged.\n";
        return;
    }
    try {
        const auto profile = load_market_profile(selected);
        auto selected_key = key;
        if (profile) {
            selected_key = profile->key;
        } else {
            output << "Key file [" << key.string() << "] (Enter keeps current, u cancels): " << std::flush;
            if (!std::getline(input, line) || line == "u") { return; }
            if (!line.empty()) { selected_key = line; }
        }
        if (!std::filesystem::is_regular_file(selected)
                || !std::filesystem::is_regular_file(selected_key)
                || std::filesystem::equivalent(selected, selected_key)) {
            throw std::runtime_error("configuration and key must be existing, separate files");
        }
        config = std::move(selected);
        key = std::move(selected_key);
        output << "[OK] Account selected: " << market_profile_label(config, errors)
               << ". Menu 9 uses this configuration and key.\n"
                  "Identity and environment are not verified; no connection made.\n";
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot select account; current paths unchanged: " << error.what() << '\n';
    }
}

inline void run_legacy_import_menu(
        std::filesystem::path& config, std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<int(const std::filesystem::path&, const std::filesystem::path&)>& importer) {
    auto selected_config = config;
    auto selected_key = key;
    std::string line;
    output << "\n[Import legacy X-Trader INI]\n"
              "Create a new encrypted configuration; existing files are not overwritten.\n"
              "Encrypted config file [" << config.string() << "] (Enter keeps current, u cancels): " << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_config = line; }
    output << "Key file [" << key.string() << "] (Enter keeps current, u cancels): " << std::flush;
    if (!std::getline(input, line) || line == "u") { return; }
    if (!line.empty()) { selected_key = line; }
    output << "Type IMPORT to continue to legacy INI and secret entry: " << std::flush;
    if (!std::getline(input, line) || line != "IMPORT") { return; }
    try {
        const auto status = importer(selected_config, selected_key);
        if (status != 0) {
            errors << "[ERROR] Legacy import exited with status " << status << "; selected paths unchanged.\n";
            return;
        }
        config = std::move(selected_config);
        key = std::move(selected_key);
        output << "[OK] Imported configuration selected for this CLI session.\n";
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot import legacy configuration: " << error.what() << '\n';
    }
}

void open_market_config_selection();
void open_legacy_import_menu();

inline void run_plaintext_import_menu(
        std::filesystem::path& config, const std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        const std::function<std::filesystem::path(const std::filesystem::path&)>& importer) {
    output << "\n[Import plaintext X-Trader INI]\n"
              "The source contains credentials. It must be a readable regular file.\n"
              "Enter the complete plaintext INI path (u cancels): " << std::flush;
    std::string line;
    if (!std::getline(input, line) || line.empty() || line == "u") { return; }
    try {
        auto imported = importer(std::filesystem::path(line));
        if (imported.empty()) {
            errors << "[ERROR] Plaintext INI import did not produce an encrypted configuration.\n";
            return;
        }
        config = std::move(imported);
        output << "[OK] Encrypted configuration created and selected for this CLI session.\n";
        run_market_profile_alias_menu(config, key, input, output, errors, true);
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot import plaintext INI: " << error.what() << '\n';
    }
}

void open_plaintext_import_menu();

} // namespace seastar::xtrader::cli
