/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/ctp_callback_bridge.hh>
#include <seastar/xtrader/ctp_gateway.hh>

#include <memory>
#include <stdexcept>

// B1: startup must fail before any CTP API is created or connected.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_ctp_gateway_start_is_disabled_without_trusted_binding) {
    seastar::xtrader::gateway_config config;
    config.broker_id = "offline-test";
    config.investor_id = "offline-test";
    config.md_front_addr = "tcp://127.0.0.1:1";
    config.td_front_addr = "tcp://127.0.0.1:2";
    auto gateway = std::make_unique<seastar::xtrader::ctp_gateway>(std::move(config));

    BOOST_CHECK_THROW(gateway->start().get(), std::runtime_error);
    BOOST_REQUIRE(gateway->status() == seastar::xtrader::gateway_status::disconnected);
    BOOST_REQUIRE_EQUAL(seastar::xtrader::ctp_gateway::instrument_shard_id("rb9999", 1), 0);
    BOOST_REQUIRE_EQUAL(seastar::xtrader::ctp_gateway::instrument_shard_id("rb9999", 2), 1);
}

// B4: synthetic owner-shard callbacks exercise delivery without connecting CTP.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_ctp_gateway_market_requires_a_handler) {
    using namespace seastar::xtrader;
    auto gateway = std::make_unique<ctp_gateway>(gateway_config{});
    domain::market_data data;
    data.instrument_id = "synthetic";
    gateway->on_market_data(data);
    for (int attempt = 0; attempt < 100 && gateway->status() != gateway_status::error; ++attempt) {
        gateway->drain_md_ring();
    }
    BOOST_CHECK(gateway->status() == gateway_status::error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_gateway_market_handler_delivery_and_failure_are_visible) {
    using namespace seastar::xtrader;
    auto gateway = std::make_unique<ctp_gateway>(gateway_config{});
    unsigned delivered = 0;
    gateway->set_market_handler([&delivered] (const domain::market_data& data) {
        BOOST_CHECK(data.instrument_id == "synthetic");
        ++delivered;
    });
    domain::market_data data;
    data.instrument_id = "synthetic";
    gateway->on_market_data(data);
    for (int attempt = 0; attempt < 100 && delivered == 0; ++attempt) {
        gateway->drain_md_ring();
    }
    BOOST_CHECK_EQUAL(delivered, 1);

    gateway->set_market_handler([] (const domain::market_data&) {
        throw std::runtime_error("synthetic delivery failure");
    });
    gateway->on_market_data(data);
    for (int attempt = 0; attempt < 100 && gateway->status() != gateway_status::error; ++attempt) {
        gateway->drain_md_ring();
    }
    BOOST_CHECK(gateway->status() == gateway_status::error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_gateway_market_queue_overflow_stops_delivery) {
    using namespace seastar::xtrader;
    auto gateway = std::make_unique<ctp_gateway>(gateway_config{});
    domain::market_data data;
    data.instrument_id = "synthetic";
    for (size_t count = 0; count <= md_ring_capacity; ++count) {
        gateway->on_market_data(data);
    }
    BOOST_CHECK(gateway->status() == gateway_status::error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_gateway_missing_callback_bridge_stops_delivery) {
    using namespace seastar::xtrader;
    auto gateway = std::make_unique<ctp_gateway>(gateway_config{});
    unsigned delivered = 0;
    gateway->set_market_handler([&delivered] (const domain::market_data&) { ++delivered; });
    domain::market_data data;
    data.instrument_id = "synthetic";
    gateway->on_market_data(data);
    gateway->post_trader_callback(ctp_trader_callback{});
    gateway->drain_md_ring();
    BOOST_CHECK_EQUAL(delivered, 0);
    BOOST_CHECK(gateway->status() == gateway_status::error);
}
