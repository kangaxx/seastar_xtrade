/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <seastar/xtrader/backtest_driver.hh>

#include "ledger_validation.hh"
#include <seastar/xtrader/historical_data_manager.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace seastar::xtrader {

namespace {

sstring to_lower_copy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return sstring(result);
}

bool parse_digits(std::string_view value, size_t max_digits, int& result) {
    if (value.empty() || value.size() > max_digits ||
        !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) {
        return false;
    }
    result = 0;
    for (char ch : value) {
        result = result * 10 + ch - '0';
    }
    return true;
}

sstring normalize_calendar_date(const std::string& datetime) {
    const auto pos = datetime.find(' ');
    const auto date = pos == std::string::npos ? datetime : datetime.substr(0, pos);
    if (date.empty() || date.back() == '/' || date.back() == '-') {
        return {};
    }

    std::vector<std::string> parts;
    std::string current;
    for (char ch : date) {
        if (ch == '/' || ch == '-') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }

    if (parts.size() != 3) {
        return {};
    }
    int year = 0;
    int month = 0;
    int day = 0;
    if (parts[0].size() != 4 || !parse_digits(parts[0], 4, year) || year == 0 ||
        !parse_digits(parts[1], 2, month) || !parse_digits(parts[2], 2, day) ||
        !std::chrono::year_month_day{
            std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
            std::chrono::day{static_cast<unsigned>(day)}}.ok()) {
        return {};
    }

    auto pad2 = [] (std::string value) {
        if (value.size() == 1) {
            value.insert(value.begin(), '0');
        }
        return value;
    };

    return sstring(parts[0] + pad2(parts[1]) + pad2(parts[2]));
}

sstring normalize_time_str(const std::string& datetime) {
    const auto pos = datetime.find(' ');
    if (pos == std::string::npos) {
        return {};
    }

    const auto time = datetime.substr(pos + 1);
    if (time.empty() || time.back() == ':') {
        return {};
    }
    std::vector<std::string> parts;
    std::string current;
    for (char ch : time) {
        if (ch == ':') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }

    if (parts.size() < 2 || parts.size() > 3) {
        return {};
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    std::string seconds = parts.size() == 3 ? parts[2] : "0";
    std::string fraction;
    const auto dot = seconds.find('.');
    if (dot != std::string::npos) {
        fraction = seconds.substr(dot + 1);
        seconds.resize(dot);
        int ignored = 0;
        if (!parse_digits(fraction, 9, ignored)) {
            return {};
        }
    }
    if (!parse_digits(parts[0], 2, hour) || hour > 23 ||
        !parse_digits(parts[1], 2, minute) || minute > 59 ||
        !parse_digits(seconds, 2, second) || second > 59) {
        return {};
    }

    auto pad2 = [] (std::string value) {
        if (value.size() == 1) {
            value.insert(value.begin(), '0');
        }
        return value;
    };

    std::string result = pad2(parts[0]) + ":" + pad2(parts[1]) + ":";
    result += pad2(seconds);
    if (!fraction.empty()) {
        result += "." + fraction;
    }
    return sstring(std::move(result));
}

sstring normalize_date_filter(const sstring& value) {
    if (value.empty() || value.find(' ') != sstring::npos) {
        return {};
    }
    std::string date(value);
    if (date.size() == 8 && std::all_of(date.begin(), date.end(), [] (unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) {
        date = date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2);
    }
    return normalize_calendar_date(date);
}

std::optional<int64_t> calendar_timestamp_nanos(const sstring& date, const sstring& time) {
    const std::string_view date_view(date.data(), date.size());
    const std::string_view time_view(time.data(), time.size());
    if (date_view.size() != 8 || time_view.size() < 8) {
        return std::nullopt;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!parse_digits(date_view.substr(0, 4), 4, year) ||
        !parse_digits(date_view.substr(4, 2), 2, month) ||
        !parse_digits(date_view.substr(6, 2), 2, day) ||
        !parse_digits(time_view.substr(0, 2), 2, hour) ||
        !parse_digits(time_view.substr(3, 2), 2, minute) ||
        !parse_digits(time_view.substr(6, 2), 2, second)) {
        return std::nullopt;
    }
    int fraction = 0;
    if (time_view.size() > 8) {
        if (time_view[8] != '.' || !parse_digits(time_view.substr(9), 9, fraction)) {
            return std::nullopt;
        }
        for (size_t digits = time_view.size() - 9; digits < 9; ++digits) {
            fraction *= 10;
        }
    }
    const auto calendar_day = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)}
        / std::chrono::day{static_cast<unsigned>(day)};
    if (!calendar_day.ok() || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    constexpr int64_t nanos_per_second = 1'000'000'000LL;
    constexpr int64_t nanos_per_day = 86'400LL * nanos_per_second;
    const int64_t days = std::chrono::sys_days{calendar_day}.time_since_epoch().count();
    if (days > std::numeric_limits<int64_t>::max() / nanos_per_day ||
        days < std::numeric_limits<int64_t>::min() / nanos_per_day) {
        return std::nullopt;
    }
    const int64_t day_start = days * nanos_per_day;
    const int64_t elapsed = (hour * 3600LL + minute * 60LL + second) * nanos_per_second + fraction;
    if (day_start > std::numeric_limits<int64_t>::max() - elapsed) {
        return std::nullopt;
    }
    return day_start + elapsed;
}

} // namespace

