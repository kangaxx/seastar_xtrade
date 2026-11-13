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
#include <seastar/core/gate.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/xtrader/domain_types.hh>
#include <seastar/xtrader/event_bus.hh>
#include <seastar/xtrader/market_data_paths.hh>
#include <seastar/xtrader/offline_bar_timeline.hh>

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace seastar::xtrader {

enum class backtest_data_mode {
    historical,
    embedded_demo,
};

struct backtest_config {
    // Menu setup only; explicit files and strategy are not wired into execution yet.
    // Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
    sstring data_root{market_data_paths{}.source_root().string()};
    std::vector<std::filesystem::path> data_files;
    sstring product_type;
    sstring strategy_id;
    sstring dataset_exchange;
    sstring dataset_timeframe;
    backtest_data_mode data_mode = backtest_data_mode::historical;
    // Inclusive calendar-date filters, not exchange trading days.
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

// B3: historical CSV time meaning and trading-day resolution are explicit.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct backtest_bar_export_options {
    std::optional<offline_bar_timestamp> timestamp_kind;
    int64_t duration_nanos = 0;
    sstring time_domain;
    std::function<sstring(const sstring& exchange, const sstring& instrument,
                          const sstring& calendar_date, const sstring& source_time)> resolve_trading_day;
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
    [[nodiscard]] std::vector<offline_bar_record>
    export_offline_bars(const backtest_bar_export_options& options) const;

    void set_event_bus(event_bus* bus) {
        if (_running) {
            throw std::logic_error("cannot change the event bus during replay");
        }
        _event_bus = bus;
    }
    void set_execution_simulator(execution_simulator* sim) {
        if (sim != nullptr) {
            throw std::logic_error("execution simulation is not connected to replay");
        }
    }

    const backtest_stats& stats() const noexcept { return _stats; }
    const backtest_clock& clock() const noexcept { return _clock; }

private:
    struct replay_record {
        market_event_type type = market_event_type::tick;
        int64_t timestamp_nanos = 0;
        sstring calendar_date;
        sstring trading_day;
        sstring time_str;
        sstring exchange_id;
        sstring timeframe;
        double last_price = 0.0;
        double open_price = 0.0;
        double high_price = 0.0;
        double low_price = 0.0;
        double close_price = 0.0;
        int volume = 0;
        double turnover = 0.0;
        double open_interest = 0.0;
    };

    void load_historical_data(const sstring& instrument_id);
    void load_demo_data(const sstring& instrument_id);
    void store_loaded_data(const sstring& instrument_id, std::vector<replay_record> records);
    future<> run_impl();
    bool replay_event(const sstring& instrument_id, const replay_record& record);

    backtest_config _cfg;
    backtest_clock _clock;
    backtest_stats _stats;
    event_bus* _event_bus = nullptr;

    std::unordered_map<sstring, std::vector<replay_record>> _replay_data;
    std::unordered_set<sstring> _failed_instruments;
    gate _operations;
    size_t _pending_loads = 0;
    bool _started = false;
    bool _stopping = false;
    bool _running = false;
    bool _replay_completed = false;
    bool _replay_failed = false;
};

} // namespace seastar::xtrader
