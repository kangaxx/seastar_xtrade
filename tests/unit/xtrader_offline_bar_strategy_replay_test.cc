/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#include <seastar/core/sleep.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/backtest_driver.hh>
#include <seastar/xtrader/offline_bar_strategy_replay.hh>

#include "xtrader_offline_futures_fixture.hh"
#include "xtrader_replay_test_files.hh"

#include <chrono>
#include <stdexcept>
#include <vector>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B3: run a real strategy interface against synthetic offline bars only.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_bar_record make_bar(int64_t timestamp, double open, double close) {
    return {
        .exchange_id = "XSGE",
        .instrument_id = "rb9999",
        .timeframe = "1m",
        .trading_day = "20260102",
        .time_domain = "synthetic-clock",
        .timestamp_nanos = timestamp,
        .duration_nanos = 10,
        .timestamp_kind = offline_bar_timestamp::open,
        .open_price = open,
        .high_price = 110,
        .low_price = 90,
        .close_price = close,
        .volume = 7,
        .open_interest = 50,
    };
}

offline_bar_strategy_config make_config() {
    auto terms = make_terms();
    terms.tick_value_per_lot = 100;
    return {
        .replay = {
            .account = test_account(),
            .money_unit = "synthetic-unit",
            .strategy_id = "strategy",
            .contracts = {terms},
            .cost_allocation = offline_cost_allocation::fifo,
            .price_normalization = offline_price_normalization::exact,
            .bar_liquidity = simulated_bar_liquidity{.buy_volume = 1, .sell_volume = 1},
            .run_id = 41,
            .initial_balance = 10000,
        },
        .instrument_spec = {
            .instrument_id = "rb9999",
            .exchange_id = "XSGE",
            .volume_multiple = 10,
            .price_tick = 1,
            .is_trading = true,
        },
        .maximum_price_ticks = 110,
        .money_units_per_currency = 10,
        .close_rule = close_position_rule::today_first,
        .max_notifications_per_boundary = 16,
        .format_timestamp = [] (int64_t timestamp) {
            switch (timestamp) {
            case 100: return offline_calendar_time{"20260102", "09:00:00"};
            case 110: return offline_calendar_time{"20260102", "09:00:10"};
            case 120: return offline_calendar_time{"20260102", "09:00:20"};
            default: throw std::invalid_argument("unexpected synthetic timestamp");
            }
        },
    };
}

class replay_test_strategy final : public strategy_base {
public:
    replay_test_strategy() : strategy_base({
        .strategy_id = "strategy",
        .strategy_name = "strategy",
        .strategy_type = "offline-test",
        .subscribed_instruments = {"rb9999"},
    }) {}

    seastar::future<> on_init(strategy_context& context) override {
        BOOST_CHECK_EQUAL(context.trading_day, "20260102");
        BOOST_CHECK_EQUAL(context.spec.exchange_id, "XSGE");
        BOOST_CHECK_EQUAL(context.query_position().long_today, 0);
        BOOST_CHECK_THROW(context.query_account(), std::logic_error);
        if (!subscribe_bars) {
            return seastar::make_ready_future<>();
        }
        return context.subscribe_bar_data();
    }

    seastar::future<> on_market_data(strategy_context&, const domain::market_data&) override {
        return seastar::make_exception_future<>(std::runtime_error("unexpected tick"));
    }

    seastar::future<> on_bar(strategy_context& context, const domain::bar_data& bar) override {
        ++bar_count;
        if (bar_count != 1) {
            calls.push_back("bar-2");
            return seastar::make_ready_future<>();
        }
        calls.push_back("bar-1");
        BOOST_CHECK_EQUAL(bar.calendar_date, "20260102");
        BOOST_CHECK_EQUAL(bar.timestamp, expected_time);
        BOOST_CHECK_EQUAL(bar.close_price, 101);
        if (defer_first_bar) {
            waiting = true;
            return release_bar.get_future();
        }
        return context.submit_order({
            .strategy_id = forged_owner ? "other" : "strategy",
            .instrument_id = "rb9999",
            .direction = domain::side::buy,
            .offset = domain::offset_flag::open,
            .hedge = domain::hedge_flag::speculation,
            .volume = 2,
            .price = 105,
            .type = domain::order_type::limit,
        }).then([this] (domain::order_status status) {
            BOOST_CHECK(status == domain::order_status::accepted);
        });
    }