// ==================== execution_simulator ====================

execution_simulator::execution_simulator(const backtest_config& cfg)
    : _cfg(cfg)
{}

domain::trade_report execution_simulator::simulate_fill(
    const domain::order_request&,
    const market_event&,
    double&) {
    throw std::runtime_error("execution simulation is disabled until matching and accounting are integrated");
}

domain::order_status execution_simulator::simulate_limit_order(
    const domain::order_request&,
    const market_event&) {
    throw std::runtime_error("execution simulation is disabled until matching and accounting are integrated");
}

// ==================== backtest_clock ====================

void backtest_clock::advance_to(int64_t nanos) {
    _current_nanos = nanos;
}

bool backtest_clock::is_trading_time(const sstring& time_str) const {
    if (time_str.empty() || time_str.size() < 8) {
        return false;
    }

    int hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
    int minute = (time_str[3] - '0') * 10 + (time_str[4] - '0');
    int total_minutes = hour * 60 + minute;

    // Day session: 09:00 - 11:30, 13:30 - 15:00
    // Night session: 21:00 - 02:30 (crosses midnight)
    bool morning = total_minutes >= 9 * 60 && total_minutes < 11 * 60 + 30;
    bool afternoon = total_minutes >= 13 * 60 + 30 && total_minutes < 15 * 60;
    bool night_early = total_minutes >= 21 * 60;
    bool night_late = hour < 2 || (hour == 2 && minute < 30);

    return morning || afternoon || night_early || night_late;
}

backtest_clock::trading_session backtest_clock::get_session(
    const sstring& instrument_id) {
    trading_session session;
    if (instrument_id.find("SHFE") != sstring::npos
        || instrument_id.find("INE") != sstring::npos) {
        session.open_time = "09:00:00";
        session.close_time = "15:00:00";
        session.crosses_midnight = false;
    } else if (instrument_id.find("CFFEX") != sstring::npos) {
        session.open_time = "09:30:00";
        session.close_time = "15:15:00";
        session.crosses_midnight = false;
    } else {
        session.open_time = "21:00:00";
        session.close_time = "15:00:00";
        session.crosses_midnight = true;
    }
    return session;
}

// ==================== backtest_driver ====================

backtest_driver::backtest_driver(backtest_config cfg)
    : _cfg(std::move(cfg))
{}

future<> backtest_driver::start() {
    if (_stopping) {
        return make_exception_future<>(std::runtime_error("replay driver is still stopping"));
    }
    if (_replay_failed || _replay_completed) {
        return make_exception_future<>(std::runtime_error("replay requires a new driver"));
    }
    if (_started) {
        return make_ready_future<>();
    }
    if (_operations.is_closed()) {
        _operations = gate{};
    }
    _started = true;
    return make_ready_future<>();
}

future<> backtest_driver::stop() {
    if (_stopping) {
        return make_exception_future<>(std::runtime_error("replay driver is already stopping"));
    }
    _started = false;
    if (_operations.is_closed()) {
        return make_ready_future<>();
    }
    _stopping = true;
    return _operations.close().then([this] {
        _stopping = false;
    });
}

