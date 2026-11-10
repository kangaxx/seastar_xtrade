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
#include <seastar/xtrader/offline_fund_ledger.hh>

#include "xtrader_offline_account_fixture.hh"

#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B2 offline fund ledger regressions with explicit, synthetic monetary units.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_require_explicit_scope_and_reservations) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    auto invalid_account = account;
    invalid_account.trading_day = "20260229";
    BOOST_CHECK_THROW(static_cast<void>(offline_fund_ledger{invalid_account, "unit"}), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(offline_fund_ledger{account, " "}), std::invalid_argument);

    offline_fund_ledger ledger(account, "synthetic-unit");
    const auto order = make_order(account, instrument, "order", domain::side::buy,
                                  domain::offset_flag::open, 3, 10000, 100);
    BOOST_CHECK(!ledger.can_reserve_funds());
    BOOST_CHECK_THROW(static_cast<void>(ledger.snapshot()), std::logic_error);
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::logic_error);
    BOOST_CHECK_THROW(ledger.initialize_flat(-1), std::invalid_argument);
    ledger.initialize_flat(100000);
    BOOST_CHECK_THROW(ledger.initialize_flat(100000), std::logic_error);
    BOOST_CHECK(ledger.can_reserve_funds());
    BOOST_CHECK_EQUAL(ledger.money_unit(), "synthetic-unit");

    auto invalid = order;
    invalid.margin_per_lot.reset();
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = order;
    invalid.position.account.account_id = "other";
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = order;
    invalid.position.direction.reset();
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = order;
    invalid.position.volume = 11;
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);
    invalid = order;
    invalid.position.offset = domain::offset_flag::close_today;
    BOOST_CHECK_THROW(ledger.reserve_order(invalid), std::invalid_argument);

    ledger.reserve_order(order);
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::invalid_argument);
    const auto state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.balance, 100000);
    BOOST_CHECK_EQUAL(state.frozen_margin, 30000);
    BOOST_CHECK_EQUAL(state.frozen_commission, 300);
    BOOST_CHECK_EQUAL(state.available, 69700);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_conserve_partial_cancel_late_fills_and_close) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_fund_ledger ledger(account, "synthetic-unit");
    ledger.initialize_flat(100000);
    const auto order = make_order(account, instrument, "open", domain::side::buy,
                                  domain::offset_flag::open, 3, 10000, 100);
    ledger.reserve_order(order);
    const auto first = make_trade(order, "open-1", 1, 9000, 50, 0);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_fund_trade_result::applied);
    auto state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.balance, 99950);
    BOOST_CHECK_EQUAL(state.position_margin, 9000);
    BOOST_CHECK_EQUAL(state.frozen_margin, 20000);
    BOOST_CHECK_EQUAL(state.frozen_commission, 200);
    BOOST_CHECK_EQUAL(state.available, 70750);

    ledger.confirm_terminal(account, "open", position_terminal_status::canceled, 2);
    ledger.confirm_terminal(account, "open", position_terminal_status::canceled, 2);
    state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.frozen_margin, 10000);
    BOOST_CHECK_EQUAL(state.frozen_commission, 100);
    BOOST_CHECK_EQUAL(state.available, 80850);
    BOOST_CHECK_EQUAL(ledger.orders_awaiting_trades(), 1);
    BOOST_CHECK(!ledger.can_reserve_funds());
    auto blocked = order;
    blocked.position.order_id = "blocked";
    BOOST_CHECK_THROW(ledger.reserve_order(blocked), std::logic_error);

    const auto late = make_trade(order, "open-2", 1, 8000, 40, 0);
    BOOST_REQUIRE(ledger.apply_trade(late) == offline_fund_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_fund_trade_result::duplicate);
    state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.balance, 99910);
    BOOST_CHECK_EQUAL(state.position_margin, 17000);
    BOOST_CHECK_EQUAL(state.frozen_margin, 0);
    BOOST_CHECK_EQUAL(state.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.available, 82910);
    BOOST_CHECK_EQUAL(ledger.orders_awaiting_trades(), 0);
    BOOST_REQUIRE(ledger.can_reserve_funds());

    const auto close = make_order(account, instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 2, 0, 100);
    ledger.reserve_order(close);
    BOOST_REQUIRE(ledger.apply_trade(make_trade(close, "close-1", 1, -9000, 50, 1000))
                  == offline_fund_trade_result::applied);
    state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.balance, 100860);
    BOOST_CHECK_EQUAL(state.position_margin, 8000);
    BOOST_CHECK_EQUAL(state.frozen_commission, 100);
    BOOST_CHECK_EQUAL(state.available, 92760);
    BOOST_REQUIRE(ledger.apply_trade(make_trade(close, "close-2", 1, -8000, 60, -500))
                  == offline_fund_trade_result::applied);
    ledger.confirm_terminal(account, "close", position_terminal_status::filled, 2);
    state = ledger.snapshot();
    BOOST_CHECK_EQUAL(state.balance, 100300);
    BOOST_CHECK_EQUAL(state.position_margin, 0);
    BOOST_CHECK_EQUAL(state.frozen_margin, 0);
    BOOST_CHECK_EQUAL(state.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.commission, 200);
    BOOST_CHECK_EQUAL(state.close_profit, 500);
    BOOST_CHECK_EQUAL(state.available, 100300);
    BOOST_CHECK_EQUAL(ledger.position_margin(instrument, domain::side::buy), 0);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_fail_closed_on_invalid_trade_reports) {
    const auto check_rejection = [] (const auto& corrupt) {
        const auto account = test_account();
        const auto order = make_order(account, test_instrument(), "order", domain::side::buy,
                                      domain::offset_flag::open, 1, 100, 10);
        offline_fund_ledger ledger(account, "unit");
        ledger.initialize_flat(1000);
        ledger.reserve_order(order);
        auto trade = make_trade(order, "trade", 1, 100, 1, 0);
        corrupt(trade);
        const auto before = ledger.snapshot();
        BOOST_CHECK_THROW(ledger.apply_trade(trade), std::runtime_error);
        const auto after = ledger.snapshot();
        BOOST_CHECK_EQUAL(after.balance, before.balance);
        BOOST_CHECK_EQUAL(after.position_margin, before.position_margin);
        BOOST_CHECK_EQUAL(after.frozen_margin, before.frozen_margin);
        BOOST_CHECK_EQUAL(after.frozen_commission, before.frozen_commission);
        BOOST_CHECK_EQUAL(after.available, before.available);
        BOOST_CHECK(!ledger.is_consistent());
        auto another = order;
        another.position.order_id = "another";
        BOOST_CHECK_THROW(ledger.reserve_order(another), std::logic_error);
    };
    check_rejection([] (auto& trade) { trade.commission.reset(); });
    check_rejection([] (auto& trade) { trade.commission = -1; });
    check_rejection([] (auto& trade) { trade.commission = 11; });
    check_rejection([] (auto& trade) { trade.margin_change = 101; });
    check_rejection([] (auto& trade) { trade.margin_change = -1; });
    check_rejection([] (auto& trade) { trade.close_profit = 1; });
    check_rejection([] (auto& trade) { trade.position.volume = 2; });
    check_rejection([] (auto& trade) { trade.position.account.account_id = "other"; });
    check_rejection([] (auto& trade) { trade.position.direction = domain::side::sell; });
    check_rejection([] (auto& trade) { trade.position.price = std::numeric_limits<double>::quiet_NaN(); });
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_reject_conflicting_duplicate_or_terminal_reports) {
    const auto account = test_account();
    const auto order = make_order(account, test_instrument(), "order", domain::side::buy,
                                  domain::offset_flag::open, 2, 100, 10);
    offline_fund_ledger duplicates(account, "unit");
    duplicates.initialize_flat(1000);
    duplicates.reserve_order(order);
    auto trade = make_trade(order, "trade", 1, 100, 5, 0);
    BOOST_REQUIRE(duplicates.apply_trade(trade) == offline_fund_trade_result::applied);
    BOOST_REQUIRE(duplicates.apply_trade(trade) == offline_fund_trade_result::duplicate);
    const auto before = duplicates.snapshot();
    trade.commission = 6;
    BOOST_CHECK_THROW(duplicates.apply_trade(trade), std::runtime_error);
    BOOST_CHECK(!duplicates.is_consistent());
    BOOST_CHECK_EQUAL(duplicates.snapshot().balance, before.balance);
    BOOST_CHECK_EQUAL(duplicates.snapshot().frozen_margin, before.frozen_margin);

    offline_fund_ledger terminal(account, "unit");
    terminal.initialize_flat(1000);
    terminal.reserve_order(order);
    terminal.confirm_terminal(account, "order", position_terminal_status::canceled, 1);
    const auto frozen = terminal.snapshot().frozen_margin;
    BOOST_CHECK_THROW(terminal.confirm_terminal(account, "order", position_terminal_status::canceled, 0),
                      std::runtime_error);
    BOOST_CHECK(!terminal.is_consistent());
    BOOST_CHECK_EQUAL(terminal.snapshot().frozen_margin, frozen);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_isolate_margin_by_instrument_and_direction) {
    const auto account = test_account();
    const auto first_instrument = test_instrument();
    const auto second_instrument = test_instrument("cu9999");
    offline_fund_ledger ledger(account, "unit");
    ledger.initialize_flat(100000);
    const auto first = make_order(account, first_instrument, "long", domain::side::buy,
                                  domain::offset_flag::open, 1, 6000, 0);
    const auto second = make_order(account, second_instrument, "short", domain::side::sell,
                                   domain::offset_flag::open, 1, 7000, 0);
    ledger.reserve_order(first);
    ledger.reserve_order(second);
    ledger.apply_trade(make_trade(first, "trade-1", 1, 6000, 0, 0));
    ledger.apply_trade(make_trade(second, "trade-2", 1, 7000, 0, 0));
    BOOST_CHECK_EQUAL(ledger.position_margin(first_instrument, domain::side::buy), 6000);
    BOOST_CHECK_EQUAL(ledger.position_margin(second_instrument, domain::side::sell), 7000);

    const auto close = make_order(account, first_instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 1, 0, 0);
    ledger.reserve_order(close);
    const auto before = ledger.snapshot();
    BOOST_CHECK_THROW(ledger.apply_trade(make_trade(close, "over-release", 1, -6001, 0, 0)),
                      std::runtime_error);
    BOOST_CHECK(!ledger.is_consistent());
    BOOST_CHECK_EQUAL(ledger.snapshot().position_margin, before.position_margin);
    BOOST_CHECK_EQUAL(ledger.position_margin(first_instrument, domain::side::buy), 6000);
    BOOST_CHECK_EQUAL(ledger.position_margin(second_instrument, domain::side::sell), 7000);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_funds_check_overflow_and_record_losses) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_fund_ledger overflow(account, "unit");
    overflow.initialize_flat(100);
    auto too_large = make_order(account, instrument, "large", domain::side::buy,
                                domain::offset_flag::open, 2, std::numeric_limits<int64_t>::max(), 0);
    BOOST_CHECK_THROW(overflow.reserve_order(too_large), std::overflow_error);
    BOOST_CHECK(overflow.can_reserve_funds());
    BOOST_CHECK_EQUAL(overflow.snapshot().available, 100);

    offline_fund_ledger gain_overflow(account, "unit");
    gain_overflow.initialize_flat(std::numeric_limits<int64_t>::max());
    const auto open = make_order(account, instrument, "open", domain::side::buy,
                                 domain::offset_flag::open, 1, 0, 0);
    gain_overflow.reserve_order(open);
    gain_overflow.apply_trade(make_trade(open, "open-trade", 1, 0, 0, 0));
    const auto close = make_order(account, instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 1, 0, 0);
    gain_overflow.reserve_order(close);
    BOOST_CHECK_THROW(gain_overflow.apply_trade(make_trade(close, "profit", 1, 0, 0, 1)),
                      std::runtime_error);
    BOOST_CHECK(!gain_overflow.is_consistent());
    BOOST_CHECK_EQUAL(gain_overflow.snapshot().balance, std::numeric_limits<int64_t>::max());

    offline_fund_ledger loss(account, "unit");
    loss.initialize_flat(1000);
    const auto funded_open = make_order(account, instrument, "funded-open", domain::side::buy,
                                        domain::offset_flag::open, 1, 100, 0);
    loss.reserve_order(funded_open);
    loss.apply_trade(make_trade(funded_open, "funded-trade", 1, 100, 0, 0));
    const auto funded_close = make_order(account, instrument, "funded-close", domain::side::sell,
                                         domain::offset_flag::close_today, 1, 0, 0);
    loss.reserve_order(funded_close);
    loss.apply_trade(make_trade(funded_close, "loss-trade", 1, -100, 0, -2000));
    BOOST_CHECK_EQUAL(loss.snapshot().balance, -1000);
    BOOST_CHECK_EQUAL(loss.snapshot().available, -1000);
    BOOST_CHECK(!loss.can_reserve_funds());
    too_large.position.order_id = "blocked";
    BOOST_CHECK_THROW(loss.reserve_order(too_large), std::logic_error);
}
