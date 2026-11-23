// One schema-checked POSIX backend; imports publish daily replacements atomically.
// Source: user request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#include <seastar/xtrader/market_data_writer.hh>
#include "market_capture_io.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <sys/file.h>

namespace seastar::xtrader {
namespace {

using namespace capture_detail;

const std::string TICK_HEADER =
    "schema_version,session_id,sequence,received_unix_ms,exchange,instrument,action_day,trading_day,"
    "update_time,update_millisec,last_price,volume,turnover,open_interest,open_price,highest_price,"
    "lowest_price,pre_close_price,pre_settlement_price,pre_open_interest,upper_limit_price,lower_limit_price,"
    "bid_price1,bid_volume1,ask_price1,ask_volume1,bid_price2,bid_volume2,ask_price2,ask_volume2,"
    "bid_price3,bid_volume3,ask_price3,ask_volume3,bid_price4,bid_volume4,ask_price4,ask_volume4,"
    "bid_price5,bid_volume5,ask_price5,ask_volume5\n";
const std::string BAR_HEADER = "date,open,high,low,close,volume,open_interest,money,source_symbol\n";

double number(const std::string& value) {
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double result = 0;
    if (!(input >> result) || !input.eof() || !std::isfinite(result)) {
        throw std::invalid_argument("invalid finite market CSV number");
    }
    return result;
}

void integer(const std::string& value, bool positive = false) {
    if (value.empty() || !std::all_of(value.begin(), value.end(),
            [] (char ch) { return ch >= '0' && ch <= '9'; })) {
        throw std::invalid_argument("invalid unsigned market CSV integer");
    }
    std::size_t consumed = 0;
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size() || (positive && result == 0)) {
        throw std::invalid_argument("invalid market CSV integer range");
    }
}

void time_of_day(const std::string& time) {
    if (time.size() != 8 || time[2] != ':' || time[5] != ':') {
        throw std::invalid_argument("market CSV time must be HH:MM:SS");
    }
    for (const auto index : {0, 1, 3, 4, 6, 7}) {
        if (time[index] < '0' || time[index] > '9') {
            throw std::invalid_argument("invalid market CSV time digits");
        }
    }
    if (time.substr(0, 2) > "23" || time.substr(3, 2) > "59" || time.substr(6, 2) > "59") {
        throw std::invalid_argument("invalid market CSV time range");
    }
}

file_descriptor open_directory(const market_data_paths& paths, const std::filesystem::path& file) {
    paths.validate_filesystem_path(file);
    if (::mkdir(paths.root().c_str(), 0750) != 0 && errno != EEXIST) {
        io_error("create market data root");
    }
    file_descriptor directory(::open(paths.root().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) { io_error("open market data root"); }
    for (const auto& component : file.parent_path().lexically_relative(paths.root())) {
        if (::mkdirat(directory.get(), component.c_str(), 0750) == 0) {
            sync_file(directory.get());
        } else if (errno != EEXIST) {
            io_error("create market data directory");
        }
        file_descriptor next(::openat(directory.get(), component.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) { io_error("open market data directory"); }
        directory = std::move(next);
    }
    return directory;
}

file_descriptor open_regular(int directory, const std::string& name, int flags) {
    file_descriptor file(::openat(directory, name.c_str(),
        flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640));
    if (file.get() < 0) { io_error("open market CSV/lock"); }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) { io_error("inspect market CSV/lock"); }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        throw std::runtime_error("market CSV/lock must be a regular, single-link file");
    }
    return file;
}

file_descriptor lock_file(int directory, const std::string& name) {
    auto lock = open_regular(directory, name + ".lock", O_RDWR | O_CREAT);
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        io_error("lock market CSV (another writer is active)");
    }
    return lock;
}

std::string read_fd(int fd) {
    std::string text;
    char buffer[8192];
    while (true) {
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0) { io_error("read existing market CSV"); }
        if (count == 0) { return text; }
        text.append(buffer, static_cast<std::size_t>(count));
    }
}

std::string row_identity(market_data_kind kind, const market_csv_row& row) {
    if (kind == market_data_kind::bar) { return row[0]; }
    return row[6] + " " + row[8] + "." + std::string(3 - row[9].size(), '0')
        + row[9] + ":" + row[1] + ":" + row[2];
}

void publish(int directory, const std::string& name, const std::string& contents) {
    static std::atomic<unsigned long> serial{0};
    std::string temporary;
    file_descriptor file;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = "." + name + "." + std::to_string(::getpid())
            + "." + std::to_string(++serial) + ".partial";
        file = file_descriptor(::openat(directory, temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0640));
        if (file.get() >= 0) { break; }
        if (errno != EEXIST) { io_error("create temporary market CSV"); }
    }
    if (file.get() < 0) { throw std::runtime_error("cannot reserve temporary market CSV"); }
    bool renamed = false;
    try {
        write_all(file.get(), contents.data(), contents.size());
        sync_file(file.get());
        if (::renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
            io_error("publish market CSV");
        }
        renamed = true;
        sync_file(directory);
    } catch (...) {
        if (!renamed && ::unlinkat(directory, temporary.c_str(), 0) != 0) {
            std::throw_with_nested(std::runtime_error("cannot remove incomplete market CSV"));
        }
        throw;
    }
}

} // namespace

