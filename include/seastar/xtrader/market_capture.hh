// MD-only lifecycle, injectable SDK factory for offline full-chain tests.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <seastar/xtrader/market_capture_config.hh>
#include <seastar/xtrader/market_data_paths.hh>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>

class CThostFtdcMdApi;

namespace seastar::xtrader {

class market_capture_session {
public:
    using api_factory = std::function<CThostFtdcMdApi*(const char*)>;

    market_capture_session(market_capture_config config, market_data_paths paths,
                           std::filesystem::path state_directory, api_factory factory);
    ~market_capture_session();
    market_capture_session(const market_capture_session&) = delete;
    market_capture_session& operator=(const market_capture_session&) = delete;

    void start();
    void poll_for(std::chrono::milliseconds timeout);
    void stop();
    bool is_ready() const noexcept;
    uint64_t ticks_written() const noexcept;
    void print_status(std::ostream& output) const;

private:
    class implementation;
    std::unique_ptr<implementation> _impl;
};

} // namespace seastar::xtrader
