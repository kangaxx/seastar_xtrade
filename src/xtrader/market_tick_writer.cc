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
#include <string_view>
#include <sys/file.h>

namespace seastar::xtrader::capture_detail {
namespace {

constexpr std::string_view HEADER =
    "schema_version,session_id,sequence,received_unix_ms,exchange,instrument,action_day,trading_day,"
    "update_time,update_millisec,last_price,volume,turnover,open_interest,open_price,highest_price,"
    "lowest_price,pre_close_price,pre_settlement_price,pre_open_interest,upper_limit_price,lower_limit_price,"
    "bid_price1,bid_volume1,ask_price1,ask_volume1,bid_price2,bid_volume2,ask_price2,ask_volume2,"
    "bid_price3,bid_volume3,ask_price3,ask_volume3,bid_price4,bid_volume4,ask_price4,ask_volume4,"
    "bid_price5,bid_volume5,ask_price5,ask_volume5\n";

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

file_descriptor open_dataset_directory(const market_data_paths& paths, const std::filesystem::path& file) {
    paths.validate_filesystem_path(file);
    file_descriptor directory(::open(paths.root().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) { io_error("open data root (prepare /data and grant access first)"); }
    const auto relative = file.parent_path().lexically_relative(paths.root());
    for (const auto& component : relative) {
        if (::mkdirat(directory.get(), component.c_str(), 0750) == 0) {
            sync_file(directory.get());
        } else if (errno != EEXIST) {
            io_error("create market data directory");
        }
        file_descriptor next(::openat(directory.get(), component.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) { io_error("open market data directory (links are not allowed)"); }
        directory = std::move(next);
    }
    return directory;
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
    : _paths(std::move(paths)), _session_id(random_session()) {
    for (const auto& contract : contracts) {
        _exchanges.emplace(contract.instrument, contract.exchange);
    }
}

market_tick_writer::output_file market_tick_writer::open_file(const std::filesystem::path& path) {
    auto directory = open_dataset_directory(_paths, path);
    file_descriptor file(::openat(directory.get(), path.filename().c_str(),
        O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640));
    if (file.get() < 0) { io_error("open Tick CSV"); }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) { io_error("inspect Tick CSV"); }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        throw std::runtime_error("Tick output must be a regular, single-link file");
    }
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
        io_error("lock Tick CSV (another collector may be writing this contract)");
    }
    if (::fstat(file.get(), &status) != 0) { io_error("inspect locked Tick CSV"); }
    if (status.st_size == 0) {
        write_all(file.get(), HEADER.data(), HEADER.size());
        sync_file(file.get());
        sync_file(directory.get());
    } else {
        std::string header(HEADER.size(), '\0');
        char tail = '\0';
        if (::pread(file.get(), header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())
                || header != HEADER || ::pread(file.get(), &tail, 1, status.st_size - 1) != 1 || tail != '\n') {
            throw std::runtime_error("existing Tick CSV has a different schema or an incomplete final row; preserve and inspect it");
        }
    }
    return {path, std::move(file)};
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
    return output.str() + '\n';
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
    const auto path = _paths.data_file(key, sdk_text(tick.ActionDay));
    auto file = _files.find(instrument);
    if (file != _files.end() && file->second.path != path) {
        sync_file(file->second.fd.get());
        _files.erase(file);
        file = _files.end();
    }
    if (file == _files.end()) {
        file = _files.emplace(instrument, open_file(path)).first;
    }
    const auto text = row(tick, found->second, sequence, received_unix_ms);
    write_all(file->second.fd.get(), text.data(), text.size());
}

void market_tick_writer::sync() {
    for (const auto& [instrument, file] : _files) {
        (void)instrument;
        sync_file(file.fd.get());
    }
}

void market_tick_writer::close() {
    sync();
    _files.clear();
}

} // namespace seastar::xtrader::capture_detail
