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
#include <seastar/xtrader/offline_simulated_execution.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B3: exercise an offline-only match, settlement, and failure boundary.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_simulated_order_request make_request(domain::side direction, domain::offset_flag offset,
                                             int volume, int64_t limit, int64_t maximum) {
    offline_simulated_order_request request{
        .strategy_id = "strategy",
        .instrument = test_instrument(),
        .direction = direction,
        .offset = offset,
        .volume = volume,
        .limit_price_ticks = limit,
        .maximum_price_ticks = maximum,
    };
    if (offset == domain::offset_flag::close) {
        request.close_rule = close_position_rule::today_first;
    }
    return request;
}

offline_simulated_quote quote(uint64_t sequence, int64_t bid, int bid_volume,
                              int64_t ask, int ask_volume) {
    return {
        .sequence = sequence,
        .exchange_id = "XSGE",
        .instrument_id = "rb9999",
        .bid_ticks = bid,
        .bid_volume = bid_volume,
        .ask_ticks = ask,
        .ask_volume = ask_volume,
    };
}

offline_simulated_bar_open bar(uint64_t sequence, int64_t open) {
    return {.sequence = sequence, .exchange_id = "XSGE", .instrument_id = "rb9999", .open_ticks = open};
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_settles_later_quotes_and_fifo_close) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_simulated_execution execution(account, "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 11,
                                           simulated_market_mode::tick_quote, std::nullopt);
    execution.initialize_flat(1000);
    BOOST_REQUIRE(execution.on_quote(quote(1, 98, 2, 100, 2)).trades.empty());
    const auto open = execution.submit(make_request(domain::side::buy, domain::offset_flag::open, 2, 101, 110));
    BOOST_CHECK_EQUAL(open.submitted_after_event, 1);
    BOOST_CHECK_EQUAL(open.id.sequence, 1);

    const auto first = execution.on_quote(quote(2, 98, 2, 100, 1));
    BOOST_REQUIRE_EQUAL(first.trades.size(), 1);
    BOOST_CHECK_EQUAL(first.trades[0].price, 100.0);
    BOOST_CHECK_EQUAL(first.trades[0].volume, 1);
    BOOST_CHECK(first.trades[0].source == simulated_fill_source::best_quote_snapshot);
    BOOST_CHECK(first.trades[0].is_approximate);
    BOOST_REQUIRE(first.trades[0].commission_units);
    BOOST_REQUIRE(first.trades[0].margin_change_units);
    BOOST_REQUIRE(first.trades[0].close_profit_units);
    BOOST_CHECK_EQUAL(*first.trades[0].commission_units, 3);
    BOOST_CHECK_EQUAL(*first.trades[0].margin_change_units, 100);
    BOOST_CHECK_EQUAL(*first.trades[0].close_profit_units, 0);
    BOOST_CHECK(first.order_updates[0].status == simulated_order_status::partially_filled);
    const auto second = execution.on_quote(quote(3, 98, 2, 99, 1));
    BOOST_REQUIRE_EQUAL(second.trades.size(), 1);
    BOOST_CHECK(second.order_updates[0].status == simulated_order_status::filled);
    auto state = execution.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 2);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 199);
    BOOST_CHECK_EQUAL(state.funds.balance, 994);
    BOOST_CHECK_EQUAL(state.funds.frozen_margin, 0);

    const auto close = execution.submit(make_request(domain::side::sell, domain::offset_flag::close, 2, 100, 110));
    const auto closing = execution.on_quote(quote(4, 102, 2, 103, 2));
    BOOST_REQUIRE_EQUAL(closing.trades.size(), 1);
    BOOST_CHECK(closing.trades[0].order_id == close.id);
    BOOST_CHECK_EQUAL(closing.trades[0].volume, 2);
    BOOST_CHECK_EQUAL(closing.trades[0].price, 102.0);
    BOOST_REQUIRE(closing.trades[0].commission_units);
    BOOST_REQUIRE(closing.trades[0].margin_change_units);
    BOOST_REQUIRE(closing.trades[0].close_profit_units);
    BOOST_CHECK_EQUAL(*closing.trades[0].commission_units, 8);
    BOOST_CHECK_EQUAL(*closing.trades[0].margin_change_units, -199);
    BOOST_CHECK_EQUAL(*closing.trades[0].close_profit_units, 50);
    state = execution.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.commission, 14);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 50);
    BOOST_CHECK_EQUAL(state.funds.balance, 1036);
    BOOST_CHECK_EQUAL(state.funds.available, 1036);
    BOOST_CHECK(execution.get_order(close.id, "strategy").status == simulated_order_status::filled);
    BOOST_CHECK(execution.can_submit_orders());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_cancel_releases_only_unfilled_reservations) {
    const auto instrument = test_instrument();
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 12,
                                           simulated_market_mode::tick_quote, std::nullopt);
    execution.initialize_flat(2000);
    const auto order = execution.submit(make_request(domain::side::buy, domain::offset_flag::open, 3, 101, 110));
    const auto first = execution.on_quote(quote(1, 99, 1, 100, 1));
    BOOST_REQUIRE_EQUAL(first.trades.size(), 1);
    BOOST_CHECK_THROW(execution.cancel(order.id, "other"), std::invalid_argument);
    BOOST_CHECK(execution.is_consistent());
    const auto canceled = execution.cancel(order.id, "strategy");
    BOOST_CHECK(canceled.status == simulated_order_status::canceled);
    BOOST_CHECK_EQUAL(canceled.filled_volume, 1);
    const auto state = execution.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 1);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 100);
    BOOST_CHECK_EQUAL(state.funds.frozen_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.funds.available, 1897);
    BOOST_CHECK(execution.on_quote(quote(2, 99, 2, 100, 2)).trades.empty());
    BOOST_CHECK_THROW(execution.cancel(order.id, "strategy"), std::logic_error);
    BOOST_CHECK(execution.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_uses_explicit_bar_open_limits) {
    const auto instrument = test_instrument();
    const simulated_bar_liquidity liquidity{.buy_volume = 1, .sell_volume = 1};
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 13,
                                           simulated_market_mode::next_bar_open, liquidity);
    execution.initialize_flat(1000);
    BOOST_REQUIRE(execution.on_bar_open(bar(1, 100)).trades.empty());
    const auto order = execution.submit(make_request(domain::side::buy, domain::offset_flag::open, 2, 105, 110));
    auto fill = execution.on_bar_open(bar(2, 102));
    BOOST_REQUIRE_EQUAL(fill.trades.size(), 1);
    BOOST_CHECK(fill.trades[0].order_id == order.id);
    BOOST_CHECK(fill.trades[0].source == simulated_fill_source::bar_open_approximation);
    BOOST_CHECK(fill.trades[0].is_approximate);
    fill = execution.on_bar_open(bar(3, 101));
    BOOST_REQUIRE_EQUAL(fill.trades.size(), 1);
    BOOST_CHECK(fill.order_updates[0].status == simulated_order_status::filled);

    execution.submit(make_request(domain::side::sell, domain::offset_flag::close_today, 2, 100, 110));
    fill = execution.on_bar_open(bar(4, 103));
    BOOST_REQUIRE_EQUAL(fill.trades.size(), 1);
    BOOST_CHECK(fill.order_updates[0].status == simulated_order_status::partially_filled);
    fill = execution.on_bar_open(bar(5, 104));
    BOOST_REQUIRE_EQUAL(fill.trades.size(), 1);
    const auto state = execution.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 40);
    BOOST_CHECK_EQUAL(state.funds.commission, 20);
    BOOST_CHECK_EQUAL(state.funds.balance, 1020);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_rolls_back_whole_market_event_on_settlement_failure) {
    const auto instrument = test_instrument();
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 14,
                                           simulated_market_mode::tick_quote, std::nullopt);
    execution.initialize_flat(2000);
    const auto first = execution.submit(make_request(domain::side::sell, domain::offset_flag::open, 1, 95, 110));
    const auto second = execution.submit(make_request(domain::side::sell, domain::offset_flag::open, 1, 95, 100));
    const auto before = execution.snapshot(instrument);
    BOOST_CHECK_THROW(static_cast<void>(execution.on_quote(quote(1, 101, 2, 102, 2))), std::invalid_argument);
    const auto after = execution.snapshot(instrument);
    BOOST_CHECK_EQUAL(after.position.total.short_today, before.position.total.short_today);
    BOOST_CHECK_EQUAL(after.funds.balance, before.funds.balance);
    BOOST_CHECK_EQUAL(after.funds.position_margin, before.funds.position_margin);
    BOOST_CHECK_EQUAL(after.funds.frozen_margin, before.funds.frozen_margin);
    BOOST_CHECK_EQUAL(after.funds.frozen_commission, before.funds.frozen_commission);
    BOOST_CHECK_EQUAL(execution.get_order(first.id, "strategy").filled_volume, 0);
    BOOST_CHECK_EQUAL(execution.get_order(second.id, "strategy").filled_volume, 0);
    BOOST_CHECK(!execution.is_consistent());
    BOOST_CHECK(!execution.can_submit_orders());
    BOOST_CHECK_THROW(execution.cancel(first.id, "strategy"), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_rejects_bad_requests_without_ghost_orders) {
    auto other_hedge = make_terms();
    other_hedge.instrument.hedge = domain::hedge_flag::hedge;
    other_hedge.price_tick = 0.5;
    BOOST_CHECK_THROW(static_cast<void>(offline_simulated_execution{
                          test_account(), "synthetic-unit", "strategy", {make_terms(), other_hedge},
                          offline_cost_allocation::fifo, 15, simulated_market_mode::tick_quote, std::nullopt}),
                      std::invalid_argument);
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 15,
                                           simulated_market_mode::tick_quote, std::nullopt);
    BOOST_CHECK_THROW(static_cast<void>(execution.on_quote(quote(1, 99, 1, 100, 1))), std::logic_error);
    BOOST_CHECK(execution.is_consistent());
    execution.initialize_flat(1000);
    auto invalid = make_request(domain::side::buy, domain::offset_flag::open, 1, 100, 110);
    invalid.strategy_id = "other";
    BOOST_CHECK_THROW(execution.submit(invalid), std::invalid_argument);
    invalid.strategy_id = "strategy";
    invalid.maximum_price_ticks = 99;
    BOOST_CHECK_THROW(execution.submit(invalid), std::invalid_argument);
    invalid.maximum_price_ticks = 110;
    invalid.instrument.instrument_id = "unknown";
    BOOST_CHECK_THROW(execution.submit(invalid), std::invalid_argument);
    invalid.instrument = test_instrument();
    invalid.volume = 100;
    BOOST_CHECK_THROW(execution.submit(invalid), std::invalid_argument);
    const auto order = execution.submit(make_request(domain::side::buy, domain::offset_flag::open, 1, 100, 110));
    BOOST_CHECK_EQUAL(order.id.sequence, 1);
    BOOST_CHECK(execution.is_consistent());
    BOOST_CHECK_THROW(static_cast<void>(execution.on_quote(quote(0, 99, 1, 100, 1))), std::invalid_argument);
    BOOST_CHECK(!execution.is_consistent());
    BOOST_CHECK_EQUAL(execution.get_order(order.id, "strategy").filled_volume, 0);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_execution_normalizes_per_instrument_tick_sizes) {
    auto second = make_terms("cu9999");
    second.price_tick = 0.2;
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms(), second},
                                           offline_cost_allocation::fifo, 16,
                                           simulated_market_mode::tick_quote, std::nullopt);
    execution.initialize_flat(1000);
    auto request = make_request(domain::side::buy, domain::offset_flag::open, 1, 100, 110);
    request.instrument = test_instrument("cu9999");
    const auto order = execution.submit(request);
    const auto raw = std::nextafter(99.0 * 0.2, -std::numeric_limits<double>::infinity());
    BOOST_CHECK_THROW(static_cast<void>(execution.normalize_price(
                          "XSGE", "cu9999", raw, offline_price_normalization::exact)), std::invalid_argument);
    const auto normalized = execution.normalize_price("XSGE", "cu9999", raw,
                                                       offline_price_normalization::nearest_one_ulp);
    BOOST_CHECK_EQUAL(normalized.price_ticks, 99);
    BOOST_CHECK(normalized.adjusted);
    auto market = quote(1, 98, 1, 99, 1);
    market.instrument_id = "cu9999";
    market.ask_ticks = normalized.price_ticks;
    const auto result = execution.on_quote(market);
    BOOST_REQUIRE_EQUAL(result.trades.size(), 1);
    BOOST_CHECK(result.trades[0].order_id == order.id);
    BOOST_CHECK_EQUAL(result.trades[0].price, 99.0 * 0.2);
    BOOST_CHECK_EQUAL(execution.snapshot(request.instrument).position.total.long_today, 1);
    BOOST_CHECK(execution.is_consistent());
}