future<> backtest_driver::load_data(const sstring& instrument_id) {
    if (_running || _replay_completed || _replay_failed || _pending_loads != 0 ||
        _stopping || _operations.is_closed()) {
        return make_exception_future<>(std::runtime_error("replay driver cannot start another data load"));
    }
    const auto id = sstring(instrument_id);
    ++_pending_loads;
    return with_gate(_operations, [this, id] {
        return futurize_invoke([this, id] {
            if (!std::any_of(id.begin(), id.end(), [] (unsigned char ch) {
                    return !std::isspace(ch);
                })) {
                throw std::invalid_argument("backtest instrument is required");
            }
            switch (_cfg.data_mode) {
            case backtest_data_mode::historical:
                load_historical_data(id);
                break;
            case backtest_data_mode::embedded_demo:
                load_demo_data(id);
                break;
            default:
                throw std::invalid_argument("invalid backtest data mode");
            }
        }).then_wrapped([this, id] (future<> result) {
            if (result.failed()) {
                _failed_instruments.insert(id);
                return make_exception_future<>(result.get_exception());
            }
            result.get();
            _failed_instruments.erase(id);
            return make_ready_future<>();
        }).finally([this] {
            --_pending_loads;
        });
    });
}

void backtest_driver::load_historical_data(const sstring& instrument_id) {
    historical_data_manager manager(std::filesystem::path(std::string(_cfg.data_root)));
    std::string warning;
    const auto datasets = manager.scan_datasets(&warning);
    if (!warning.empty()) {
        throw std::runtime_error(warning);
    }

    const auto instrument_key = to_lower_copy(instrument_id);
    const auto exchange_key = to_lower_copy(_cfg.dataset_exchange);
    const auto timeframe_key = to_lower_copy(_cfg.dataset_timeframe);
    const dataset_manifest* selected = nullptr;
    for (const auto& dataset : datasets) {
        if (to_lower_copy(dataset.symbol) != instrument_key ||
            (!exchange_key.empty() && to_lower_copy(dataset.exchange) != exchange_key) ||
            (!timeframe_key.empty() && to_lower_copy(dataset.timeframe) != timeframe_key)) {
            continue;
        }
        if (selected != nullptr) {
            throw std::runtime_error("ambiguous historical datasets for " + std::string(instrument_id));
        }
        selected = &dataset;
    }
    if (!selected) {
        throw std::runtime_error("historical dataset not found for " + std::string(instrument_id));
    }
    if (selected->exchange.empty() || selected->timeframe.empty()) {
        throw std::runtime_error("historical dataset is missing exchange or timeframe");
    }
    if (to_lower_copy(selected->timeframe) == "tick") {
        throw std::runtime_error("historical tick datasets are not supported by the bar loader");
    }

    std::vector<historical_bar> bars;
    if (!manager.load_bars(*selected, bars, &warning, historical_load_mode::strict)) {
        throw std::runtime_error(warning);
    }
    std::vector<replay_record> records;
    records.reserve(bars.size());
    for (const auto& bar : bars) {
        auto calendar_date = normalize_calendar_date(bar.datetime);
        auto time_str = normalize_time_str(bar.datetime);
        const auto timestamp_nanos = calendar_timestamp_nanos(calendar_date, time_str);
        if (!timestamp_nanos ||
            bar.volume > std::numeric_limits<int>::max() || std::trunc(bar.volume) != bar.volume) {
            throw std::runtime_error("invalid historical bar at line " + std::to_string(bar.source_line)
                + " in " + selected->active_file_path.generic_string());
        }
        records.emplace_back(replay_record{
            .type = market_event_type::bar,
            .timestamp_nanos = *timestamp_nanos,
            .calendar_date = std::move(calendar_date),
            .time_str = std::move(time_str),
            .exchange_id = selected->exchange,
            .timeframe = selected->timeframe,
            .open_price = bar.open,
            .high_price = bar.high,
            .low_price = bar.low,
            .close_price = bar.close,
            .volume = static_cast<int>(bar.volume),
            .open_interest = bar.open_interest,
        });
    }
    store_loaded_data(instrument_id, std::move(records));
}

