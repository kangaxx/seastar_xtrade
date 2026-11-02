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
#include <seastar/xtrader/historical_data_manager.hh>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iostream>
#include <string_view>

namespace seastar::xtrader {

namespace {

sstring to_lower_copy(std::string_view value) {
    sstring result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

sstring normalize_trading_day(const std::string& datetime) {
    const auto pos = datetime.find(' ');
    const auto date = pos == std::string::npos ? datetime : datetime.substr(0, pos);

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

    if (parts.size() < 2) {
        return {};
    }

    auto pad2 = [] (std::string value) {
        if (value.size() == 1) {
            value.insert(value.begin(), '0');
        }
        return value;
    };

    std::string result = pad2(parts[0]) + ":" + pad2(parts[1]) + ":";
    result += parts.size() > 2 ? pad2(parts[2]) : "00";
    return sstring(std::move(result));
}

} // namespace

// ==================== execution_simulator ====================

execution_simulator::execution_simulator(const backtest_config& cfg)
    : _cfg(cfg)
{}

domain::trade_report execution_simulator::simulate_fill(
    const domain::order_request& request,
    const market_event& event,
    double& out_slippage) {
    domain::trade_report report;
    report.instrument_id = request.instrument_id;
    report.order_sys_id = "bt-" + request.instrument_id;
    report.direction = request.direction;
    report.offset = request.offset;
    report.volume = request.volume;
    report.trade_time = event.timestamp;
    report.trading_day = event.trading_day;

    double base_price = event.last_price;
    if (_cfg.slippage_ticks > 0.0) {
        double slip = _cfg.slippage_ticks * _cfg.price_tick;
        if (request.direction == domain::side::buy) {
            base_price += slip;
        } else {
            base_price -= slip;
        }
    }

    report.price = base_price;

    double slip_amount = std::abs(base_price - event.last_price)
        * request.volume * _cfg.volume_multiple;
    out_slippage = slip_amount;

    report.commission = _cfg.fee_per_lot * request.volume;

    return report;
}

domain::order_status execution_simulator::simulate_limit_order(
    const domain::order_request& request,
    const market_event& event) {
    if (request.direction == domain::side::buy) {
        if (event.last_price <= request.price) {
            return domain::order_status::filled;
        }
        if (event.low_price > 0 && event.low_price <= request.price) {
            return domain::order_status::filled;
        }
    } else {
        if (event.last_price >= request.price) {
            return domain::order_status::filled;
        }
        if (event.high_price > 0 && event.high_price >= request.price) {
            return domain::order_status::filled;
        }
    }
    return domain::order_status::accepted;
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
    if (_started) {
        return make_ready_future<>();
    }
    _started = true;
    return make_ready_future<>();
}

future<> backtest_driver::stop() {
    _started = false;
    return make_ready_future<>();
}

future<> backtest_driver::load_data(const sstring& instrument_id) {
    std::cout << "[INFO] backtest_driver: loading data for "
              << instrument_id << " from " << _cfg.data_root << std::endl;

    historical_data_manager manager(std::filesystem::path(std::string(_cfg.data_root)));
    std::string warning;
    const auto datasets = manager.scan_datasets(&warning);

    const auto instrument_key = to_lower_copy(instrument_id);
    const auto dataset_it = std::find_if(datasets.begin(), datasets.end(),
        [&instrument_key](const dataset_manifest& dataset) {
            return to_lower_copy(dataset.symbol) == instrument_key;
        });

    if (dataset_it != datasets.end()) {
        std::vector<historical_bar> bars;
        if (manager.load_bars(*dataset_it, bars, &warning) && !bars.empty()) {
            std::vector<tick_record> ticks;
            ticks.reserve(bars.size());

            int64_t timestamp_nanos = 0;
            for (const auto& bar : bars) {
                auto trading_day = normalize_trading_day(bar.datetime);
                auto time_str = normalize_time_str(bar.datetime);
                if (trading_day.empty() || time_str.empty()) {
                    continue;
                }

                ticks.emplace_back(tick_record{
                    .timestamp_nanos = timestamp_nanos,
                    .trading_day = std::move(trading_day),
                    .time_str = std::move(time_str),
                    .last_price = bar.close,
                    .open_price = bar.open,
                    .high_price = bar.high,
                    .low_price = bar.low,
                    .close_price = bar.close,
                    .volume = static_cast<int>(bar.volume),
                    .turnover = bar.close * bar.volume,
                    .open_interest = bar.open_interest,
                });
                timestamp_nanos += 60'000'000'000LL;
            }

            if (!ticks.empty()) {
                _tick_data[instrument_id] = std::move(ticks);
                _stats.total_ticks += _tick_data[instrument_id].size();

                std::cout << "[INFO] backtest_driver: loaded "
                          << _tick_data[instrument_id].size()
                          << " rows from manifest-backed dataset for "
                          << instrument_id << std::endl;
                return make_ready_future<>();
            }
        }
    }

    if (!warning.empty()) {
        std::cout << "[WARN] backtest_driver: " << warning
                  << "; falling back to embedded sample ticks for "
                  << instrument_id << std::endl;
    }

    std::vector<tick_record> ticks;
    ticks.emplace_back(tick_record{
        .timestamp_nanos = 0,
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
    ticks.emplace_back(tick_record{
        .timestamp_nanos = 500000000,
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

    _tick_data[instrument_id] = std::move(ticks);
    _stats.total_ticks += _tick_data[instrument_id].size();

    std::cout << "[INFO] backtest_driver: loaded "
              << _tick_data[instrument_id].size()
              << " ticks for " << instrument_id << std::endl;

    return make_ready_future<>();
}

future<> backtest_driver::run() {
    std::cout << "[INFO] backtest_driver: starting replay" << std::endl;

    for (auto& [instrument_id, ticks] : _tick_data) {
        for (const auto& tick : ticks) {
            _clock.advance_to(tick.timestamp_nanos);

            if (!_clock.is_trading_time(tick.time_str)) {
                std::cout << "[INFO] backtest_driver: skipping non-trading tick, "
                          << "instrument=" << instrument_id
                          << " time=" << tick.time_str
                          << " day=" << tick.trading_day << std::endl;
                continue;
            }

            replay_tick(instrument_id, tick);
        }
    }

    _stats.end_date = _cfg.end_date;
    _stats.start_date = _cfg.start_date;
    std::cout << "[INFO] backtest_driver: replay complete, ticks="
              << _stats.total_ticks << std::endl;
    return make_ready_future<>();
}

void backtest_driver::replay_tick(const sstring& instrument_id, const tick_record& tick) {
    if (_event_bus) {
        market_event evt;
        evt.type = market_event_type::tick;
        evt.instrument_id = instrument_id;
        evt.trading_day = tick.trading_day;
        evt.timestamp = tick.time_str;
        evt.last_price = tick.last_price;
        evt.open_price = tick.open_price;
        evt.high_price = tick.high_price;
        evt.low_price = tick.low_price;
        evt.close_price = tick.close_price;
        evt.volume = tick.volume;
        evt.turnover = tick.turnover;
        evt.open_interest = tick.open_interest;
        evt.raw_md.instrument_id = instrument_id;
        evt.raw_md.update_time = tick.time_str;
        evt.raw_md.last_price = tick.last_price;
        evt.raw_md.volume = tick.volume;
        evt.raw_md.last_volume = tick.volume;
        evt.raw_md.turnover = tick.turnover;
        evt.raw_md.open_interest = tick.open_interest;
        evt.raw_md.last_open_interest = tick.open_interest;
        evt.raw_md.open_price = tick.open_price;
        evt.raw_md.highest_price = tick.high_price;
        evt.raw_md.lowest_price = tick.low_price;
        evt.raw_md.average_price = tick.last_price;
        evt.raw_md.trading_day = tick.trading_day;

        _event_bus->enqueue(evt);
    }
}

} // namespace seastar::xtrader
