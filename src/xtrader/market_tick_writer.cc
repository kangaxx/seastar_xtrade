// Single-writer append with schema/tail checks and explicit durable checkpoints.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include "market_tick_writer.hh"

#include <openssl/rand.h>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace seastar::xtrader::capture_detail {
namespace {

std::string random_session() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
        throw std::runtime_error("cannot generate capture session identifier");
    }
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += HEX[byte >> 4];
        result += HEX[byte & 15];
    }
    return result;
}

void require_time(const std::string& time, int millisec) {
    if (time.size() != 8 || time[2] != ':' || time[5] != ':'
            || millisec < 0 || millisec > 999) {
        throw std::runtime_error("invalid CTP update time");
    }
    for (const auto index : {0, 1, 3, 4, 6, 7}) {
        if (time[index] < '0' || time[index] > '9') {
            throw std::runtime_error("invalid CTP update time digits");
        }
    }
    if (time.substr(0, 2) > "23" || time.substr(3, 2) > "59" || time.substr(6, 2) > "59") {
        throw std::runtime_error("invalid CTP update time range");
    }
}

} // namespace

std::string quote_number(double number) {
    if (!std::isfinite(number) || number == std::numeric_limits<double>::max()) {
        return {};
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
    return output.str();
}

market_tick_writer::market_tick_writer(market_data_paths paths,
        const std::vector<market_capture_contract>& contracts)
    : _paths(std::move(paths)), _writer(_paths), _session_id(random_session()) {
    for (const auto& contract : contracts) {
        _exchanges.emplace(contract.instrument, contract.exchange);
    }
}

std::string market_tick_writer::row(const CThostFtdcDepthMarketDataField& tick, const std::string& exchange,
                                    uint64_t sequence, int64_t received_unix_ms) const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "1," << _session_id << ',' << sequence << ',' << received_unix_ms << ','
           << exchange << ',' << sdk_text(tick.InstrumentID) << ',' << sdk_text(tick.ActionDay) << ','
           << sdk_text(tick.TradingDay) << ',' << sdk_text(tick.UpdateTime) << ',' << tick.UpdateMillisec;
    const auto number = [&output] (double value) { output << ',' << quote_number(value); };
    number(tick.LastPrice);
    output << ',' << tick.Volume;
    for (const auto value : {tick.Turnover, tick.OpenInterest, tick.OpenPrice, tick.HighestPrice, tick.LowestPrice,
            tick.PreClosePrice, tick.PreSettlementPrice, tick.PreOpenInterest, tick.UpperLimitPrice, tick.LowerLimitPrice}) {
        number(value);
    }
    const auto level = [&output, &number] (double bid, int bid_size, double ask, int ask_size) {
        number(bid); output << ',' << bid_size; number(ask); output << ',' << ask_size;
    };
    level(tick.BidPrice1, tick.BidVolume1, tick.AskPrice1, tick.AskVolume1);
    level(tick.BidPrice2, tick.BidVolume2, tick.AskPrice2, tick.AskVolume2);
    level(tick.BidPrice3, tick.BidVolume3, tick.AskPrice3, tick.AskVolume3);
    level(tick.BidPrice4, tick.BidVolume4, tick.AskPrice4, tick.AskVolume4);
    level(tick.BidPrice5, tick.BidVolume5, tick.AskPrice5, tick.AskVolume5);
    return output.str();
}

void market_tick_writer::write(const CThostFtdcDepthMarketDataField& tick,
                                uint64_t sequence, int64_t received_unix_ms) {
    const auto instrument = sdk_text(tick.InstrumentID);
    const auto found = _exchanges.find(instrument);
    if (found == _exchanges.end()) {
        throw std::runtime_error("CTP delivered an unconfigured instrument");
    }
    const auto exchange = sdk_text(tick.ExchangeID);
    if (!exchange.empty() && exchange != found->second) {
        throw std::runtime_error("CTP exchange differs from the configured contract mapping");
    }
    require_time(sdk_text(tick.UpdateTime), tick.UpdateMillisec);
    if (tick.Volume < 0 || sequence == 0 || received_unix_ms <= 0) {
        throw std::runtime_error("invalid CTP volume or receive sequence/time");
    }
    const market_data_key key{"x_trader_data", "futures", found->second, instrument, "tick"};
    (void)_paths.data_file(key, sdk_text(tick.TradingDay));
    _writer.append_tick(key, parse_market_csv_row(row(tick, found->second, sequence, received_unix_ms)));
}

void market_tick_writer::sync() {
    _writer.sync();
}

void market_tick_writer::close() {
    _writer.close();
}

} // namespace seastar::xtrader::capture_detail
