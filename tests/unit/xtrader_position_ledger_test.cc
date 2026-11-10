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
#include <seastar/xtrader/position_ledger.hh>

#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
namespace domain = seastar::xtrader::domain;

position_account_scope test_account() {
    return {
        .environment = "offline-a",
        .broker_id = "broker-a",
        .account_id = "account-a",
        .trading_day = "20260102",
    };
}

position_instrument test_instrument() {
    return {
        .exchange_id = "XSGE",
        .instrument_id = "rb9999",
        .hedge = domain::hedge_flag::speculation,
    };
}

position_trade make_trade(const position_order& order, const seastar::sstring& trade_id, int volume) {
    return {
        .account = order.account,
        .trade_id = trade_id,
        .order_id = order.order_id,
        .instrument = order.instrument,
        .direction = order.direction,
        .offset = order.offset,
        .volume = volume,
        .price = 3500.0,
    };
}

position_order open_long_order(const position_account_scope& account,
                               const position_instrument& instrument,
                               const seastar::sstring& order_id, int volume) {
    return {
        .account = account,
        .order_id = order_id,
        .instrument = instrument,
        .direction = domain::side::buy,
        .offset = domain::offset_flag::open,
        .volume = volume,
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_requires_complete_explicit_snapshot) {
    auto account = test_account();
    auto invalid_account = account;
    invalid_account.account_id = "";
    BOOST_CHECK_THROW(static_cast<void>(position_ledger{invalid_account}), std::invalid_argument);
    invalid_account = account;
    invalid_account.trading_day = "20260229";
    BOOST_CHECK_THROW(static_cast<void>(position_ledger{invalid_account}), std::invalid_argument);
    invalid_account.trading_day = "20240229";
    BOOST_CHECK_NO_THROW(static_cast<void>(position_ledger{invalid_account}));

    position_ledger ledger(account);
    const auto instrument = test_instrument();
    auto order = open_long_order(account, instrument, "order-1", 1);
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::logic_error);
    BOOST_CHECK_THROW(static_cast<void>(ledger.snapshot(instrument)), std::logic_error);

    auto invalid_instrument = instrument;
    invalid_instrument.hedge.reset();
    BOOST_CHECK_THROW(ledger.seed_position(invalid_instrument, {}), std::invalid_argument);
    BOOST_CHECK_THROW(ledger.seed_position(instrument, {.long_today = -1}), std::invalid_argument);
    ledger.seed_position(instrument, {.long_today = 3});
    BOOST_CHECK_THROW(ledger.seed_position(instrument, {}), std::invalid_argument);
    ledger.complete_initial_snapshot_without_open_orders();
    BOOST_REQUIRE(ledger.can_reserve_positions());
    BOOST_CHECK_THROW(ledger.seed_position(instrument, {}), std::logic_error);
    BOOST_CHECK_THROW(ledger.complete_initial_snapshot_without_open_orders(), std::logic_error);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).total.long_today, 3);

    order.direction.reset();
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::invalid_argument);
    order.direction = domain::side::buy;
    order.account.account_id = "different-account";
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::invalid_argument);
    BOOST_REQUIRE(ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_freeze_partial_cancel_and_late_trade) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    auto another_instrument = instrument;
    another_instrument.instrument_id = "cu9999";
    auto another_hedge = instrument;
    another_hedge.hedge = domain::hedge_flag::hedge;

    position_ledger ledger(account);
    ledger.seed_position(instrument, {.long_today = 3, .long_yesterday = 2, .short_today = 1});
    ledger.seed_position(another_instrument, {.long_today = 5});
    ledger.seed_position(another_hedge, {.long_today = 8});
    ledger.complete_initial_snapshot_without_open_orders();

    auto other_account = account;
    other_account.account_id = "account-b";
    position_ledger other_ledger(other_account);
    other_ledger.seed_position(instrument, {.long_today = 10});
    other_ledger.complete_initial_snapshot_without_open_orders();

    const position_order close_first{
        .account = account,
        .order_id = "close-first",
        .instrument = instrument,
        .direction = domain::side::sell,
        .offset = domain::offset_flag::close,
        .volume = 4,
        .close_rule = close_position_rule::today_first,
    };
    auto no_rule = close_first;
    no_rule.close_rule.reset();
    BOOST_CHECK_THROW(ledger.reserve_order(no_rule), std::invalid_argument);
    ledger.reserve_order(close_first);
    BOOST_CHECK_THROW(ledger.reserve_order(close_first), std::invalid_argument);
    auto state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.frozen.long_today, 3);
    BOOST_REQUIRE_EQUAL(state.frozen.long_yesterday, 1);
    BOOST_REQUIRE_EQUAL(state.available().long_today, 0);
    BOOST_REQUIRE_EQUAL(state.available().long_yesterday, 1);

    position_order close_yesterday{
        .account = account,
        .order_id = "close-yesterday",
        .instrument = instrument,
        .direction = domain::side::sell,
        .offset = domain::offset_flag::close_yesterday,
        .volume = 2,
    };
    BOOST_CHECK_THROW(ledger.reserve_order(close_yesterday), std::invalid_argument);
    close_yesterday.volume = 1;
    ledger.reserve_order(close_yesterday);

    const auto first_trade = make_trade(close_first, "trade-1", 2);
    BOOST_REQUIRE(ledger.apply_trade(first_trade) == position_trade_result::applied);
    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.total.long_today, 1);
    BOOST_REQUIRE_EQUAL(state.frozen.long_today, 1);
    BOOST_REQUIRE_EQUAL(state.frozen.long_yesterday, 2);

    ledger.confirm_terminal(account, close_first.order_id, position_terminal_status::canceled, 3);
    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.frozen.long_today, 1);
    BOOST_REQUIRE_EQUAL(state.frozen.long_yesterday, 1);
    BOOST_REQUIRE_EQUAL(ledger.orders_awaiting_trades(), 1);
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    BOOST_CHECK_THROW(ledger.reserve_order(open_long_order(account, instrument, "blocked", 1)), std::logic_error);

    const auto late_trade = make_trade(close_first, "trade-2", 1);
    BOOST_REQUIRE(ledger.apply_trade(late_trade) == position_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(late_trade) == position_trade_result::duplicate);
    ledger.confirm_terminal(account, close_first.order_id, position_terminal_status::canceled, 3);
    BOOST_REQUIRE(ledger.can_reserve_positions());
    BOOST_REQUIRE_EQUAL(ledger.orders_awaiting_trades(), 0);

    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.total.long_today, 0);
    BOOST_REQUIRE_EQUAL(state.total.long_yesterday, 2);
    BOOST_REQUIRE_EQUAL(state.frozen.long_today, 0);
    BOOST_REQUIRE_EQUAL(state.frozen.long_yesterday, 1);
    BOOST_REQUIRE_EQUAL(state.available().long_yesterday, 1);
    ledger.confirm_terminal(account, close_yesterday.order_id, position_terminal_status::canceled, 0);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).available().long_yesterday, 2);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(another_instrument).total.long_today, 5);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(another_hedge).total.long_today, 8);
    BOOST_REQUIRE_EQUAL(other_ledger.snapshot(instrument).total.long_today, 10);
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_terminal_before_fills_and_short_close) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    position_ledger ledger(account);
    ledger.seed_position(instrument, {.short_today = 2, .short_yesterday = 2});
    ledger.complete_initial_snapshot_without_open_orders();
    const position_order order{
        .account = account,
        .order_id = "close-short",
        .instrument = instrument,
        .direction = domain::side::buy,
        .offset = domain::offset_flag::close,
        .volume = 3,
        .close_rule = close_position_rule::yesterday_first,
    };
    ledger.reserve_order(order);
    auto state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.frozen.short_yesterday, 2);
    BOOST_REQUIRE_EQUAL(state.frozen.short_today, 1);

    ledger.confirm_terminal(account, order.order_id, position_terminal_status::filled, 3);
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    BOOST_REQUIRE_EQUAL(ledger.orders_awaiting_trades(), 1);
    BOOST_REQUIRE(ledger.apply_trade(make_trade(order, "short-trade-1", 2)) == position_trade_result::applied);
    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.total.short_yesterday, 0);
    BOOST_REQUIRE_EQUAL(state.frozen.short_today, 1);
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    BOOST_REQUIRE(ledger.apply_trade(make_trade(order, "short-trade-2", 1)) == position_trade_result::applied);
    ledger.confirm_terminal(account, order.order_id, position_terminal_status::filled, 3);
    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.total.short_today, 1);
    BOOST_REQUIRE_EQUAL(state.frozen.short_today, 0);
    BOOST_REQUIRE(ledger.can_reserve_positions());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_close_rules_and_release_are_explicit) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    position_ledger ledger(account);
    ledger.seed_position(instrument, {.long_today = 2, .long_yesterday = 3});
    ledger.complete_initial_snapshot_without_open_orders();

    position_order close_today{
        .account = account,
        .order_id = "close-today",
        .instrument = instrument,
        .direction = domain::side::sell,
        .offset = domain::offset_flag::close_today,
        .volume = 3,
    };
    BOOST_CHECK_THROW(ledger.reserve_order(close_today), std::invalid_argument);
    close_today.volume = 1;
    ledger.reserve_order(close_today);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_today, 1);
    ledger.confirm_terminal(account, close_today.order_id, position_terminal_status::rejected, 0);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_today, 0);

    const position_order generic_close{
        .account = account,
        .order_id = "close-yesterday-first",
        .instrument = instrument,
        .direction = domain::side::sell,
        .offset = domain::offset_flag::close,
        .volume = 4,
        .close_rule = close_position_rule::yesterday_first,
    };
    ledger.reserve_order(generic_close);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_yesterday, 3);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_today, 1);
    ledger.apply_trade(make_trade(generic_close, "yesterday-first-1", 1));
    ledger.confirm_terminal(account, generic_close.order_id, position_terminal_status::canceled, 2);
    auto state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.frozen.long_today, 0);
    BOOST_REQUIRE_EQUAL(state.frozen.long_yesterday, 1);
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    ledger.apply_trade(make_trade(generic_close, "yesterday-first-2", 1));
    state = ledger.snapshot(instrument);
    BOOST_REQUIRE_EQUAL(state.total.long_today, 2);
    BOOST_REQUIRE_EQUAL(state.total.long_yesterday, 1);
    BOOST_REQUIRE_EQUAL(state.available().long_yesterday, 1);
    BOOST_REQUIRE(ledger.can_reserve_positions());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_trade_ids_are_scoped_by_account_and_exchange) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    auto other_exchange = instrument;
    other_exchange.exchange_id = "XDCE";
    other_exchange.instrument_id = "m9999";

    position_ledger ledger(account);
    ledger.complete_initial_snapshot_without_open_orders();
    const auto first = open_long_order(account, instrument, "order-a", 1);
    const auto second = open_long_order(account, other_exchange, "order-b", 1);
    ledger.reserve_order(first);
    ledger.reserve_order(second);
    BOOST_REQUIRE(ledger.apply_trade(make_trade(first, "same-id", 1)) == position_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(make_trade(second, "same-id", 1)) == position_trade_result::applied);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).total.long_today, 1);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(other_exchange).total.long_today, 1);

    auto other_account = account;
    other_account.account_id = "account-b";
    position_ledger other_ledger(other_account);
    other_ledger.complete_initial_snapshot_without_open_orders();
    const auto third = open_long_order(other_account, instrument, "order-a", 1);
    other_ledger.reserve_order(third);
    BOOST_REQUIRE(other_ledger.apply_trade(make_trade(third, "same-id", 1)) == position_trade_result::applied);
    BOOST_REQUIRE_EQUAL(other_ledger.snapshot(instrument).total.long_today, 1);

    auto conflict = make_trade(first, "same-id", 1);
    conflict.instrument.instrument_id = "another-contract";
    BOOST_CHECK_THROW(ledger.apply_trade(conflict), std::runtime_error);
    BOOST_REQUIRE(!ledger.is_consistent());
    BOOST_REQUIRE(other_ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_conflicting_events_stop_new_orders) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    position_ledger ledger(account);
    ledger.complete_initial_snapshot_without_open_orders();
    const auto order = open_long_order(account, instrument, "open-long", 2);
    ledger.reserve_order(order);
    auto trade = make_trade(order, "duplicate-id", 1);
    BOOST_REQUIRE(ledger.apply_trade(trade) == position_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(trade) == position_trade_result::duplicate);
    trade.price = 3501.0;
    BOOST_CHECK_THROW(ledger.apply_trade(trade), std::runtime_error);
    BOOST_REQUIRE(!ledger.is_consistent());
    BOOST_REQUIRE(!ledger.can_reserve_positions());
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).total.long_today, 1);
    BOOST_CHECK_THROW(ledger.reserve_order(open_long_order(account, instrument, "blocked", 1)), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_rejects_misrouted_and_impossible_reports) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    const auto order = open_long_order(account, instrument, "order", 2);

    position_ledger wrong_account(account);
    wrong_account.complete_initial_snapshot_without_open_orders();
    wrong_account.reserve_order(order);
    auto trade = make_trade(order, "trade-1", 1);
    trade.account.environment = "another-environment";
    BOOST_CHECK_THROW(wrong_account.apply_trade(trade), std::runtime_error);
    BOOST_REQUIRE(!wrong_account.is_consistent());
    BOOST_REQUIRE_EQUAL(wrong_account.snapshot(instrument).total.long_today, 0);

    position_ledger overfill(account);
    overfill.complete_initial_snapshot_without_open_orders();
    overfill.reserve_order(order);
    BOOST_CHECK_THROW(overfill.apply_trade(make_trade(order, "trade-2", 3)), std::runtime_error);
    BOOST_REQUIRE_EQUAL(overfill.snapshot(instrument).total.long_today, 0);
    BOOST_REQUIRE(!overfill.can_reserve_positions());

    position_ledger missing_economics(account);
    missing_economics.complete_initial_snapshot_without_open_orders();
    missing_economics.reserve_order(order);
    trade = make_trade(order, "trade-3", 1);
    trade.price.reset();
    BOOST_CHECK_THROW(missing_economics.apply_trade(trade), std::runtime_error);
    BOOST_REQUIRE(!missing_economics.is_consistent());

    position_ledger zero_price(account);
    zero_price.complete_initial_snapshot_without_open_orders();
    zero_price.reserve_order(order);
    trade = make_trade(order, "trade-4a", 1);
    trade.price = 0.0;
    BOOST_CHECK_THROW(zero_price.apply_trade(trade), std::runtime_error);

    position_ledger negative_price(account);
    negative_price.complete_initial_snapshot_without_open_orders();
    negative_price.reserve_order(order);
    trade = make_trade(order, "trade-4b", 1);
    trade.price = -1.0;
    BOOST_CHECK_THROW(negative_price.apply_trade(trade), std::runtime_error);

    position_ledger inconsistent_terminal(account);
    inconsistent_terminal.complete_initial_snapshot_without_open_orders();
    inconsistent_terminal.reserve_order(order);
    inconsistent_terminal.apply_trade(make_trade(order, "trade-4", 1));
    BOOST_CHECK_THROW(inconsistent_terminal.confirm_terminal(
        account, order.order_id, position_terminal_status::canceled, 0), std::runtime_error);
    BOOST_REQUIRE_EQUAL(inconsistent_terminal.snapshot(instrument).total.long_today, 1);
    BOOST_REQUIRE(!inconsistent_terminal.can_reserve_positions());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_conflicting_terminal_keeps_frozen_positions) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    position_ledger ledger(account);
    ledger.seed_position(instrument, {.long_today = 3});
    ledger.complete_initial_snapshot_without_open_orders();
    const position_order order{
        .account = account,
        .order_id = "close",
        .instrument = instrument,
        .direction = domain::side::sell,
        .offset = domain::offset_flag::close_today,
        .volume = 3,
    };
    ledger.reserve_order(order);
    ledger.confirm_terminal(account, order.order_id, position_terminal_status::canceled, 1);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_today, 1);
    BOOST_CHECK_THROW(ledger.confirm_terminal(account, order.order_id, position_terminal_status::canceled, 2),
                      std::runtime_error);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).total.long_today, 3);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).frozen.long_today, 1);
    BOOST_REQUIRE(!ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_position_ledger_overflow_does_not_mutate_position) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    position_ledger ledger(account);
    ledger.seed_position(instrument, {.long_today = std::numeric_limits<int>::max()});
    ledger.complete_initial_snapshot_without_open_orders();
    const auto order = open_long_order(account, instrument, "overflow", 1);
    ledger.reserve_order(order);
    BOOST_CHECK_THROW(ledger.apply_trade(make_trade(order, "trade", 1)), std::runtime_error);
    BOOST_REQUIRE_EQUAL(ledger.snapshot(instrument).total.long_today, std::numeric_limits<int>::max());
    BOOST_REQUIRE(!ledger.is_consistent());
}