void backtest_driver::load_demo_data(const sstring& instrument_id) {
    const auto first_time = calendar_timestamp_nanos("20260101", "09:31:00");
    const auto second_time = calendar_timestamp_nanos("20260101", "09:32:00");
    if (!first_time || !second_time) {
        throw std::logic_error("embedded replay dates are invalid");
    }
    std::vector<replay_record> records;
    records.emplace_back(replay_record{
        .timestamp_nanos = *first_time,
        .calendar_date = "20260101",
        .trading_day = "20260101",
        .time_str = "09:31:00",
        .last_price = 3500.0,
        .open_price = 3495.0,
        .high_price = 3502.0,
        .low_price = 3494.0,
        .close_price = 3500.0,
        .volume = 100,
        .turnover = 3500000.0,
        .open_interest = 50000.0,
    });
    records.emplace_back(replay_record{
        .timestamp_nanos = *second_time,
        .calendar_date = "20260101",
        .trading_day = "20260101",
        .time_str = "09:32:00",
        .last_price = 3510.0,
        .open_price = 3500.0,
        .high_price = 3512.0,
        .low_price = 3499.0,
        .close_price = 3510.0,
        .volume = 200,
        .turnover = 7020000.0,
        .open_interest = 50100.0,
    });

    store_loaded_data(instrument_id, std::move(records));
}

void backtest_driver::store_loaded_data(const sstring& instrument_id, std::vector<replay_record> records) {
    const auto start = normalize_date_filter(_cfg.start_date);
    const auto end = normalize_date_filter(_cfg.end_date);
    if ((!_cfg.start_date.empty() && start.empty()) || (!_cfg.end_date.empty() && end.empty()) ||
        (!start.empty() && !end.empty() && start > end)) {
        throw std::invalid_argument("invalid replay calendar-date range");
    }
    std::erase_if(records, [&start, &end] (const replay_record& record) {
        return (!start.empty() && record.calendar_date < start) ||
            (!end.empty() && record.calendar_date > end);
    });
    if (records.empty()) {
        throw std::runtime_error("no replay data within the requested calendar-date range for "
            + std::string(instrument_id));
    }
    auto [it, inserted] = _replay_data.try_emplace(instrument_id);
    auto adjust_counts = [this] (const std::vector<replay_record>& values, bool add) {
        for (const auto& value : values) {
            auto& count = value.type == market_event_type::bar ? _stats.total_bars : _stats.total_ticks;
            if (add) {
                ++count;
            } else {
                --count;
            }
        }
    };
    if (!inserted) {
        adjust_counts(it->second, false);
    }
    it->second = std::move(records);
    adjust_counts(it->second, true);
}

std::vector<offline_bar_record> backtest_driver::export_offline_bars(
        const backtest_bar_export_options& options) const {
    if (_running || _stopping || _pending_loads != 0 || _replay_failed || !_failed_instruments.empty()
        || _replay_data.empty()) {
        throw std::logic_error("offline bar export requires successfully loaded, idle replay data");
    }
    if (!options.timestamp_kind || (*options.timestamp_kind != offline_bar_timestamp::open
                                   && *options.timestamp_kind != offline_bar_timestamp::close)
        || options.duration_nanos <= 0 || !ledger_detail::has_identifier(options.time_domain)
        || !options.resolve_trading_day) {
        throw std::invalid_argument("offline bar export requires explicit time and trading-day rules");
    }
    std::vector<offline_bar_record> bars;
    for (const auto& [instrument, records] : _replay_data) {
        for (const auto& record : records) {
            if (record.type != market_event_type::bar) {
                throw std::invalid_argument("offline bar export does not accept tick or demo data");
            }
            auto trading_day = options.resolve_trading_day(
                record.exchange_id, instrument, record.calendar_date, record.time_str);
            if (!ledger_detail::is_valid_trading_day(trading_day)) {
                throw std::invalid_argument("offline bar trading-day resolver returned an invalid day");
            }
            bars.push_back({
                .exchange_id = record.exchange_id,
                .instrument_id = instrument,
                .timeframe = record.timeframe,
                .trading_day = std::move(trading_day),
                .time_domain = options.time_domain,
                .timestamp_nanos = record.timestamp_nanos,
                .duration_nanos = options.duration_nanos,
                .timestamp_kind = options.timestamp_kind,
                .open_price = record.open_price,
                .high_price = record.high_price,
                .low_price = record.low_price,
                .close_price = record.close_price,
                .volume = record.volume,
                .open_interest = record.open_interest,
            });
        }
    }
    std::stable_sort(bars.begin(), bars.end(), [] (const auto& left, const auto& right) {
        return std::tie(left.timestamp_nanos, left.exchange_id, left.instrument_id)
             < std::tie(right.timestamp_nanos, right.exchange_id, right.instrument_id);
    });
    return bars;
}

