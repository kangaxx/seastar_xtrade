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
#include <seastar/xtrader/offline_futures_account.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B2: exercise FIFO costs and atomic offline account settlement.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_futures_order make_futures_order(const position_account_scope& account,
        const position_instrument& instrument, const seastar::sstring& order_id,
        domain::side direction, domain::offset_flag offset, int volume, int64_t maximum_price_ticks) {
    auto order = make_order(account, instrument, order_id, direction, offset, volume, 0, 0);
    return {.position = std::move(order.position), .maximum_price_ticks = maximum_price_ticks};
}

offline_futures_trade make_futures_trade(const offline_futures_order& order,
        const seastar::sstring& trade_id, int volume, int64_t price_ticks, double price_tick = 1.0) {
    return {
        .position = {
            .account = order.position.account,
            .trade_id = trade_id,
            .order_id = order.position.order_id,
            .instrument = order.position.instrument,
            .direction = order.position.direction,
            .offset = order.position.offset,
            .volume = volume,
            .price = static_cast<double>(price_ticks) * price_tick,
        },
        .price_ticks = price_ticks,
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_requires_explicit_compatible_terms) {
    const auto account = test_account();
    const auto terms = make_terms();
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_account{
                          account, "synthetic-unit", {}, offline_cost_allocation::fifo}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_account{
                          account, "synthetic-unit", {terms}, static_cast<offline_cost_allocation>(99)}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_account{
                          account, "other-unit", {terms}, offline_cost_allocation::fifo}),
                      std::invalid_argument);
    const std::vector<offline_futures_terms> duplicates{terms, terms};
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_account{
                          account, "synthetic-unit", duplicates, offline_cost_allocation::fifo}),
                      std::invalid_argument);
    offline_futures_account ledger(account, "synthetic-unit", {terms}, offline_cost_allocation::fifo);
    const auto open = make_futures_order(account, test_instrument(), "open", domain::side::buy,
                                         domain::offset_flag::open, 1, 100);
    BOOST_CHECK_THROW(ledger.reserve_order(open), std::logic_error);
    BOOST_CHECK_THROW(ledger.initialize_flat(-1), std::invalid_argument);
    ledger.initialize_flat(1000);
    BOOST_CHECK_THROW(ledger.initialize_flat(1000), std::logic_error);
    auto invalid = open;
    invalid.position.instrument.instrument_id = "unknown";
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = open;
    invalid.maximum_price_ticks = 0;
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = open;
    invalid.position.account.account_id = "other";
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    const auto no_position = make_futures_order(account, test_instrument(), "close", domain::side::sell,
                                                 domain::offset_flag::close_today, 1, 100);
    BOOST_CHECK_THROW(ledger.reserve_order(no_position), std::invalid_argument);
    BOOST_CHECK_EQUAL(ledger.snapshot(test_instrument()).funds.available, 1000);
    BOOST_CHECK(ledger.can_reserve_orders());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_uses_fifo_opening_costs) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_futures_account ledger(account, "synthetic-unit", {make_terms()}, offline_cost_allocation::fifo);
    ledger.initialize_flat(1000);

    const auto first = make_futures_order(account, instrument, "open-a", domain::side::buy,
                                          domain::offset_flag::open, 2, 100);
    ledger.reserve_order(first);
    BOOST_REQUIRE(ledger.apply_trade(make_futures_trade(first, "trade-a", 2, 98))
                  == offline_account_trade_result::applied);
    ledger.confirm_terminal(account, "open-a", position_terminal_status::filled, 2);
    const auto second = make_futures_order(account, instrument, "open-b", domain::side::buy,
                                           domain::offset_flag::open, 1, 110);
    ledger.reserve_order(second);
    ledger.apply_trade(make_futures_trade(second, "trade-b", 1, 102));
    ledger.confirm_terminal(account, "open-b", position_terminal_status::filled, 1);
    BOOST_CHECK_EQUAL(ledger.position_margin(instrument, domain::side::buy), 298);

    const auto first_close = make_futures_order(account, instrument, "close-a", domain::side::sell,
                                                domain::offset_flag::close, 2, 120);
    ledger.reserve_order(first_close);
    ledger.apply_trade(make_futures_trade(first_close, "trade-c", 2, 110));
    ledger.confirm_terminal(account, "close-a", position_terminal_status::filled, 2);
    auto state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 1);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 240);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 102);
    BOOST_CHECK_EQUAL(state.funds.balance, 1222);

    const auto second_close = make_futures_order(account, instrument, "close-b", domain::side::sell,
                                                 domain::offset_flag::close, 1, 120);
    ledger.reserve_order(second_close);
    ledger.apply_trade(make_futures_trade(second_close, "trade-d", 1, 90));
    ledger.confirm_terminal(account, "close-b", position_terminal_status::filled, 1);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.commission, 22);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 120);
    BOOST_CHECK_EQUAL(state.funds.balance, 1098);
    BOOST_CHECK_EQUAL(state.funds.available, 1098);
    BOOST_CHECK(ledger.can_reserve_orders());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_isolates_instruments_and_sides) {
    const auto account = test_account();
    const auto first = test_instrument();
    const auto second = test_instrument("cu9999");
    auto second_terms = make_terms("cu9999");
    second_terms.tick_value_per_lot = 5;
    offline_futures_account ledger(account, "synthetic-unit", {make_terms(), second_terms},
                                   offline_cost_allocation::fifo);
    ledger.initialize_flat(2000);
    const auto short_order = make_futures_order(account, first, "short", domain::side::sell,
                                                domain::offset_flag::open, 1, 110);
    const auto long_order = make_futures_order(account, second, "long", domain::side::buy,
                                               domain::offset_flag::open, 1, 60);
    ledger.reserve_order(short_order);
    ledger.reserve_order(long_order);
    ledger.apply_trade(make_futures_trade(short_order, "short-trade", 1, 101));
    ledger.apply_trade(make_futures_trade(long_order, "long-trade", 1, 50));
    BOOST_CHECK_EQUAL(ledger.position_margin(first, domain::side::sell), 202);
    BOOST_CHECK_EQUAL(ledger.position_margin(second, domain::side::buy), 25);

    const auto close = make_futures_order(account, first, "cover", domain::side::buy,
                                          domain::offset_flag::close_today, 1, 110);
    ledger.reserve_order(close);
    ledger.apply_trade(make_futures_trade(close, "cover-trade", 1, 95));
    auto state = ledger.snapshot(first);
    BOOST_CHECK_EQUAL(state.position.total.short_today, 0);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 60);
    BOOST_CHECK_EQUAL(ledger.position_margin(first, domain::side::sell), 0);
    BOOST_CHECK_EQUAL(ledger.position_margin(second, domain::side::buy), 25);
    BOOST_CHECK_EQUAL(ledger.snapshot(second).position.total.long_today, 1);
    BOOST_CHECK(ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_tracks_cancel_and_late_fills) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_futures_account ledger(account, "synthetic-unit", {make_terms()}, offline_cost_allocation::fifo);
    ledger.initialize_flat(1000);
    const auto open = make_futures_order(account, instrument, "open", domain::side::buy,
                                         domain::offset_flag::open, 3, 100);
    ledger.reserve_order(open);
    const auto first = make_futures_trade(open, "first", 1, 95);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_account_trade_result::applied);
    ledger.confirm_terminal(account, "open", position_terminal_status::canceled, 2);
    ledger.confirm_terminal(account, "open", position_terminal_status::canceled, 2);
    BOOST_CHECK_EQUAL(ledger.orders_awaiting_trades(), 1);
    BOOST_CHECK(!ledger.can_reserve_orders());
    const auto late = make_futures_trade(open, "late", 1, 96);
    BOOST_REQUIRE(ledger.apply_trade(late) == offline_account_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_account_trade_result::duplicate);
    BOOST_CHECK_EQUAL(ledger.orders_awaiting_trades(), 0);
    BOOST_REQUIRE(ledger.can_reserve_orders());
    BOOST_CHECK_EQUAL(ledger.position_margin(instrument, domain::side::buy), 191);

    const auto close = make_futures_order(account, instrument, "close", domain::side::sell,
                                          domain::offset_flag::close, 2, 110);
    ledger.reserve_order(close);
    ledger.apply_trade(make_futures_trade(close, "close-trade", 2, 100));
    const auto state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 90);
    BOOST_CHECK_EQUAL(state.funds.commission, 14);
    BOOST_CHECK_EQUAL(state.funds.balance, 1076);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_keeps_fifo_with_concurrent_closes) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_futures_account ledger(account, "synthetic-unit", {make_terms()}, offline_cost_allocation::fifo);
    ledger.initialize_flat(2000);
    const auto first = make_futures_order(account, instrument, "open-a", domain::side::buy,
                                          domain::offset_flag::open, 3, 100);
    const auto second = make_futures_order(account, instrument, "open-b", domain::side::buy,
                                           domain::offset_flag::open, 2, 100);
    ledger.reserve_order(first);
    ledger.reserve_order(second);
    ledger.apply_trade(make_futures_trade(first, "open-trade-a", 3, 95));
    ledger.apply_trade(make_futures_trade(second, "open-trade-b", 2, 96));
    const auto close_a = make_futures_order(account, instrument, "close-a", domain::side::sell,
                                            domain::offset_flag::close, 3, 110);
    const auto close_b = make_futures_order(account, instrument, "close-b", domain::side::sell,
                                            domain::offset_flag::close, 2, 110);
    ledger.reserve_order(close_a);
    ledger.reserve_order(close_b);
    ledger.apply_trade(make_futures_trade(close_b, "close-b-1", 1, 100));
    ledger.apply_trade(make_futures_trade(close_a, "close-a-1", 1, 100));
    ledger.confirm_terminal(account, "close-a", position_terminal_status::canceled, 2);
    BOOST_CHECK(!ledger.can_reserve_orders());
    ledger.apply_trade(make_futures_trade(close_a, "close-a-late", 1, 100));
    ledger.apply_trade(make_futures_trade(close_b, "close-b-2", 1, 100));
    ledger.confirm_terminal(account, "close-b", position_terminal_status::filled, 2);
    auto state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 1);
    BOOST_CHECK_EQUAL(state.position.frozen.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 96);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 190);

    const auto last = make_futures_order(account, instrument, "last", domain::side::sell,
                                         domain::offset_flag::close, 1, 110);
    ledger.reserve_order(last);
    ledger.apply_trade(make_futures_trade(last, "last-trade", 1, 100));
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.close_profit, 230);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_rejects_bad_reports_atomically) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    const auto run_bad_trade = [&] (const auto& corrupt, bool invalid_argument = false) {
        offline_futures_account ledger(account, "synthetic-unit", {make_terms()}, offline_cost_allocation::fifo);
        ledger.initialize_flat(1000);
        const auto open = make_futures_order(account, instrument, "open", domain::side::buy,
                                             domain::offset_flag::open, 1, 100);
        ledger.reserve_order(open);
        auto trade = make_futures_trade(open, "trade", 1, 98);
        corrupt(trade);
        const auto before = ledger.snapshot(instrument);
        if (invalid_argument) {
            BOOST_CHECK_THROW(static_cast<void>(ledger.apply_trade(trade)), std::invalid_argument);
        } else {
            BOOST_CHECK_THROW(static_cast<void>(ledger.apply_trade(trade)), std::runtime_error);
        }
        const auto after = ledger.snapshot(instrument);
        BOOST_CHECK_EQUAL(after.position.total.long_today, before.position.total.long_today);
        BOOST_CHECK_EQUAL(after.funds.balance, before.funds.balance);
        BOOST_CHECK_EQUAL(after.funds.position_margin, before.funds.position_margin);
        BOOST_CHECK_EQUAL(after.funds.frozen_margin, before.funds.frozen_margin);
        BOOST_CHECK_EQUAL(after.funds.frozen_commission, before.funds.frozen_commission);
        BOOST_CHECK(!ledger.is_consistent());
        BOOST_CHECK(!ledger.can_reserve_orders());
    };
    run_bad_trade([] (auto& trade) { trade.position.price = 99.0; });
    run_bad_trade([] (auto& trade) { trade.position.volume = 2; });
    run_bad_trade([] (auto& trade) { trade.position.account.account_id = "other"; });
    run_bad_trade([] (auto& trade) { trade.position.direction = domain::side::sell; });
    run_bad_trade([] (auto& trade) { trade.position.price = 101.0; trade.price_ticks = 101; }, true);

    offline_futures_account ledger(account, "synthetic-unit", {make_terms()}, offline_cost_allocation::fifo);
    ledger.initialize_flat(1000);
    const auto open = make_futures_order(account, instrument, "open", domain::side::buy,
                                         domain::offset_flag::open, 1, 100);
    ledger.reserve_order(open);
    auto trade = make_futures_trade(open, "trade", 1, 98);
    BOOST_REQUIRE(ledger.apply_trade(trade) == offline_account_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(trade) == offline_account_trade_result::duplicate);
    const auto before = ledger.snapshot(instrument);
    trade.position.price = 99.0;
    trade.price_ticks = 99;
    BOOST_CHECK_THROW(static_cast<void>(ledger.apply_trade(trade)), std::runtime_error);
    BOOST_CHECK_EQUAL(ledger.snapshot(instrument).funds.balance, before.funds.balance);
    BOOST_CHECK_EQUAL(ledger.snapshot(instrument).position.total.long_today, before.position.total.long_today);
    BOOST_CHECK(!ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_futures_account_requires_normalized_tick_prices) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    auto terms = make_terms();
    terms.price_tick = 0.2;
    const offline_futures_economics economics(terms);
    offline_futures_account ledger(account, "synthetic-unit", {terms}, offline_cost_allocation::fifo);
    ledger.initialize_flat(1000);
    const auto order = make_futures_order(account, instrument, "open", domain::side::buy,
                                          domain::offset_flag::open, 1, 100);
    ledger.reserve_order(order);
    auto trade = make_futures_trade(order, "trade", 1, 98, 0.2);
    trade.position.price = economics.price_at_ticks(trade.price_ticks);
    BOOST_REQUIRE(ledger.apply_trade(trade) == offline_account_trade_result::applied);
    BOOST_CHECK_EQUAL(ledger.snapshot(instrument).position.total.long_today, 1);
}