    seastar::future<> on_order_update(strategy_context& context, const domain::order& order) override {
        if (order.status == domain::order_status::accepted) {
            calls.push_back("accepted");
            order_sys_id = order.order_sys_id;
        } else if (order.status == domain::order_status::partially_filled) {
            calls.push_back("partial");
            BOOST_CHECK_EQUAL(order.volume_traded, 1);
            return context.cancel_order(order.order_sys_id).then([] (domain::order_status status) {
                BOOST_CHECK(status == domain::order_status::canceled);
            });
        } else if (order.status == domain::order_status::canceled) {
            calls.push_back("canceled");
        }
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_trade_update(strategy_context& context, const domain::trade_report& trade) override {
        calls.push_back("trade");
        BOOST_CHECK_EQUAL(trade.order_sys_id, order_sys_id);
        BOOST_CHECK_EQUAL(trade.price, 102);
        BOOST_CHECK_EQUAL(trade.volume, 1);
        BOOST_CHECK_EQUAL(trade.trade_time, expected_time);
        BOOST_CHECK_CLOSE(trade.commission, 1.3, 0.00001);
        BOOST_REQUIRE(trade.fill_origin);
        BOOST_CHECK(*trade.fill_origin == domain::trade_fill_origin::simulated_bar_open);
        BOOST_REQUIRE(trade.is_approximate);
        BOOST_CHECK(*trade.is_approximate);
        BOOST_CHECK_EQUAL(context.query_position().long_today, 1);
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_stop(strategy_context& context, const seastar::sstring&) override {
        calls.push_back("stop");
        if (!forged_owner && subscribe_bars && !defer_first_bar) {
            BOOST_CHECK_EQUAL(context.query_position().long_today, 1);
        }
        return seastar::make_ready_future<>();
    }

    bool subscribe_bars = true;
    bool forged_owner = false;
    bool defer_first_bar = false;
    bool waiting = false;
    int bar_count = 0;
    seastar::promise<> release_bar;
    seastar::sstring expected_time = "09:00:10";
    seastar::sstring order_sys_id;
    std::vector<seastar::sstring> calls;
};

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_strategy_replay_routes_order_trade_and_cancel) {
    auto strategy = std::make_shared<replay_test_strategy>();
    auto replay = offline_bar_strategy_replay::create(
        make_config(), strategy, {make_bar(100, 100, 101), make_bar(110, 102, 103)});
    replay->run().get();
    const std::vector<seastar::sstring> expected{"bar-1", "accepted", "partial", "trade", "canceled", "bar-2", "stop"};
    BOOST_REQUIRE_EQUAL(strategy->calls.size(), expected.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(strategy->calls.begin(), strategy->calls.end(), expected.begin(), expected.end());
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
    BOOST_CHECK_EQUAL(replay->active_order_count(), 0);
    BOOST_CHECK_EQUAL(replay->snapshot().position.total.long_today, 1);
    BOOST_CHECK_EQUAL(replay->snapshot().funds.commission, 13);
    BOOST_CHECK_EQUAL(replay->stats().fills, 1);
    BOOST_CHECK_EQUAL(strategy->stats().signal_count, 2);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_strategy_replay_rejects_bad_config_and_foreign_orders) {
    const std::vector<offline_bar_record> bars{make_bar(100, 100, 101), make_bar(110, 102, 103)};
    auto config = make_config();
    config.money_units_per_currency = 0;
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_strategy_replay::create(
                          config, std::make_shared<replay_test_strategy>(), bars)), std::invalid_argument);
    config = make_config();
    config.format_timestamp = [] (int64_t) { return offline_calendar_time{"20260230", "99:00:00"}; };
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_strategy_replay::create(
                          config, std::make_shared<replay_test_strategy>(), bars)), std::invalid_argument);
    config = make_config();
    config.replay.contracts.front().tick_value_per_lot = 10;
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_strategy_replay::create(
                          config, std::make_shared<replay_test_strategy>(), bars)), std::invalid_argument);
    auto strategy = std::make_shared<replay_test_strategy>();
    strategy->forged_owner = true;
    auto replay = offline_bar_strategy_replay::create(make_config(), strategy, bars);
    BOOST_CHECK_THROW(replay->run().get(), std::invalid_argument);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::failed);
    BOOST_CHECK_EQUAL(replay->active_order_count(), 0);
    BOOST_CHECK_EQUAL(replay->stats().submitted_orders, 0);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_strategy_replay_consumes_historical_csv) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE",
        "2026/1/2 9:00,100,110,90,101,7,50\n"
        "2026/1/2 9:01,102,110,90,103,7,50\n");
    backtest_config data_config;
    data_config.data_root = directory.path().string();
    backtest_driver driver(data_config);
    driver.load_data("rb9999").get();
    backtest_bar_export_options export_options{
        .timestamp_kind = offline_bar_timestamp::open,
        .duration_nanos = 60'000'000'000LL,
        .time_domain = "synthetic-naive-local",
        .resolve_trading_day = [] (const auto&, const auto&, const auto& date, const auto&) {
            if (date != "20260102") {
                throw std::invalid_argument("unexpected synthetic trading day");
            }
            return seastar::sstring("20260102");
        },
    };
    const auto bars = driver.export_offline_bars(export_options);
    BOOST_REQUIRE_EQUAL(bars.size(), 2);
    const auto first = bars.front().timestamp_nanos;
    auto config = make_config();
    config.format_timestamp = [first] (int64_t timestamp) {
        if (timestamp == first) {
            return offline_calendar_time{"20260102", "09:00:00"};
        }
        if (timestamp == first + 60'000'000'000LL) {
            return offline_calendar_time{"20260102", "09:01:00"};
        }
        if (timestamp == first + 120'000'000'000LL) {
            return offline_calendar_time{"20260102", "09:02:00"};
        }
        throw std::invalid_argument("unexpected synthetic CSV timestamp");
    };
    auto strategy = std::make_shared<replay_test_strategy>();
    strategy->expected_time = "09:01:00";
    auto replay = offline_bar_strategy_replay::create(config, strategy, bars);
    replay->run().get();
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
    BOOST_CHECK_EQUAL(replay->stats().fills, 1);
    BOOST_CHECK_EQUAL(replay->snapshot().position.total.long_today, 1);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_strategy_replay_requires_subscription) {
    auto strategy = std::make_shared<replay_test_strategy>();
    strategy->subscribe_bars = false;
    auto replay = offline_bar_strategy_replay::create(make_config(), strategy, {make_bar(100, 100, 101)});
    BOOST_CHECK_THROW(replay->run().get(), std::runtime_error);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::failed);
    BOOST_CHECK_EQUAL(replay->stats().open_events, 0);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_strategy_replay_rejects_concurrent_run_without_stopping_first) {
    auto strategy = std::make_shared<replay_test_strategy>();
    strategy->defer_first_bar = true;
    auto replay = offline_bar_strategy_replay::create(
        make_config(), strategy, {make_bar(100, 100, 101), make_bar(110, 102, 103)});
    auto first = replay->run();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!strategy->waiting && std::chrono::steady_clock::now() < deadline) {
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    BOOST_REQUIRE(strategy->waiting);
    BOOST_CHECK_THROW(replay->run().get(), std::logic_error);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::running);
    strategy->release_bar.set_value();
    first.get();
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
}
