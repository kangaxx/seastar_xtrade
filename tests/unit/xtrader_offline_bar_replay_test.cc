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
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/sleep.hh>
#include <seastar/xtrader/offline_bar_replay.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B3: exercise an internal replay without enabling a broker or trading entry.
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

offline_bar_replay_config make_config() {
    return {
        .account = test_account(),
        .money_unit = "synthetic-unit",
        .strategy_id = "strategy",
        .contracts = {make_terms()},
        .cost_allocation = offline_cost_allocation::fifo,
        .price_normalization = offline_price_normalization::exact,
        .bar_liquidity = simulated_bar_liquidity{.buy_volume = 1, .sell_volume = 1},
        .run_id = 31,
        .initial_balance = 1000,
    };
}

offline_simulated_order_request make_request(int volume = 1) {
    return {
        .strategy_id = "strategy",
        .instrument = test_instrument(),
        .direction = domain::side::buy,
        .offset = domain::offset_flag::open,
        .volume = volume,
        .limit_price_ticks = 105,
        .maximum_price_ticks = 110,
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_settles_next_open_before_callback) {
    auto replay = offline_bar_replay::create(make_config(), {make_bar(100, 100, 101), make_bar(110, 102, 103)});
    std::optional<simulated_order_id> order;
    offline_bar_replay_context* prior_context = nullptr;
    std::vector<seastar::sstring> calls;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_start = [&] (offline_bar_replay_context& context) {
        prior_context = &context;
        calls.push_back("start");
        return seastar::make_ready_future<>();
    };
    callbacks.on_bar_open = [&] (offline_bar_replay_context& context, const offline_bar_open_notice& notice,
                                 const simulated_match_result& result) {
        if (notice.timestamp_nanos == 100) {
            calls.push_back("open-1");
            BOOST_CHECK(result.trades.empty());
            BOOST_CHECK_EQUAL(notice.open_price, 100);
            return seastar::make_ready_future<>();
        }
        calls.push_back("open-2");
        BOOST_REQUIRE(order);
        BOOST_REQUIRE_EQUAL(result.trades.size(), 1);
        BOOST_CHECK(result.trades[0].order_id == *order);
        BOOST_CHECK_EQUAL(result.trades[0].price, 102);
        BOOST_CHECK(result.trades[0].is_approximate);
        BOOST_CHECK_EQUAL(context.snapshot(test_instrument()).position.total.long_today, 1);
        BOOST_CHECK(context.get_order(*order).status == simulated_order_status::partially_filled);
        BOOST_CHECK(context.cancel(*order).status == simulated_order_status::canceled);
        return seastar::make_ready_future<>();
    };
    callbacks.on_bar_completed = [&] (offline_bar_replay_context& context,
                                      const offline_completed_bar_notice& notice) {
        if (notice.timestamp_nanos == 110) {
            calls.push_back("close-1");
            BOOST_CHECK_EQUAL(notice.bar.close_price, 101);
            const auto submitted = context.submit(make_request(2));
            BOOST_CHECK_EQUAL(submitted.submitted_after_event, 1);
            order = submitted.id;
        } else {
            calls.push_back("close-2");
            BOOST_CHECK_EQUAL(notice.bar.close_price, 103);
        }
        return seastar::make_ready_future<>();
    };
    replay->run(callbacks).get();
    BOOST_REQUIRE_EQUAL(calls.size(), 5);
    BOOST_CHECK_EQUAL(calls[0], "start");
    BOOST_CHECK_EQUAL(calls[1], "open-1");
    BOOST_CHECK_EQUAL(calls[2], "close-1");
    BOOST_CHECK_EQUAL(calls[3], "open-2");
    BOOST_CHECK_EQUAL(calls[4], "close-2");
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
    BOOST_CHECK_EQUAL(replay->stats().open_events, 2);
    BOOST_CHECK_EQUAL(replay->stats().completed_bars, 2);
    BOOST_CHECK_EQUAL(replay->stats().submitted_orders, 1);
    BOOST_CHECK_EQUAL(replay->stats().canceled_orders, 1);
    BOOST_CHECK_EQUAL(replay->stats().fills, 1);
    BOOST_CHECK_EQUAL(replay->active_order_count(), 0);
    BOOST_CHECK_EQUAL(replay->snapshot(test_instrument()).funds.frozen_margin, 0);
    BOOST_REQUIRE(prior_context);
    BOOST_CHECK_THROW(prior_context->submit(make_request()), std::logic_error);
    BOOST_CHECK_THROW(replay->run(callbacks).get(), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_preflights_explicit_config_and_all_prices) {
    const std::vector<offline_bar_record> bars{make_bar(100, 100, 101), make_bar(110, 102, 103)};
    auto config = make_config();
    config.cost_allocation.reset();
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(config, bars)), std::invalid_argument);
    config = make_config();
    config.price_normalization.reset();
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(config, bars)), std::invalid_argument);
    config = make_config();
    config.bar_liquidity.reset();
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(config, bars)), std::invalid_argument);
    config = make_config();
    config.initial_balance = -1;
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(config, bars)), std::invalid_argument);
    auto bad_bars = bars;
    bad_bars[1].trading_day = "20260103";
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(make_config(), bad_bars)), std::invalid_argument);
    bad_bars = bars;
    bad_bars[1].high_price = 109.5;
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(make_config(), bad_bars)), std::invalid_argument);
    bad_bars = bars;
    bad_bars[1].instrument_id = "unknown";
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(make_config(), bad_bars)), std::invalid_argument);

    auto replay = offline_bar_replay::create(make_config(), bars);
    offline_bar_replay_callbacks missing;
    BOOST_CHECK_THROW(replay->run(missing).get(), std::invalid_argument);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::ready);
    missing.on_bar_open = [] (auto&, const auto&, const auto&) { return seastar::make_ready_future<>(); };
    missing.on_bar_completed = [] (auto&, const auto&) { return seastar::make_ready_future<>(); };
    replay->run(missing).get();
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_reports_and_applies_price_normalization) {
    auto bar = make_bar(100, std::nextafter(100.0, std::numeric_limits<double>::infinity()), 101);
    BOOST_CHECK_THROW(static_cast<void>(offline_bar_replay::create(make_config(), {bar})), std::invalid_argument);
    auto config = make_config();
    config.price_normalization = offline_price_normalization::nearest_one_ulp;
    auto replay = offline_bar_replay::create(config, {bar});
    BOOST_CHECK_EQUAL(replay->stats().normalized_price_fields, 1);
    offline_bar_replay_callbacks callbacks;
    callbacks.on_bar_open = [] (auto&, const auto& notice, const auto&) {
        BOOST_CHECK_EQUAL(notice.open_price, 100);
        return seastar::make_ready_future<>();
    };
    callbacks.on_bar_completed = [] (auto&, const auto& notice) {
        BOOST_CHECK_EQUAL(notice.bar.open_price, 100);
        return seastar::make_ready_future<>();
    };
    replay->run(callbacks).get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_aborts_after_callback_failure) {
    auto replay = offline_bar_replay::create(make_config(), {make_bar(100, 100, 101)});
    offline_bar_replay_context* prior_context = nullptr;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_start = [&] (offline_bar_replay_context& context) {
        prior_context = &context;
        context.submit(make_request());
        return seastar::make_exception_future<>(std::runtime_error("synthetic strategy failure"));
    };
    callbacks.on_bar_open = [] (auto&, const auto&, const auto&) { return seastar::make_ready_future<>(); };
    callbacks.on_bar_completed = [] (auto&, const auto&) { return seastar::make_ready_future<>(); };
    BOOST_CHECK_THROW(replay->run(callbacks).get(), std::runtime_error);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::failed);
    BOOST_CHECK_EQUAL(replay->stats().open_events, 0);
    BOOST_CHECK_EQUAL(replay->active_order_count(), 1);
    BOOST_CHECK(replay->snapshot(test_instrument()).funds.frozen_margin > 0);
    BOOST_REQUIRE(prior_context);
    BOOST_CHECK_THROW(prior_context->submit(make_request()), std::logic_error);
    BOOST_CHECK_THROW(replay->run(callbacks).get(), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_waits_for_async_callback_before_next_open) {
    auto replay = offline_bar_replay::create(make_config(), {make_bar(100, 100, 101), make_bar(110, 102, 103)});
    seastar::promise<> release_callback;
    bool waiting = false;
    size_t opens = 0;
    size_t fills = 0;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_bar_open = [&] (auto&, const auto&, const simulated_match_result& result) {
        ++opens;
        fills += result.trades.size();
        return seastar::make_ready_future<>();
    };
    callbacks.on_bar_completed = [&] (offline_bar_replay_context& context, const offline_completed_bar_notice& bar) {
        if (bar.timestamp_nanos != 110) {
            return seastar::make_ready_future<>();
        }
        waiting = true;
        return release_callback.get_future().then([&context] {
            context.submit(make_request());
        });
    };
    auto completed = replay->run(callbacks);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!waiting && std::chrono::steady_clock::now() < deadline) {
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    BOOST_REQUIRE(waiting);
    BOOST_CHECK_EQUAL(opens, 1);
    BOOST_CHECK(!completed.available());
    release_callback.set_value();
    completed.get();
    BOOST_CHECK_EQUAL(opens, 2);
    BOOST_CHECK_EQUAL(fills, 1);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_stop_waits_for_callback_and_blocks_new_orders) {
    auto replay = offline_bar_replay::create(make_config(), {make_bar(100, 100, 101)});
    seastar::promise<> release_callback;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_start = [&] (offline_bar_replay_context& context) {
        return release_callback.get_future().then([&context] {
            BOOST_CHECK_THROW(context.submit(make_request()), std::logic_error);
        });
    };
    callbacks.on_bar_open = [] (auto&, const auto&, const auto&) { return seastar::make_ready_future<>(); };
    callbacks.on_bar_completed = [] (auto&, const auto&) { return seastar::make_ready_future<>(); };
    auto completed = replay->run(callbacks);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::running);
    replay->request_stop();
    BOOST_CHECK(!completed.available());
    release_callback.set_value();
    BOOST_CHECK_THROW(completed.get(), std::runtime_error);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::stopped);
    BOOST_CHECK_EQUAL(replay->stats().open_events, 0);
    BOOST_CHECK_EQUAL(replay->stats().submitted_orders, 0);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_replay_finish_can_cancel_but_not_submit) {
    auto replay = offline_bar_replay::create(make_config(), {make_bar(100, 100, 101)});
    std::optional<simulated_order_id> order;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_start = [&] (offline_bar_replay_context& context) {
        auto request = make_request();
        request.limit_price_ticks = 99;
        order = context.submit(request).id;
        return seastar::make_ready_future<>();
    };
    callbacks.on_bar_open = [] (auto&, const auto&, const auto&) { return seastar::make_ready_future<>(); };
    callbacks.on_bar_completed = [] (auto&, const auto&) { return seastar::make_ready_future<>(); };
    callbacks.on_finish = [&] (offline_bar_replay_context& context) {
        BOOST_CHECK_THROW(context.submit(make_request()), std::logic_error);
        BOOST_REQUIRE(order);
        BOOST_CHECK(context.cancel(*order).status == simulated_order_status::canceled);
        return seastar::make_ready_future<>();
    };
    replay->run(callbacks).get();
    BOOST_CHECK_EQUAL(replay->active_order_count(), 0);
    BOOST_CHECK(replay->state() == offline_bar_replay_state::completed);
}
