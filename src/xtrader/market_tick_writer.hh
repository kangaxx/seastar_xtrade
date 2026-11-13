// Raw SDK Tick CSV writer; not a bar decoder or normalized paper-trading sink.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <seastar/xtrader/market_capture_config.hh>
#include <seastar/xtrader/market_data_paths.hh>
#include <ThostFtdcMdApi.h>
#include "market_capture_io.hh"

#include <chrono>
#include <map>
#include <string>

namespace seastar::xtrader::capture_detail {

template<std::size_t N>
std::string sdk_text(const char (&value)[N]) {
    const auto end = static_cast<const char*>(std::memchr(value, '\0', N));
    if (!end) { throw std::runtime_error("CTP text field is not terminated"); }
    return std::string(value, end);
}

std::string quote_number(double number);

class market_tick_writer {
public:
    market_tick_writer(market_data_paths paths, const std::vector<market_capture_contract>& contracts);
    void write(const CThostFtdcDepthMarketDataField& tick, uint64_t sequence, int64_t received_unix_ms);
    void sync();
    void close();
private:
    struct output_file {
        std::filesystem::path path;
        file_descriptor fd;
    };
    output_file open_file(const std::filesystem::path& path);
    std::string row(const CThostFtdcDepthMarketDataField& tick, const std::string& exchange,
                    uint64_t sequence, int64_t received_unix_ms) const;
    market_data_paths _paths;
    std::map<std::string, std::string> _exchanges;
    std::map<std::string, output_file> _files;
    std::string _session_id;
};

} // namespace seastar::xtrader::capture_detail
