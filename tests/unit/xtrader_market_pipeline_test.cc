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
#include <seastar/xtrader/paper_market_session.hh>
#include <seastar/xtrader/market_record_format.hh>

#include "xtrader_offline_futures_fixture.hh"

#include <limits>
#include <stdexcept>

namespace {

using namespace seastar;
using namespace seastar::xtrader;
using namespace seastar::xtrader::test;

// B5 synthetic source and sinks only; no file, Redis or gateway connection.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
market_tick_input tick(uint64_t sequence, int64_t timestamp, int volume) {
    market_tick_input input{
        .sequence = sequence, .exchange_id = "XSGE", .time_domain = "synthetic-ms",
        .timestamp_ms = timestamp, .session_begin_ms = 0, .session_end_ms = 3600000,
    };
    input.tick.instrument_id = "rb9999";
    input.tick.trading_day = "20260102";
    input.tick.last_price = 100;
    input.tick.bid_price[0] = 99;
    input.tick.ask_price[0] = 100;
    input.tick.bid_volume[0] = 2;
    input.tick.ask_volume[0] = 2;
    input.tick.volume = volume;
    return input;
}

paper_market_config config() {
    auto account = test_account();
    account.environment = "paper";
    return {
        .account = account, .money_unit = "synthetic-unit", .strategy_id = "strategy",
        .contracts = {make_terms()}, .allocation = offline_cost_allocation::fifo,
        .run_id = 31, .initial_balance = 1000, .time_domain = "synthetic-ms",
        .maximum_quote_age = std::chrono::hours(1),
    };
}

offline_simulated_order_request request() {
    return {
        .strategy_id = "strategy", .instrument = test_instrument(),
        .direction = domain::side::buy, .offset = domain::offset_flag::open,
        .volume = 1, .limit_price_ticks = 101, .maximum_price_ticks = 110,
    };
}

future<> final_bars(const std::vector<recorded_minute_bar>&) {
    return make_ready_future<>();
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_market_pipeline_does_not_invent_initial_or_boundary_volume) {
    market_data_pipeline pipeline("synthetic-ms", {{"XSGE", "rb9999"}});
    BOOST_CHECK(!pipeline.accept(tick(1, 0, 100)).incremental_volume);
    auto second = pipeline.accept(tick(2, 1000, 103));
    BOOST_REQUIRE(second.incremental_volume);
    BOOST_CHECK_EQUAL(*second.incremental_volume, 3);
    auto next = pipeline.accept(tick(3, 60000, 108));
    BOOST_REQUIRE(next.completed_bar);
    BOOST_CHECK_EQUAL(next.completed_bar->observed_volume, 3);
    BOOST_CHECK(!next.completed_bar->is_complete);
    auto final = pipeline.finish();
    BOOST_REQUIRE_EQUAL(final.size(), 1);
    BOOST_CHECK_EQUAL(final[0].observed_volume, 0);
    BOOST_CHECK(!final[0].is_complete);
    BOOST_CHECK_THROW(pipeline.accept(tick(4, 61000, 109)), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_market_pipeline_rejects_bad_ticks_and_tracks_day_reset) {
    market_data_pipeline pipeline("synthetic-ms", {{"XSGE", "rb9999"}});
    pipeline.accept(tick(1, 1000, 100));
    BOOST_CHECK_THROW(pipeline.accept(tick(1, 1001, 100)), std::invalid_argument);
    BOOST_CHECK_THROW(pipeline.accept(tick(2, 1001, 99)), std::invalid_argument);
    auto bad = tick(2, 1001, 101);
    bad.tick.ask_price[0] = std::numeric_limits<double>::max();
    BOOST_CHECK_THROW(pipeline.accept(bad), std::invalid_argument);
    auto next_day = tick(2, 3600000, 1);
    next_day.session_begin_ms = 3600000;
    next_day.session_end_ms = 7200000;
    next_day.tick.trading_day = "20260103";
    const auto normalized = pipeline.accept(next_day);
    BOOST_CHECK(!normalized.incremental_volume);
    BOOST_REQUIRE(normalized.completed_bar);
    BOOST_CHECK_EQUAL(normalized.completed_bar->trading_day, "20260102");
}

SEASTAR_THREAD_TEST_CASE(xtrader_market_pipeline_gap_invalidates_other_instrument_baseline) {
    market_data_pipeline pipeline("synthetic-ms", {{"XSGE", "rb9999"}, {"XSGE", "rb8888"}});
    pipeline.accept(tick(1, 0, 100));
    auto other = tick(2, 0, 100);
    other.tick.instrument_id = "rb8888";
    pipeline.accept(other);
    BOOST_CHECK(pipeline.accept(tick(4, 1000, 110)).has_sequence_gap);
    other.sequence = 5;
    other.timestamp_ms = 1000;
    other.tick.volume = 120;
    BOOST_CHECK(!pipeline.accept(other).incremental_volume);
}

SEASTAR_THREAD_TEST_CASE(xtrader_market_record_formats_preserve_unknown_volume_and_namespace) {
    market_data_pipeline pipeline("synthetic-ms", {{"XSGE", "rb9999"}});
    const auto first = pipeline.accept(tick(1, 0, 100));
    const auto json = market_tick_json(first);
    BOOST_CHECK(json.find("\"incremental_volume\":null") != sstring::npos);
    BOOST_CHECK(json.find("\"schema_version\":1") != sstring::npos);
    BOOST_CHECK_EQUAL(paper_tick_key("alias", 31, "XSGE", "rb9999"),
                      "xtrader:v1:paper:alias:31:XSGE:rb9999:ticks");
    BOOST_CHECK_THROW(paper_tick_key("live:alias", 31, "XSGE", "rb9999"), std::invalid_argument);
    BOOST_CHECK_THROW(paper_tick_key("alias", 0, "XSGE", "rb9999"), std::invalid_argument);
    const auto bar = pipeline.finish().at(0);
    BOOST_CHECK(minute_bar_csv_header().find("observed_volume") != sstring::npos);
    BOOST_CHECK(minute_bar_csv_row(bar).find(",false\n") != sstring::npos);
}

SEASTAR_THREAD_TEST_CASE(xtrader_paper_session_records_before_matching_and_settles_locally) {
    size_t records = 0;
    paper_market_session session(config(), [&records] (const normalized_market_tick&) {
        ++records;
        return make_ready_future<>();
    }, final_bars);
    BOOST_CHECK_THROW(session.submit(request()), std::logic_error);
    BOOST_CHECK(session.on_tick(tick(1, 0, 100)).get0().matches.trades.empty());
    session.submit(request());
    const auto result = session.on_tick(tick(2, 1000, 101)).get0();
    BOOST_CHECK_EQUAL(records, 2);
    BOOST_REQUIRE_EQUAL(result.matches.trades.size(), 1);
    BOOST_CHECK(result.matches.trades[0].is_approximate);
    BOOST_CHECK_EQUAL(session.snapshot(test_instrument()).position.total.long_today, 1);
    session.disconnect();
    BOOST_CHECK_THROW(session.submit(request()), std::logic_error);
    session.stop().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_paper_recording_failure_blocks_match_but_allows_local_cancel) {
    size_t records = 0;
    paper_market_session session(config(), [&records] (const normalized_market_tick&) {
        if (++records == 2) {
            return make_exception_future<>(std::runtime_error("synthetic recording failure"));
        }
        return make_ready_future<>();
    }, final_bars);
    session.on_tick(tick(1, 0, 100)).get0();
    const auto order = session.submit(request());
    BOOST_CHECK_THROW(session.on_tick(tick(2, 1000, 101)).get0(), std::runtime_error);
    BOOST_CHECK(session.has_failed());
    BOOST_CHECK_EQUAL(session.snapshot(test_instrument()).position.total.long_today, 0);
    BOOST_CHECK_THROW(session.submit(request()), std::logic_error);
    session.cancel(order.id, "strategy");
    BOOST_CHECK_EQUAL(session.snapshot(test_instrument()).funds.frozen_margin, 0);
    session.stop().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_paper_stop_drains_delayed_recording_without_late_match) {
    promise<> recorded;
    const normalized_market_tick* borrowed = nullptr;
    paper_market_session session(config(), [&] (const normalized_market_tick& normalized) {
        borrowed = &normalized;
        return recorded.get_future();
    }, final_bars);
    auto pending = session.on_tick(tick(1, 0, 100));
    BOOST_REQUIRE(borrowed);
    BOOST_CHECK_EQUAL(borrowed->source.sequence, 1);
    BOOST_CHECK_THROW(session.on_tick(tick(2, 1, 101)).get0(), std::logic_error);
    auto stopped = session.stop();
    BOOST_CHECK(!stopped.available());
    recorded.set_value();
    BOOST_CHECK_THROW(pending.get0(), std::runtime_error);
    stopped.get();
    BOOST_CHECK(session.has_failed());
}
