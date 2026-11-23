// Shared CSV schemas and durable storage for imports and live capture.
// Source: user request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include <seastar/xtrader/market_data_paths.hh>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace seastar::xtrader {

enum class market_data_kind { tick, bar };
using market_csv_row = std::vector<std::string>;

market_data_kind data_kind_for_timeframe(const std::string& timeframe);
const std::string& market_csv_header(market_data_kind kind);
market_csv_row parse_market_csv_row(const std::string& line);
std::string encode_market_csv_row(const market_csv_row& row);
std::string validate_market_csv_row(const market_data_key& key, const market_csv_row& row);

struct market_write_result {
    std::vector<std::filesystem::path> files;
    std::size_t added = 0;
    std::size_t overwritten = 0;
    std::size_t skipped = 0;
};

// Blocking worker-side I/O. Tick serializers do not own a separate filesystem backend.
class market_data_writer {
public:
    explicit market_data_writer(market_data_paths paths);
    ~market_data_writer();
    market_data_writer(const market_data_writer&) = delete;
    market_data_writer& operator=(const market_data_writer&) = delete;

    void append_tick(const market_data_key& key, const market_csv_row& row);
    market_write_result merge(const market_data_key& key,
                              const std::vector<market_csv_row>& rows, bool overwrite);
    void sync();
    void close();

private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

} // namespace seastar::xtrader
