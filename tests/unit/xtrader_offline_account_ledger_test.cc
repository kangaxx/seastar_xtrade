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
#include <seastar/xtrader/offline_account_ledger.hh>

#include "xtrader_offline_account_fixture.hh"

#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

} // namespace

// B2: offline position and fund state must commit or reject together.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_offline_account_reservations_are_atomic) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_account_ledger ledger(account, "unit");
    BOOST_CHECK_THROW(ledger.initialize_flat(-1), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(ledger.snapshot(instrument)), std::logic_error);
    ledger.initialize_flat(1000);
    BOOST_CHECK_THROW(ledger.initialize_flat(1000), std::logic_error);
    BOOST_REQUIRE(ledger.can_reserve_orders());

    const auto too_expensive = make_order(account, instrument, "too-expensive", domain::side::buy,
                                          domain::offset_flag::open, 2, 600, 0);
    BOOST_CHECK_THROW(ledger.reserve_order(too_expensive), std::invalid_argument);
    auto state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(state.position.frozen.long_today, 0);
    BOOST_CHECK_EQUAL(state.funds.frozen_margin, 0);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.funds.available, 1000);

    const auto no_position = make_order(account, instrument, "no-position", domain::side::sell,
                                        domain::offset_flag::close_today, 1, 0, 50);
    BOOST_CHECK_THROW(ledger.reserve_order(no_position), std::invalid_argument);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.funds.available, 1000);
    BOOST_CHECK(ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_account_partial_cancel_late_trade_and_close) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_account_ledger ledger(account, "unit");
    ledger.initialize_flat(1000);
    const auto open = make_order(account, instrument, "open", domain::side::buy,
                                 domain::offset_flag::open, 3, 100, 10);
    ledger.reserve_order(open);
    auto state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.funds.frozen_margin, 300);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 30);
    BOOST_CHECK_EQUAL(state.funds.available, 670);

    const auto first = make_trade(open, "open-1", 1, 90, 5, 0);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_account_trade_result::applied);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 1);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 90);
    BOOST_CHECK_EQUAL(state.funds.available, 685);

    ledger.confirm_terminal(account, "open", position_terminal_status::canceled, 2);
    BOOST_CHECK_EQUAL(ledger.orders_awaiting_trades(), 1);
    BOOST_CHECK(!ledger.can_reserve_orders());
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.funds.frozen_margin, 100);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 10);
    BOOST_CHECK_EQUAL(state.funds.available, 795);

    const auto late = make_trade(open, "open-2", 1, 80, 4, 0);
    BOOST_REQUIRE(ledger.apply_trade(late) == offline_account_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(first) == offline_account_trade_result::duplicate);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 2);
    BOOST_CHECK_EQUAL(state.funds.balance, 991);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 170);
    BOOST_CHECK_EQUAL(state.funds.available, 821);
    BOOST_REQUIRE(ledger.can_reserve_orders());

    const auto too_many = make_order(account, instrument, "too-many", domain::side::sell,
                                     domain::offset_flag::close_today, 3, 0, 10);
    BOOST_CHECK_THROW(ledger.reserve_order(too_many), std::invalid_argument);
    BOOST_CHECK_EQUAL(ledger.snapshot(instrument).funds.frozen_commission, 0);
    const auto close = make_order(account, instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 2, 0, 10);
    ledger.reserve_order(close);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.frozen.long_today, 2);
    BOOST_CHECK_EQUAL(state.funds.available, 801);
    const auto close_first = make_trade(close, "close-1", 1, -90, 5, 50);
    BOOST_REQUIRE(ledger.apply_trade(close_first) == offline_account_trade_result::applied);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.total.long_today, 1);
    BOOST_CHECK_EQUAL(state.position.frozen.long_today, 1);
    BOOST_CHECK_EQUAL(state.funds.balance, 1036);
    BOOST_CHECK_EQUAL(state.funds.position_margin, 80);
    BOOST_CHECK_EQUAL(state.funds.available, 946);

    ledger.confirm_terminal(account, "close", position_terminal_status::canceled, 1);
    state = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(state.position.frozen.long_today, 0);
    BOOST_CHECK_EQUAL(state.position.available().long_today, 1);
    BOOST_CHECK_EQUAL(state.funds.frozen_commission, 0);
    BOOST_CHECK_EQUAL(state.funds.available, 956);
    BOOST_CHECK_EQUAL(ledger.position_margin(instrument, domain::side::buy), 80);
    BOOST_CHECK(ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_account_rejects_bad_economics_without_half_settlement) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_account_ledger ledger(account, "unit");
    ledger.initialize_flat(1000);
    const auto order = make_order(account, instrument, "order", domain::side::buy,
                                  domain::offset_flag::open, 1, 100, 10);
    ledger.reserve_order(order);
    const auto before = ledger.snapshot(instrument);
    const auto invalid = make_trade(order, "bad-trade", 1, 101, 1, 0);
    BOOST_CHECK_THROW(ledger.apply_trade(invalid), std::runtime_error);
    const auto after = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(after.position.total.long_today, before.position.total.long_today);
    BOOST_CHECK_EQUAL(after.funds.balance, before.funds.balance);
    BOOST_CHECK_EQUAL(after.funds.position_margin, before.funds.position_margin);
    BOOST_CHECK_EQUAL(after.funds.frozen_margin, before.funds.frozen_margin);
    BOOST_CHECK_EQUAL(after.funds.frozen_commission, before.funds.frozen_commission);
    BOOST_CHECK(!ledger.is_consistent());
    BOOST_CHECK(!ledger.can_reserve_orders());
    BOOST_CHECK_THROW(ledger.reserve_order(order), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_account_rejects_close_margin_overrelease_atomically) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_account_ledger ledger(account, "unit");
    ledger.initialize_flat(1000);
    const auto open = make_order(account, instrument, "open", domain::side::buy,
                                 domain::offset_flag::open, 1, 100, 0);
    ledger.reserve_order(open);
    ledger.apply_trade(make_trade(open, "open-trade", 1, 100, 0, 0));
    const auto close = make_order(account, instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 1, 0, 0);
    ledger.reserve_order(close);
    const auto before = ledger.snapshot(instrument);
    BOOST_CHECK_THROW(ledger.apply_trade(make_trade(close, "close-trade", 1, -101, 0, 0)),
                      std::runtime_error);
    const auto after = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(after.position.total.long_today, before.position.total.long_today);
    BOOST_CHECK_EQUAL(after.position.frozen.long_today, before.position.frozen.long_today);
    BOOST_CHECK_EQUAL(after.funds.position_margin, before.funds.position_margin);
    BOOST_CHECK_EQUAL(after.funds.available, before.funds.available);
    BOOST_CHECK(!ledger.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_account_detects_conflicting_duplicate_economics) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    offline_account_ledger ledger(account, "unit");
    ledger.initialize_flat(1000);
    const auto order = make_order(account, instrument, "order", domain::side::buy,
                                  domain::offset_flag::open, 1, 100, 10);
    ledger.reserve_order(order);
    auto trade = make_trade(order, "trade", 1, 100, 1, 0);
    BOOST_REQUIRE(ledger.apply_trade(trade) == offline_account_trade_result::applied);
    BOOST_REQUIRE(ledger.apply_trade(trade) == offline_account_trade_result::duplicate);
    const auto before = ledger.snapshot(instrument);
    trade.commission = 2;
    BOOST_CHECK_THROW(ledger.apply_trade(trade), std::runtime_error);
    const auto after = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(after.position.total.long_today, before.position.total.long_today);
    BOOST_CHECK_EQUAL(after.funds.balance, before.funds.balance);
    BOOST_CHECK_EQUAL(after.funds.commission, before.funds.commission);
    BOOST_CHECK(!ledger.is_consistent());
}
