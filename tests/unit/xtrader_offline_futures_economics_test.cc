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
#include <seastar/xtrader/offline_futures_economics.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B2: test exact arithmetic and explicit synthetic economics.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_futures_terms zero_rate_terms() {
    auto terms = make_terms();
    const offline_rate zero{0, 1, offline_rounding::down};
    terms.tick_value_per_lot = 1;
    terms.long_margin_rate = zero;
    terms.short_margin_rate = zero;
    terms.open_fee = {0, zero};
    terms.close_fee = {0, zero};
    terms.close_today_fee = {0, zero};
    terms.close_yesterday_fee = {0, zero};
    return terms;
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_require_complete_explicit_terms) {
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{offline_futures_terms{}}),
                      std::invalid_argument);
    auto terms = make_terms();
    terms.instrument.hedge.reset();
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.money_unit = " ";
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.tick_value_per_lot = 0;
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.price_tick = std::numeric_limits<double>::quiet_NaN();
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.long_margin_rate.numerator = -1;
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.short_margin_rate.denominator = 0;
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.open_fee.notional_rate.rounding.reset();
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.close_fee.notional_rate.rounding = static_cast<offline_rounding>(99);
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.close_today_fee.fixed_per_lot = -1;
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
    terms = make_terms();
    terms.close_yesterday_fee.notional_rate.denominator = 0;
    BOOST_CHECK_THROW(static_cast<void>(offline_futures_economics{terms}), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_apply_explicit_rates_and_close_fees) {
    const offline_futures_economics economics(make_terms());
    BOOST_CHECK(economics.terms().instrument == test_instrument());
    BOOST_CHECK_EQUAL(economics.terms().money_unit, "synthetic-unit");
    BOOST_CHECK_EQUAL(economics.price_at_ticks(98), 98.0);

    const auto long_reserve = economics.reserve(domain::side::buy, domain::offset_flag::open, 100);
    const auto short_reserve = economics.reserve(domain::side::sell, domain::offset_flag::open, 100);
    BOOST_CHECK_EQUAL(long_reserve.margin_per_lot, 100);
    BOOST_CHECK_EQUAL(short_reserve.margin_per_lot, 200);
    BOOST_CHECK_EQUAL(long_reserve.commission_per_lot, 3);

    const auto long_open = economics.open_fill(domain::side::buy, 98, 100, 2);
    BOOST_CHECK_EQUAL(long_open.margin_change, 196);
    BOOST_CHECK_EQUAL(long_open.commission, 6);
    BOOST_CHECK_EQUAL(long_open.close_profit, 0);
    const auto short_open = economics.open_fill(domain::side::sell, 101, 110, 2);
    BOOST_CHECK_EQUAL(short_open.margin_change, 404);
    BOOST_CHECK_EQUAL(short_open.commission, 8);

    const auto long_close = economics.close_fill(domain::side::sell, domain::offset_flag::close_today,
                                                  98, 103, 110, 98, 2);
    BOOST_CHECK_EQUAL(long_close.margin_change, -196);
    BOOST_CHECK_EQUAL(long_close.commission, 12);
    BOOST_CHECK_EQUAL(long_close.close_profit, 100);
    const auto short_close = economics.close_fill(domain::side::buy, domain::offset_flag::close,
                                                   101, 95, 110, 202, 2);
    BOOST_CHECK_EQUAL(short_close.margin_change, -404);
    BOOST_CHECK_EQUAL(short_close.commission, 8);
    BOOST_CHECK_EQUAL(short_close.close_profit, 120);
    const auto short_loss = economics.close_fill(domain::side::buy, domain::offset_flag::close_yesterday,
                                                  101, 105, 110, 202, 1);
    BOOST_CHECK_EQUAL(short_loss.commission, 1);
    BOOST_CHECK_EQUAL(short_loss.close_profit, -40);
    const auto long_loss = economics.close_fill(domain::side::sell, domain::offset_flag::close,
                                                 101, 95, 110, 202, 1);
    BOOST_CHECK_EQUAL(long_loss.close_profit, -60);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_round_per_lot_before_volume) {
    auto terms = zero_rate_terms();
    terms.long_margin_rate = {1, 2, offline_rounding::down};
    terms.open_fee = {1, {1, 2, offline_rounding::up}};
    offline_futures_economics down(terms);
    BOOST_CHECK_EQUAL(down.reserve(domain::side::buy, domain::offset_flag::open, 11).margin_per_lot, 5);
    const auto fill = down.open_fill(domain::side::buy, 11, 11, 3);
    BOOST_CHECK_EQUAL(fill.margin_change, 15);
    BOOST_CHECK_EQUAL(fill.commission, 21);

    terms.long_margin_rate.rounding = offline_rounding::up;
    offline_futures_economics up(terms);
    BOOST_CHECK_EQUAL(up.reserve(domain::side::buy, domain::offset_flag::open, 11).margin_per_lot, 6);
    BOOST_CHECK_EQUAL(up.reserve(domain::side::buy, domain::offset_flag::open, 10).margin_per_lot, 5);
    terms.long_margin_rate.rounding = offline_rounding::half_up;
    offline_futures_economics half(terms);
    BOOST_CHECK_EQUAL(half.reserve(domain::side::buy, domain::offset_flag::open, 11).margin_per_lot, 6);
    terms.long_margin_rate = {1, 3, offline_rounding::half_up};
    offline_futures_economics thirds(terms);
    BOOST_CHECK_EQUAL(thirds.reserve(domain::side::buy, domain::offset_flag::open, 1).margin_per_lot, 0);
    BOOST_CHECK_EQUAL(thirds.reserve(domain::side::buy, domain::offset_flag::open, 2).margin_per_lot, 1);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_normalize_price_only_by_explicit_policy) {
    auto terms = make_terms();
    terms.price_tick = 0.1;
    const offline_futures_economics economics(terms);
    const double canonical = economics.price_at_ticks(1001);
    const auto exact = economics.normalize_price(canonical, offline_price_normalization::exact);
    BOOST_CHECK_EQUAL(exact.price_ticks, 1001);
    BOOST_CHECK_EQUAL(exact.price, canonical);
    BOOST_CHECK(!exact.adjusted);

    const double adjacent = std::nextafter(canonical, -std::numeric_limits<double>::infinity());
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(adjacent, offline_price_normalization::exact)),
                      std::invalid_argument);
    const auto adjusted = economics.normalize_price(adjacent, offline_price_normalization::nearest_one_ulp);
    BOOST_CHECK_EQUAL(adjusted.price_ticks, 1001);
    BOOST_CHECK_EQUAL(adjusted.price, canonical);
    BOOST_CHECK(adjusted.adjusted);
    const double two_steps = std::nextafter(adjacent, -std::numeric_limits<double>::infinity());
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(
                          two_steps, offline_price_normalization::nearest_one_ulp)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(
                          100.15, offline_price_normalization::nearest_one_ulp)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(
                          std::numeric_limits<double>::quiet_NaN(), offline_price_normalization::exact)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(0.0, offline_price_normalization::exact)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.normalize_price(
                          canonical, static_cast<offline_price_normalization>(99))), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_roundtrip_representable_ticks) {
    for (double tick : {0.1, 0.2, 0.25, 1.0, 2.5}) {
        auto terms = make_terms();
        terms.price_tick = tick;
        const offline_futures_economics economics(terms);
        for (int64_t ticks : {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{7}, int64_t{10},
                                  int64_t{101}, int64_t{1001}, int64_t{1'000'000'000}}) {
            const auto price = economics.price_at_ticks(ticks);
            const auto normalized = economics.normalize_price(price, offline_price_normalization::exact);
            BOOST_CHECK_EQUAL(normalized.price_ticks, ticks);
            BOOST_CHECK_EQUAL(normalized.price, price);
            BOOST_CHECK(!normalized.adjusted);
        }
    }
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_reject_invalid_or_overflowing_inputs) {
    const offline_futures_economics economics(make_terms());
    BOOST_CHECK_THROW(static_cast<void>(economics.reserve(
                          static_cast<domain::side>(99), domain::offset_flag::open, 100)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.reserve(
                          domain::side::buy, static_cast<domain::offset_flag>(99), 100)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.reserve(domain::side::buy, domain::offset_flag::open, 0)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.price_at_ticks(0)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.price_at_ticks(int64_t{1} << 53)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.open_fill(domain::side::buy, 101, 100, 1)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.open_fill(domain::side::buy, 100, 100, 0)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.close_fill(domain::side::sell, domain::offset_flag::open,
                                                              100, 100, 100, 1, 1)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.close_fill(domain::side::sell, static_cast<domain::offset_flag>(99),
                                                              100, 100, 100, 1, 1)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.close_fill(domain::side::sell, domain::offset_flag::close,
                                                              0, 100, 100, 1, 1)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.close_fill(domain::side::sell, domain::offset_flag::close,
                                                              100, 100, 100, -1, 1)), std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(economics.reserve(domain::side::buy, domain::offset_flag::open,
                                                           std::numeric_limits<int64_t>::max())),
                      std::overflow_error);

    auto terms = zero_rate_terms();
    const auto max = std::numeric_limits<int64_t>::max();
    terms.long_margin_rate = {max, max, offline_rounding::up};
    offline_futures_economics exact(terms);
    BOOST_CHECK_EQUAL(exact.reserve(domain::side::buy, domain::offset_flag::open, max).margin_per_lot, max);
    BOOST_CHECK_THROW(static_cast<void>(exact.open_fill(domain::side::buy, max, max, 2)), std::overflow_error);
    terms.long_margin_rate = {max, 1, offline_rounding::down};
    offline_futures_economics rate_overflow(terms);
    BOOST_CHECK_THROW(static_cast<void>(rate_overflow.reserve(domain::side::buy, domain::offset_flag::open, 2)),
                      std::overflow_error);
    terms.long_margin_rate = {0, 1, offline_rounding::down};
    offline_futures_economics profit_overflow(terms);
    BOOST_CHECK_THROW(static_cast<void>(profit_overflow.close_fill(domain::side::sell, domain::offset_flag::close,
                                                                   1, max, max, 0, 2)), std::overflow_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_economics_supply_atomic_account_settlement) {
    const auto account = test_account();
    const auto instrument = test_instrument();
    const offline_futures_economics economics(make_terms());
    offline_account_ledger ledger(account, economics.terms().money_unit);
    ledger.initialize_flat(1000);

    const auto open_reserve = economics.reserve(domain::side::buy, domain::offset_flag::open, 100);
    const auto open = make_order(account, instrument, "open", domain::side::buy, domain::offset_flag::open,
                                 2, open_reserve.margin_per_lot, open_reserve.commission_per_lot);
    ledger.reserve_order(open);
    const auto open_value = economics.open_fill(domain::side::buy, 98, 100, 2);
    auto open_trade = make_trade(open, "open-trade", 2, open_value.margin_change,
                                 open_value.commission, open_value.close_profit);
    open_trade.position.price = 98.0;
    BOOST_REQUIRE(ledger.apply_trade(open_trade) == offline_account_trade_result::applied);
    ledger.confirm_terminal(account, "open", position_terminal_status::filled, 2);
    BOOST_CHECK_EQUAL(ledger.snapshot(instrument).funds.position_margin, 196);

    const auto close_reserve = economics.reserve(domain::side::sell, domain::offset_flag::close_today, 110);
    const auto close = make_order(account, instrument, "close", domain::side::sell,
                                  domain::offset_flag::close_today, 2,
                                  close_reserve.margin_per_lot, close_reserve.commission_per_lot);
    ledger.reserve_order(close);
    const auto close_value = economics.close_fill(domain::side::sell, domain::offset_flag::close_today,
                                                   98, 103, 110, 98, 2);
    auto close_trade = make_trade(close, "close-trade", 2, close_value.margin_change,
                                  close_value.commission, close_value.close_profit);
    close_trade.position.price = 103.0;
    BOOST_REQUIRE(ledger.apply_trade(close_trade) == offline_account_trade_result::applied);
    ledger.confirm_terminal(account, "close", position_terminal_status::filled, 2);
    const auto final = ledger.snapshot(instrument);
    BOOST_CHECK_EQUAL(final.position.total.long_today, 0);
    BOOST_CHECK_EQUAL(final.funds.balance, 1082);
    BOOST_CHECK_EQUAL(final.funds.available, 1082);
    BOOST_CHECK_EQUAL(final.funds.commission, 18);
    BOOST_CHECK_EQUAL(final.funds.close_profit, 100);
    BOOST_CHECK_EQUAL(final.funds.position_margin, 0);
    BOOST_CHECK(ledger.can_reserve_orders());
}
