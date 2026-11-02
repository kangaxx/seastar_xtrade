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
#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/xtrader/domain_types.hh>
#include <seastar/xtrader/event_bus.hh>

#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>

namespace seastar::xtrader {

struct backtest_config {
    sstring data_root = "/data/x_trader_data";
    sstring start_date;
    sstring end_date;
    sstring fee_model = "fixed";
    sstring slippage_model = "none";
    sstring matching_mode = "conservative";

    double fee_per_lot = 5.0;
    double slippage_ticks = 0.0;
    double price_tick = 1.0;
    int volume_multiple = 10;
    double margin_rate = 0.1;
};

struct backtest_stats {
    uint64_t total_ticks = 0;
    uint64_t total_bars = 0;
    uint64_t orders_submitted = 0;
    uint64_t orders_filled = 0;
    uint64_t orders_canceled = 0;
    uint64_t orders_rejected = 0;
    double final_balance = 0.0;
    double total_commission = 0.0;
    double total_slippage = 0.0;
    double total_pnl = 0.0;
    double max_drawdown = 0.0;
    sstring start_date;
    sstring end_date;
};

struct backtest_order_record {
    sstring strategy_id;
    sstring instrument_id;
    sstring order_ref;
    domain::order_request request;
    domain::order_status status = domain::order_status::unknown;
    double filled_price = 0.0;
    int filled_volume = 0;
    domain::trading_account account_at_event;
};

class execution_simulator {
public:
    explicit execution_simulator(const backtest_config& cfg);

    domain::trade_report simulate_fill(
        const domain::order_request& request,
        const market_event& event,
        double& out_slippage);

    domain::order_status simulate_limit_order(
        const domain::order_request& request,
        const market_event& event);

private:
    backtest_config _cfg;
};

class backtest_clock {
public:
    backtest_clock() = default;

    void advance_to(int64_t nanos);
    int64_t now_nanos() const noexcept { return _current_nanos; }

    struct trading_session {
        sstring open_time;
        sstring close_time;
        bool crosses_midnight = false;
    };

    bool is_trading_time(const sstring& time_str) const;
    static trading_session get_session(const sstring& instrument_id);

private:
    int64_t _current_nanos = 0;
};

class backtest_driver {
public:
    explicit backtest_driver(backtest_config cfg);
    ~backtest_driver() = default;

    future<> start();
    future<> stop();

    future<> load_data(const sstring& instrument_id);
    future<> run();

    void set_event_bus(event_bus* bus) { _event_bus = bus; }
    void set_execution_simulator(execution_simulator* sim) { _exec_sim = sim; }

    const backtest_stats& stats() const noexcept { return _stats; }
    const backtest_clock& clock() const noexcept { return _clock; }

private:
    struct tick_record {
        int64_t timestamp_nanos;
        sstring trading_day;
        sstring time_str;
        double last_price;
        double open_price;
        double high_price;
        double low_price;
        double close_price;
        int volume;
        double turnover;
        double open_interest;
    };

    void replay_tick(const sstring& instrument_id, const tick_record& tick);

    backtest_config _cfg;
    backtest_clock _clock;
    backtest_stats _stats;
    event_bus* _event_bus = nullptr;
    execution_simulator* _exec_sim = nullptr;

    std::unordered_map<sstring, std::vector<tick_record>> _tick_data;
    bool _started = false;
};

} // namespace seastar::xtrader
