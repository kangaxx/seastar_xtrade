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
#include <seastar/xtrader/simulated_order_book.hh>

#include <limits>
#include <stdexcept>

namespace {

using namespace seastar::xtrader;
namespace domain = seastar::xtrader::domain;

// B3 offline matching regressions for quote and approximate bar-open execution.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
simulated_order_request make_order(const seastar::sstring& strategy_id, domain::side direction,
                                   double price, int volume) {
    return {
        .strategy_id = strategy_id,
        .exchange_id = "XSGE",
        .instrument_id = "rb9999",
        .direction = direction,
        .offset = domain::offset_flag::open,
        .hedge = domain::hedge_flag::speculation,
        .type = domain::order_type::limit,
        .volume = volume,
        .limit_price = price,
    };
}

simulated_quote_event make_quote(uint64_t sequence, const seastar::sstring& instrument_id,
                                 double bid, int bid_volume, double ask, int ask_volume) {
    return {
        .sequence = sequence,
        .exchange_id = "XSGE",
        .instrument_id = instrument_id,
        .bid_price = bid,
        .bid_volume = bid_volume,
        .ask_price = ask,
        .ask_volume = ask_volume,
    };
}

simulated_bar_open_event make_bar(uint64_t sequence, double open) {
    return {
        .sequence = sequence,
        .exchange_id = "XSGE",
        .instrument_id = "rb9999",
        .open_price = open,
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_tick_waits_for_later_matching_quote) {
    simulated_order_book book(11, simulated_market_mode::tick_quote, std::nullopt);
    const auto initial = make_quote(1, "rb9999", 99.0, 3, 100.0, 3);
    BOOST_REQUIRE(book.on_quote(initial).trades.empty());
    const auto order = book.submit(make_order("strategy-a", domain::side::buy, 101.0, 3));
    BOOST_REQUIRE_EQUAL(order.submitted_after_event, 1);
    BOOST_CHECK_THROW(book.on_quote(initial), std::invalid_argument);
    BOOST_REQUIRE(book.on_quote(make_quote(2, "cu9999", 99.0, 3, 100.0, 3)).trades.empty());

    const auto first = book.on_quote(make_quote(3, "rb9999", 99.0, 0, 100.0, 2));
    BOOST_REQUIRE_EQUAL(first.trades.size(), 1);
    BOOST_REQUIRE_EQUAL(first.order_updates.size(), 1);
    BOOST_CHECK(first.trades[0].order_id == order.id);
    BOOST_CHECK(first.trades[0].source == simulated_fill_source::best_quote_snapshot);
    BOOST_CHECK(first.trades[0].is_approximate);
    BOOST_CHECK_EQUAL(first.trades[0].strategy_id, "strategy-a");
    BOOST_CHECK_EQUAL(first.trades[0].price, 100.0);
    BOOST_CHECK_EQUAL(first.trades[0].volume, 2);
    BOOST_CHECK_EQUAL(first.trades[0].market_sequence, 3);
    BOOST_CHECK(first.order_updates[0].status == simulated_order_status::partially_filled);
    BOOST_CHECK_EQUAL(first.order_updates[0].remaining_volume(), 1);
    BOOST_REQUIRE(book.on_quote(make_quote(4, "rb9999", 99.0, 3, 102.0, 3)).trades.empty());

    const auto last = book.on_quote(make_quote(5, "rb9999", 99.0, 3, 101.0, 1));
    BOOST_REQUIRE_EQUAL(last.trades.size(), 1);
    BOOST_CHECK_EQUAL(last.trades[0].price, 101.0);
    BOOST_CHECK_EQUAL(last.trades[0].volume, 1);
    BOOST_CHECK(last.trades[0].id != first.trades[0].id);
    BOOST_CHECK(last.order_updates[0].status == simulated_order_status::filled);
    BOOST_CHECK_EQUAL(book.get_order(order.id, "strategy-a").filled_volume, 3);
    BOOST_CHECK_THROW(book.cancel(order.id, "strategy-a"), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_quote_uses_price_then_submission_priority) {
    simulated_order_book book(12, simulated_market_mode::tick_quote, std::nullopt);
    book.on_quote(make_quote(1, "rb9999", 99.0, 1, 100.0, 1));
    const auto buy_low = book.submit(make_order("buy-low", domain::side::buy, 101.0, 1));
    const auto buy_first = book.submit(make_order("buy-first", domain::side::buy, 103.0, 1));
    const auto buy_second = book.submit(make_order("buy-second", domain::side::buy, 103.0, 1));
    const auto sell_high = book.submit(make_order("sell-high", domain::side::sell, 99.0, 1));
    const auto sell_first = book.submit(make_order("sell-first", domain::side::sell, 97.0, 1));
    const auto sell_second = book.submit(make_order("sell-second", domain::side::sell, 97.0, 1));

    const auto first = book.on_quote(make_quote(2, "rb9999", 99.0, 2, 100.0, 2));
    BOOST_REQUIRE_EQUAL(first.trades.size(), 4);
    BOOST_REQUIRE_EQUAL(first.order_updates.size(), 4);
    BOOST_CHECK(first.trades[0].order_id == buy_first.id);
    BOOST_CHECK(first.trades[1].order_id == buy_second.id);
    BOOST_CHECK(first.trades[2].order_id == sell_first.id);
    BOOST_CHECK(first.trades[3].order_id == sell_second.id);
    BOOST_CHECK_EQUAL(first.trades[0].price, 100.0);
    BOOST_CHECK_EQUAL(first.trades[2].price, 99.0);
    for (size_t index = 0; index < first.trades.size(); ++index) {
        BOOST_CHECK(first.trades[index].order_id == first.order_updates[index].id);
        BOOST_CHECK(first.order_updates[index].status == simulated_order_status::filled);
    }
    const auto second = book.on_quote(make_quote(3, "rb9999", 99.0, 1, 100.0, 1));
    BOOST_REQUIRE_EQUAL(second.trades.size(), 2);
    BOOST_CHECK(second.trades[0].order_id == buy_low.id);
    BOOST_CHECK(second.trades[1].order_id == sell_high.id);
}

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_cancel_checks_run_and_strategy_ownership) {
    simulated_order_book first(21, simulated_market_mode::tick_quote, std::nullopt);
    simulated_order_book second(22, simulated_market_mode::tick_quote, std::nullopt);
    const auto order = first.submit(make_order("owner", domain::side::buy, 101.0, 3));
    const auto other = second.submit(make_order("owner", domain::side::buy, 101.0, 1));
    BOOST_CHECK(order.id != other.id);
    BOOST_CHECK_THROW(static_cast<void>(first.get_order(order.id, "other")), std::invalid_argument);
    BOOST_CHECK_THROW(first.cancel(order.id, "other"), std::invalid_argument);
    BOOST_CHECK_THROW(first.cancel(other.id, "owner"), std::invalid_argument);
    const simulated_order_id unknown{21, 999};
    BOOST_CHECK_THROW(static_cast<void>(first.get_order(unknown, "owner")), std::invalid_argument);

    const auto fills = first.on_quote(make_quote(1, "rb9999", 99.0, 1, 100.0, 1));
    BOOST_REQUIRE_EQUAL(fills.trades.size(), 1);
    const auto canceled = first.cancel(order.id, "owner");
    BOOST_CHECK(canceled.status == simulated_order_status::canceled);
    BOOST_CHECK_EQUAL(canceled.filled_volume, 1);
    BOOST_CHECK_EQUAL(canceled.remaining_volume(), 2);
    BOOST_CHECK_THROW(first.cancel(order.id, "owner"), std::logic_error);
    BOOST_CHECK(first.on_quote(make_quote(2, "rb9999", 99.0, 3, 100.0, 3)).trades.empty());
    BOOST_CHECK(first.get_order(order.id, "owner").status == simulated_order_status::canceled);
    BOOST_CHECK(second.get_order(other.id, "owner").status == simulated_order_status::accepted);
}

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_bars_use_only_later_open_and_explicit_limits) {
    const simulated_bar_liquidity limits{.buy_volume = 2, .sell_volume = 1};
    simulated_order_book book(31, simulated_market_mode::next_bar_open, limits);
    BOOST_REQUIRE(book.on_bar_open(make_bar(1, 100.0)).trades.empty());
    const auto buy_low = book.submit(make_order("buy-low", domain::side::buy, 101.0, 3));
    const auto buy_high = book.submit(make_order("buy-high", domain::side::buy, 105.0, 1));
    const auto sell = book.submit(make_order("sell", domain::side::sell, 99.0, 2));
    BOOST_CHECK_THROW(book.on_bar_open(make_bar(1, 100.0)), std::invalid_argument);

    const auto next = book.on_bar_open(make_bar(2, 102.0));
    BOOST_REQUIRE_EQUAL(next.trades.size(), 2);
    BOOST_CHECK(next.trades[0].order_id == buy_high.id);
    BOOST_CHECK(next.trades[1].order_id == sell.id);
    for (const auto& trade : next.trades) {
        BOOST_CHECK_EQUAL(trade.price, 102.0);
        BOOST_CHECK(trade.source == simulated_fill_source::bar_open_approximation);
        BOOST_CHECK(trade.is_approximate);
    }
    BOOST_CHECK_EQUAL(book.get_order(buy_low.id, "buy-low").filled_volume, 0);

    const auto following = book.on_bar_open(make_bar(3, 100.0));
    BOOST_REQUIRE_EQUAL(following.trades.size(), 2);
    BOOST_CHECK(following.trades[0].order_id == buy_low.id);
    BOOST_CHECK_EQUAL(following.trades[0].volume, 2);
    BOOST_CHECK(following.trades[1].order_id == sell.id);
    BOOST_CHECK_EQUAL(following.trades[1].volume, 1);
    const auto last = book.on_bar_open(make_bar(4, 100.0));
    BOOST_REQUIRE_EQUAL(last.trades.size(), 1);
    BOOST_CHECK(last.trades[0].order_id == buy_low.id);
    BOOST_CHECK_EQUAL(last.trades[0].volume, 1);
    BOOST_CHECK(last.order_updates[0].status == simulated_order_status::filled);

    BOOST_CHECK_THROW(book.on_quote(make_quote(5, "rb9999", 99.0, 1, 100.0, 1)), std::logic_error);
    BOOST_CHECK(book.on_bar_open(make_bar(5, 100.0)).trades.empty());
}

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_matching_rejects_missing_or_unsupported_inputs) {
    const simulated_bar_liquidity limits{.buy_volume = 2, .sell_volume = 1};
    const simulated_bar_liquidity invalid_limits{.buy_volume = -1, .sell_volume = 1};
    BOOST_CHECK_THROW(static_cast<void>(simulated_order_book{0, simulated_market_mode::tick_quote, std::nullopt}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(simulated_order_book{1, simulated_market_mode::tick_quote, limits}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(simulated_order_book{1, simulated_market_mode::next_bar_open, std::nullopt}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(simulated_order_book{1, simulated_market_mode::next_bar_open, invalid_limits}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(static_cast<void>(simulated_order_book{1, static_cast<simulated_market_mode>(99), std::nullopt}),
                      std::invalid_argument);

    simulated_order_book book(41, simulated_market_mode::tick_quote, std::nullopt);
    auto request = make_order("owner", domain::side::buy, 100.0, 1);
    auto invalid = request;
    invalid.strategy_id = "  ";
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.exchange_id = "";
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.direction.reset();
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid.direction = static_cast<domain::side>(99);
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.offset.reset();
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.hedge = static_cast<domain::hedge_flag>(99);
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.type.reset();
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    for (const auto type : {domain::order_type::market, domain::order_type::fok, domain::order_type::fak}) {
        invalid.type = type;
        BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    }
    invalid = request;
    invalid.volume = 0;
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid = request;
    invalid.limit_price = std::numeric_limits<double>::quiet_NaN();
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    invalid.limit_price = std::numeric_limits<double>::infinity();
    BOOST_CHECK_THROW(book.submit(invalid), std::invalid_argument);
    const auto order = book.submit(request);
    BOOST_CHECK_EQUAL(order.id.sequence, 1);

    auto quote = make_quote(1, "rb9999", 99.0, 1, 100.0, 1);
    auto bad_quote = quote;
    bad_quote.sequence = 0;
    BOOST_CHECK_THROW(book.on_quote(bad_quote), std::invalid_argument);
    bad_quote = quote;
    bad_quote.instrument_id = "";
    BOOST_CHECK_THROW(book.on_quote(bad_quote), std::invalid_argument);
    bad_quote = quote;
    bad_quote.ask_volume = -1;
    BOOST_CHECK_THROW(book.on_quote(bad_quote), std::invalid_argument);
    bad_quote = quote;
    bad_quote.bid_price = 101.0;
    BOOST_CHECK_THROW(book.on_quote(bad_quote), std::invalid_argument);
    bad_quote = quote;
    bad_quote.ask_price = std::numeric_limits<double>::quiet_NaN();
    BOOST_CHECK_THROW(book.on_quote(bad_quote), std::invalid_argument);
    BOOST_CHECK_THROW(book.on_bar_open(make_bar(1, 100.0)), std::logic_error);
    BOOST_CHECK_EQUAL(book.get_order(order.id, "owner").filled_volume, 0);
    BOOST_REQUIRE_EQUAL(book.on_quote(quote).trades.size(), 1);
    BOOST_CHECK_THROW(book.on_quote(quote), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(xtrader_simulated_zero_bar_liquidity_is_explicit_and_never_fills) {
    const simulated_bar_liquidity limits{.buy_volume = 0, .sell_volume = 0};
    simulated_order_book book(51, simulated_market_mode::next_bar_open, limits);
    const auto order = book.submit(make_order("owner", domain::side::buy, 100.0, 1));
    auto invalid = make_bar(1, 0.0);
    BOOST_CHECK_THROW(book.on_bar_open(invalid), std::invalid_argument);
    invalid.open_price = std::numeric_limits<double>::infinity();
    BOOST_CHECK_THROW(book.on_bar_open(invalid), std::invalid_argument);
    BOOST_REQUIRE(book.on_bar_open(make_bar(1, 99.0)).trades.empty());
    BOOST_CHECK(book.get_order(order.id, "owner").status == simulated_order_status::accepted);
}
