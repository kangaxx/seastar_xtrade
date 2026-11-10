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
#include <seastar/xtrader/ctp_callback_bridge.hh>

#include <cstring>

using namespace seastar::xtrader;

// B4: raw SDK records are copied; no API instance or connection is created.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_ctp_bridge_owns_synthetic_callback_payloads) {
    ctp_callback_bridge<4, 4> bridge;
    CThostFtdcTradeField source{};
    std::strcpy(source.TradeID, "trade-1");
    std::strcpy(source.InstrumentID, "rb9999");
    source.Price = 102;
    source.Volume = 2;

    ctp_trader_callback event;
    event.kind = ctp_trader_callback_kind::trade_return;
    event.session_generation = 7;
    event.has_payload = true;
    event.payload.trade = source;
    BOOST_REQUIRE(bridge.post_trader(event));
    std::strcpy(source.TradeID, "changed");
    event.payload.trade.Volume = 99;

    ctp_trader_callback received;
    BOOST_REQUIRE(bridge.pop_trader(received));
    BOOST_CHECK(received.kind == ctp_trader_callback_kind::trade_return);
    BOOST_CHECK_EQUAL(received.session_generation, 7);
    BOOST_CHECK_EQUAL(received.payload.trade.TradeID, "trade-1");
    BOOST_CHECK_EQUAL(received.payload.trade.InstrumentID, "rb9999");
    BOOST_CHECK_EQUAL(received.payload.trade.Price, 102);
    BOOST_CHECK_EQUAL(received.payload.trade.Volume, 2);
    BOOST_CHECK(!bridge.trader_event_lost());
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_bridge_reports_market_and_trade_loss_independently) {
    ctp_callback_bridge<2, 2> bridge;
    ctp_market_callback market;
    market.kind = ctp_market_callback_kind::market_data;
    market.session_generation = 1;
    BOOST_REQUIRE(bridge.post_market(market));
    BOOST_REQUIRE(bridge.post_market(market));
    BOOST_CHECK(!bridge.post_market(market));
    BOOST_CHECK(bridge.market_event_lost());
    BOOST_CHECK(!bridge.trader_event_lost());

    ctp_trader_callback trader;
    trader.kind = ctp_trader_callback_kind::account_query;
    trader.session_generation = 1;
    BOOST_REQUIRE(bridge.post_trader(trader));
    BOOST_REQUIRE(bridge.pop_trader(trader));
    BOOST_CHECK(bridge.market_event_lost());
    BOOST_CHECK(!bridge.trader_event_lost());
}
