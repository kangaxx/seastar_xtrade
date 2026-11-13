// Authenticated configuration for a read-only CTP market collector.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace seastar::xtrader {

struct market_capture_contract {
    std::string exchange;
    std::string instrument;
};

struct market_capture_config {
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string market_front;
    std::vector<market_capture_contract> contracts;

    market_capture_config() = default;
    market_capture_config(market_capture_config&&) = default;
    market_capture_config& operator=(market_capture_config&&) = delete;
    market_capture_config(const market_capture_config&) = delete;
    market_capture_config& operator=(const market_capture_config&) = delete;
    ~market_capture_config();
};

void validate_market_capture_config(const market_capture_config& config);
std::vector<market_capture_contract> parse_market_capture_contracts(const std::string& text);
void create_market_capture_key(const std::filesystem::path& key_file);
void save_market_capture_config(const market_capture_config& config,
                               const std::filesystem::path& encrypted_file,
                               const std::filesystem::path& key_file);
market_capture_config load_market_capture_config(const std::filesystem::path& encrypted_file,
                                                 const std::filesystem::path& key_file);

} // namespace seastar::xtrader