market_data_kind data_kind_for_timeframe(const std::string& timeframe) {
    const auto normalized = market_data_paths::normalize_timeframe(timeframe);
    if (normalized == "native") {
        throw std::invalid_argument("native data requires a dedicated schema, not the Tick/bar CSV codec");
    }
    return normalized == "tick" ? market_data_kind::tick : market_data_kind::bar;
}

const std::string& market_csv_header(market_data_kind kind) {
    return kind == market_data_kind::tick ? TICK_HEADER : BAR_HEADER;
}

market_csv_row parse_market_csv_row(const std::string& line) {
    market_csv_row fields;
    std::string cell;
    bool quoted = false;
    bool closed = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (quoted) {
            if (ch == '"' && index + 1 < line.size() && line[index + 1] == '"') {
                cell += '"';
                ++index;
            } else if (ch == '"') {
                quoted = false;
                closed = true;
            } else {
                cell += ch;
            }
        } else if (ch == ',') {
            fields.push_back(std::move(cell));
            cell.clear();
            closed = false;
        } else if (ch == '"' && cell.empty() && !closed) {
            quoted = true;
        } else {
            if (ch == '"' || closed || ch == '\n' || ch == '\r' || ch == '\0') {
                throw std::invalid_argument("invalid single-line market CSV quoting");
            }
            cell += ch;
        }
    }
    if (quoted) { throw std::invalid_argument("unterminated market CSV quote"); }
    fields.push_back(std::move(cell));
    return fields;
}

std::string encode_market_csv_row(const market_csv_row& row) {
    std::ostringstream output;
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (index) { output << ','; }
        const auto& cell = row[index];
        if (cell.find_first_of("\r\n") != std::string::npos || cell.find('\0') != std::string::npos) {
            throw std::invalid_argument("market CSV cell cannot contain line breaks or NUL");
        }
        if (cell.find_first_of(",\"") == std::string::npos) {
            output << cell;
        } else {
            output << '"';
            for (const char ch : cell) {
                if (ch == '"') { output << '"'; }
                output << ch;
            }
            output << '"';
        }
    }
    return output.str() + '\n';
}

std::string validate_market_csv_row(const market_data_key& key, const market_csv_row& row) {
    const auto kind = data_kind_for_timeframe(key.timeframe);
    const market_data_paths validation;
    std::string date;
    if (kind == market_data_kind::bar) {
        if (row.size() != 9 || row[0].size() != 19 || row[0][4] != '-' || row[0][7] != '-'
                || row[0][10] != ' ') {
            throw std::invalid_argument("bar CSV requires canonical datetime and nine columns");
        }
        date = row[0].substr(0, 4) + row[0].substr(5, 2) + row[0].substr(8, 2);
        time_of_day(row[0].substr(11));
        const double open = number(row[1]), high = number(row[2]), low = number(row[3]);
        const double close = number(row[4]);
        if (number(row[5]) < 0 || number(row[6]) < 0 || high < low
                || open < low || open > high || close < low || close > high) {
            throw std::invalid_argument("invalid bar OHLC/volume/open interest");
        }
        if (!row[7].empty()) { (void)number(row[7]); }
    } else {
        if (row.size() != parse_market_csv_row(TICK_HEADER.substr(0, TICK_HEADER.size() - 1)).size()
                || row[0] != "1" || row[1].empty() || row[4] != key.exchange || row[5] != key.instrument) {
            throw std::invalid_argument("Tick CSV schema or instrument/exchange differs from dataset");
        }
        integer(row[2], true);
        integer(row[3], true);
        time_of_day(row[8]);
        integer(row[9]);
        if (row[9].size() > 3 || number(row[9]) > 999) {
            throw std::invalid_argument("invalid Tick millisecond");
        }
        integer(row[11]);
        for (std::size_t index = 10; index < row.size(); ++index) {
            if (!row[index].empty()) { (void)number(row[index]); }
        }
        date = row[6];
        (void)validation.data_file({"historic", key.asset_class, key.exchange, key.instrument, "tick"}, row[7]);
    }
    (void)validation.data_file({"historic", key.asset_class, key.exchange, key.instrument, key.timeframe}, date);
    (void)encode_market_csv_row(row);
    return date;
}

struct market_data_writer::impl {
    struct output {
        file_descriptor directory;
        file_descriptor lock;
        file_descriptor file;
    };
    market_data_paths paths;
    std::map<std::filesystem::path, output> outputs;
    explicit impl(market_data_paths value) : paths(std::move(value)) {}
};

market_data_writer::market_data_writer(market_data_paths paths)
    : _impl(std::make_unique<impl>(std::move(paths))) {}