future<> backtest_driver::run() {
    if (_running || _stopping || _replay_failed || _replay_completed || _pending_loads != 0) {
        return make_exception_future<>(std::runtime_error("replay cannot be started now"));
    }
    if (!_failed_instruments.empty()) {
        return make_exception_future<>(std::runtime_error("cannot replay after a data load failed"));
    }
    if (!_started || _replay_data.empty() || !_event_bus || !_event_bus->is_started()) {
        return make_exception_future<>(
            std::runtime_error("replay requires a started driver, loaded data and a running event bus"));
    }
    _running = true;
    return with_gate(_operations, [this] {
        return futurize_invoke([this] { return run_impl(); }).then_wrapped([this] (future<> result) {
            _running = false;
            if (result.failed()) {
                _replay_failed = true;
                return make_exception_future<>(result.get_exception());
            }
            result.get();
            _replay_completed = true;
            return make_ready_future<>();
        });
    });
}

future<> backtest_driver::run_impl() {
    struct replay_item {
        const sstring* instrument;
        const replay_record* record;
        size_t index;
    };
    std::vector<replay_item> items;
    for (const auto& [instrument, records] : _replay_data) {
        for (size_t index = 0; index < records.size(); ++index) {
            if (!_clock.is_trading_time(records[index].time_str)) {
                throw std::runtime_error("replay time outside the currently supported session: "
                    + std::string(instrument) + " " + std::string(records[index].time_str));
            }
            items.push_back({&instrument, &records[index], index});
        }
    }
    std::sort(items.begin(), items.end(), [] (const replay_item& left, const replay_item& right) {
        if (left.record->timestamp_nanos != right.record->timestamp_nanos) {
            return left.record->timestamp_nanos < right.record->timestamp_nanos;
        }
        if (*left.instrument != *right.instrument) {
            return *left.instrument < *right.instrument;
        }
        return left.index < right.index;
    });

    for (const auto& item : items) {
        if (_stopping) {
            throw std::runtime_error("replay stopped before all events were delivered");
        }
        _clock.advance_to(item.record->timestamp_nanos);
        if (!replay_event(*item.instrument, *item.record)) {
            throw std::runtime_error("event bus rejected a replay event");
        }
        co_await _event_bus->drain();
    }
    _stats.start_date = normalize_date_filter(_cfg.start_date);
    _stats.end_date = normalize_date_filter(_cfg.end_date);
    std::cout << "[INFO] backtest_driver: replay complete, ticks=" << _stats.total_ticks
              << ", bars=" << _stats.total_bars << std::endl;
    co_return;
}

bool backtest_driver::replay_event(const sstring& instrument_id, const replay_record& record) {
    market_event event;
    event.type = record.type;
    event.instrument_id = instrument_id;
    event.exchange_id = record.exchange_id;
    event.calendar_date = record.calendar_date;
    event.timeframe = record.timeframe;
    event.trading_day = record.trading_day;
    event.timestamp = record.time_str;
    event.last_price = record.last_price;
    event.open_price = record.open_price;
    event.high_price = record.high_price;
    event.low_price = record.low_price;
    event.close_price = record.close_price;
    event.volume = record.volume;
    event.turnover = record.turnover;
    event.open_interest = record.open_interest;
    event.require_subscriber = true;
    if (record.type == market_event_type::tick) {
        event.raw_md.instrument_id = instrument_id;
        event.raw_md.update_time = record.time_str;
        event.raw_md.last_price = record.last_price;
        event.raw_md.volume = record.volume;
        event.raw_md.last_volume = record.volume;
        event.raw_md.turnover = record.turnover;
        event.raw_md.open_interest = record.open_interest;
        event.raw_md.last_open_interest = record.open_interest;
        event.raw_md.open_price = record.open_price;
        event.raw_md.highest_price = record.high_price;
        event.raw_md.lowest_price = record.low_price;
        event.raw_md.average_price = record.last_price;
        event.raw_md.trading_day = record.trading_day;
    }
    return _event_bus->enqueue(event);
}

} // namespace seastar::xtrader
