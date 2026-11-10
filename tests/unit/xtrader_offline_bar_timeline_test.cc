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
#include <seastar/xtrader/offline_bar_timeline.hh>
#include <seastar/xtrader/offline_simulated_execution.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <limits>
#include <stdexcept>
#include <variant>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
namespace domain = seastar::xtrader::domain;

// B3: exercise explicit bar timing and the offline match boundary.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_bar_record make_bar(const seastar::sstring& instrument, int64_t timestamp,
                            offline_bar_timestamp kind, double open, double close) {
    return {
        .exchange_id = "XSGE",
        .instrument_id = instrument,
        .timeframe = "1m",
        .trading_day = "20260102",
        .time_domain = "synthetic-clock",
        .timestamp_nanos = timestamp,
        .duration_nanos = 10,
        .timestamp_kind = kind,
        .open_price = open,
        .high_price = 110,
        .low_price = 90,
        .close_price = close,
        .volume = 7,
        .open_interest = 50,
    };
}

bool is_open(const offline_bar_timeline_event& event) {
    return std::holds_alternative<offline_bar_open_data>(event.data);
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_timeline_orders_global_boundaries) {
    const auto events = build_offline_bar_timeline({
        make_bar("rb9999", 120, offline_bar_timestamp::close, 102, 103),
        make_bar("cu9999", 100, offline_bar_timestamp::open, 99, 100),
        make_bar("rb9999", 100, offline_bar_timestamp::open, 100, 101),
    });
    BOOST_REQUIRE_EQUAL(events.size(), 6);
    BOOST_CHECK_EQUAL(events[0].timestamp_nanos, 100);
    BOOST_CHECK_EQUAL(events[0].instrument_id, "cu9999");
    BOOST_CHECK(is_open(events[0]));
    BOOST_CHECK_EQUAL(events[1].instrument_id, "rb9999");
    BOOST_CHECK(is_open(events[1]));
    BOOST_CHECK_EQUAL(events[2].timestamp_nanos, 110);
    BOOST_CHECK_EQUAL(events[2].instrument_id, "cu9999");
    BOOST_CHECK(!is_open(events[2]));
    BOOST_CHECK_EQUAL(events[3].instrument_id, "rb9999");
    BOOST_CHECK(!is_open(events[3]));
    BOOST_CHECK_EQUAL(events[4].instrument_id, "rb9999");
    BOOST_CHECK(is_open(events[4]));
    BOOST_CHECK_EQUAL(events[5].timestamp_nanos, 120);
    BOOST_CHECK(!is_open(events[5]));
    for (size_t index = 0; index < events.size(); ++index) {
        BOOST_CHECK_EQUAL(events[index].sequence, index + 1);
    }
    BOOST_CHECK_EQUAL(std::get<offline_bar_open_data>(events[4].data).open_price, 102);
    const auto& completed = std::get<offline_bar_completed_data>(events[5].data);
    BOOST_CHECK_EQUAL(completed.high_price, 110);
    BOOST_CHECK_EQUAL(completed.close_price, 103);
    BOOST_CHECK_EQUAL(completed.volume, 7);
    BOOST_CHECK_EQUAL(completed.open_interest, 50);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_timeline_rejects_bad_or_overlapping_inputs) {
    auto valid = make_bar("rb9999", 100, offline_bar_timestamp::open, 100, 101);
    BOOST_CHECK_THROW(build_offline_bar_timeline({}), std::invalid_argument);
    auto invalid = valid;
    invalid.timestamp_kind.reset();
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid.timestamp_kind = static_cast<offline_bar_timestamp>(99);
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.duration_nanos = 0;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.instrument_id = " ";
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.timeframe = "";
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.trading_day = "20260230";
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.time_domain = "";
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.high_price = std::numeric_limits<double>::quiet_NaN();
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.open_price = 111;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.low_price = 0;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.close_price = 89;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.volume = -1;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.open_interest = -1;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::invalid_argument);
    invalid = valid;
    invalid.timestamp_nanos = std::numeric_limits<int64_t>::max() - 9;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::overflow_error);
    invalid = valid;
    invalid.timestamp_kind = offline_bar_timestamp::close;
    invalid.timestamp_nanos = std::numeric_limits<int64_t>::min() + 9;
    BOOST_CHECK_THROW(build_offline_bar_timeline({invalid}), std::overflow_error);
    invalid = valid;
    invalid.timestamp_nanos = 105;
    BOOST_CHECK_THROW(build_offline_bar_timeline({valid, invalid}), std::invalid_argument);
    BOOST_CHECK_THROW(build_offline_bar_timeline({valid, valid}), std::invalid_argument);
    invalid.instrument_id = "cu9999";
    BOOST_CHECK_EQUAL(build_offline_bar_timeline({valid, invalid}).size(), 4);
    invalid.time_domain = "other-clock";
    BOOST_CHECK_THROW(build_offline_bar_timeline({valid, invalid}), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(xtrader_offline_bar_timeline_places_close_decision_at_next_open) {
    const auto events = build_offline_bar_timeline({
        make_bar("rb9999", 100, offline_bar_timestamp::open, 100, 101),
        make_bar("rb9999", 120, offline_bar_timestamp::close, 102, 103),
    });
    offline_simulated_execution execution(test_account(), "synthetic-unit", "strategy", {make_terms()},
                                           offline_cost_allocation::fifo, 21,
                                           simulated_market_mode::next_bar_open,
                                           simulated_bar_liquidity{.buy_volume = 1, .sell_volume = 1});
    execution.initialize_flat(1000);
    std::optional<simulated_order_id> order;
    size_t trade_count = 0;
    for (const auto& event : events) {
        if (is_open(event)) {
            const auto open = std::get<offline_bar_open_data>(event.data).open_price;
            const auto normalized = execution.normalize_price(event.exchange_id, event.instrument_id, open,
                                                               offline_price_normalization::exact);
            const auto result = execution.on_bar_open({
                .sequence = event.sequence,
                .exchange_id = event.exchange_id,
                .instrument_id = event.instrument_id,
                .open_ticks = normalized.price_ticks,
            });
            trade_count += result.trades.size();
            if (!result.trades.empty()) {
                BOOST_REQUIRE(order);
                BOOST_CHECK(result.trades[0].order_id == *order);
                BOOST_CHECK_EQUAL(event.timestamp_nanos, 110);
                BOOST_CHECK_EQUAL(result.trades[0].price, 102);
                BOOST_CHECK(result.trades[0].is_approximate);
            }
        } else if (event.timestamp_nanos == 110) {
            BOOST_CHECK_EQUAL(trade_count, 0);
            const auto& completed = std::get<offline_bar_completed_data>(event.data);
            BOOST_CHECK_EQUAL(completed.close_price, 101);
            order = execution.submit({
                .strategy_id = "strategy",
                .instrument = test_instrument(),
                .direction = domain::side::buy,
                .offset = domain::offset_flag::open,
                .volume = 1,
                .limit_price_ticks = 105,
                .maximum_price_ticks = 110,
            }).id;
        }
    }
    BOOST_CHECK_EQUAL(trade_count, 1);
    BOOST_CHECK_EQUAL(execution.snapshot(test_instrument()).position.total.long_today, 1);
}
