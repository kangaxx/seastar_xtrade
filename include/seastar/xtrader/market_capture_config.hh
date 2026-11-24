// Authenticated configuration for a read-only CTP market collector.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seastar::xtrader {

struct market_capture_contract {
    std::string exchange;
    std::string instrument;
};

struct legacy_trader_config {
    std::string trade_front;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string user_product_info;

    legacy_trader_config() = default;
    legacy_trader_config(legacy_trader_config&&) = default;
    legacy_trader_config& operator=(legacy_trader_config&&) = delete;
    legacy_trader_config(const legacy_trader_config&) = delete;
    legacy_trader_config& operator=(const legacy_trader_config&) = delete;
    ~legacy_trader_config();
};

struct market_capture_config {
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string market_front;
    std::vector<market_capture_contract> contracts;
    std::optional<legacy_trader_config> trader;

    market_capture_config() = default;
    market_capture_config(market_capture_config&&) = default;
    market_capture_config& operator=(market_capture_config&&) = delete;
    market_capture_config(const market_capture_config&) = delete;
    market_capture_config& operator=(const market_capture_config&) = delete;
    ~market_capture_config();
};

void validate_market_capture_config(const market_capture_config& config);
void validate_market_capture_contracts(const std::vector<market_capture_contract>& contracts);
std::vector<market_capture_contract> parse_market_capture_contracts(const std::string& text);
market_capture_config import_legacy_market_config(const std::filesystem::path& ini_file,
                                                  const std::string& secret,
                                                  std::vector<market_capture_contract> contracts);
market_capture_config import_plaintext_market_config(const std::filesystem::path& ini_file);
void create_market_capture_key(const std::filesystem::path& key_file);
// Initialize a new encrypted config, creating or securely reusing its external key.
void initialize_market_capture_config(const market_capture_config& config,
                                     const std::filesystem::path& encrypted_file,
                                     const std::filesystem::path& key_file);
void save_market_capture_config(const market_capture_config& config,
                               const std::filesystem::path& encrypted_file,
                               const std::filesystem::path& key_file);
market_capture_config load_market_capture_config(const std::filesystem::path& encrypted_file,
                                                 const std::filesystem::path& key_file);

} // namespace seastar::xtrader