market_data_writer::~market_data_writer() = default;

void market_data_writer::append_tick(const market_data_key& key, const market_csv_row& row) {
    if (data_kind_for_timeframe(key.timeframe) != market_data_kind::tick) {
        throw std::invalid_argument("append_tick requires a Tick dataset");
    }
    const auto path = _impl->paths.data_file(key, validate_market_csv_row(key, row));
    auto found = _impl->outputs.find(path);
    if (found == _impl->outputs.end()) {
        // Preserve the collector's bounded open-file count across date rotation.
        for (auto it = _impl->outputs.begin(); it != _impl->outputs.end();) {
            if (it->first.parent_path() == path.parent_path()) {
                sync_file(it->second.file.get());
                it = _impl->outputs.erase(it);
            } else { ++it; }
        }
        auto directory = open_directory(_impl->paths, path);
        auto lock = lock_file(directory.get(), path.filename().string());
        auto file = open_regular(directory.get(), path.filename().string(), O_RDWR | O_APPEND | O_CREAT);
        if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) { io_error("lock Tick CSV"); }
        struct stat status{};
        if (::fstat(file.get(), &status) != 0) { io_error("inspect locked Tick CSV"); }
        if (status.st_size == 0) {
            write_all(file.get(), TICK_HEADER.data(), TICK_HEADER.size());
            sync_file(file.get());
            sync_file(directory.get());
        } else {
            std::string header(TICK_HEADER.size(), '\0');
            char tail = '\0';
            if (::pread(file.get(), header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())
                    || header != TICK_HEADER || ::pread(file.get(), &tail, 1, status.st_size - 1) != 1 || tail != '\n') {
                throw std::runtime_error("existing Tick CSV has a different schema or incomplete final row");
            }
        }
        found = _impl->outputs.emplace(path, impl::output{
            std::move(directory), std::move(lock), std::move(file)}).first;
    }
    const auto text = encode_market_csv_row(row);
    write_all(found->second.file.get(), text.data(), text.size());
}

market_write_result market_data_writer::merge(const market_data_key& key,
        const std::vector<market_csv_row>& rows, bool overwrite) {
    const auto kind = data_kind_for_timeframe(key.timeframe);
    std::map<std::filesystem::path, std::vector<market_csv_row>> partitions;
    for (const auto& row : rows) {
        partitions[_impl->paths.data_file(key, validate_market_csv_row(key, row))].push_back(row);
    }
    market_write_result result;
    for (const auto& [path, incoming] : partitions) {
        if (_impl->outputs.count(path)) { throw std::runtime_error("cannot merge into an active append file"); }
        auto directory = open_directory(_impl->paths, path);
        auto lock = lock_file(directory.get(), path.filename().string());
        std::map<std::string, market_csv_row> merged;
        struct stat status{};
        file_descriptor existing;
        if (::fstatat(directory.get(), path.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
            existing = open_regular(directory.get(), path.filename().string(), O_RDONLY);
            if (::flock(existing.get(), LOCK_EX | LOCK_NB) != 0) { io_error("lock existing market CSV"); }
            const auto text = read_fd(existing.get());
            if (!text.empty()) {
                if (text.back() != '\n') { throw std::runtime_error("existing CSV has an incomplete final row"); }
                std::istringstream input(text);
                std::string line;
                std::getline(input, line);
                if (line + '\n' != market_csv_header(kind)) {
                    throw std::runtime_error("existing market CSV schema differs");
                }
                while (std::getline(input, line)) {
                    auto row = parse_market_csv_row(line);
                    if (_impl->paths.data_file(key, validate_market_csv_row(key, row)) != path) {
                        throw std::runtime_error("existing market CSV row belongs to another daily partition");
                    }
                    if (!merged.emplace(row_identity(kind, row), std::move(row)).second) {
                        throw std::runtime_error("duplicate row identity in existing market CSV");
                    }
                }
            }
        } else if (errno != ENOENT) { io_error("inspect existing market CSV"); }
        std::size_t changed = 0;
        for (const auto& row : incoming) {
            const auto identity = row_identity(kind, row);
            const auto found = merged.find(identity);
            if (found == merged.end()) { merged.emplace(identity, row); ++result.added; ++changed; }
            else if (overwrite) { found->second = row; ++result.overwritten; ++changed; }
            else { ++result.skipped; }
        }
        if (changed) {
            std::string contents = market_csv_header(kind);
            for (const auto& [identity, row] : merged) {
                (void)identity;
                contents += encode_market_csv_row(row);
            }
            publish(directory.get(), path.filename().string(), contents);
        }
        result.files.push_back(path);
    }
    return result;
}

void market_data_writer::sync() {
    for (const auto& [path, output] : _impl->outputs) {
        (void)path;
        sync_file(output.file.get());
    }
}

void market_data_writer::close() {
    sync();
    _impl->outputs.clear();
}

} // namespace seastar::xtrader
