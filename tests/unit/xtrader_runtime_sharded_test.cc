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
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/xtrader/runtime_sharded.hh>

#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>

// B1: verify handler installation, shard validation, and in-flight shutdown.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_runtime_sharded_start_submit_stop) {
    seastar::xtrader::runtime_sharded runtime({.offline_initial_balance = 1000000.0});

    BOOST_CHECK_THROW(runtime.set_order_handler({}).get(), std::runtime_error);
    runtime.start().get();

    const seastar::xtrader::domain::order_request req {
        .strategy_id = "test",
        .instrument_id = "rb9999",
        .direction = seastar::xtrader::domain::side::buy,
        .offset = seastar::xtrader::domain::offset_flag::open,
        .hedge = seastar::xtrader::domain::hedge_flag::speculation,
        .volume = 1,
        .price = 3500.0,
    };

    BOOST_REQUIRE(runtime.submit_order(req).get() == seastar::xtrader::domain::order_status::rejected);
    BOOST_CHECK_THROW(runtime.set_order_handler({}).get(), std::runtime_error);
    BOOST_CHECK_THROW(runtime.set_trade_handler({}).get(), std::runtime_error);
    BOOST_CHECK_THROW(runtime.set_gateway_shard(0), std::logic_error);
    std::atomic<int> handler_calls{0};
    runtime.set_order_handler([&handler_calls](const seastar::xtrader::domain::order_request&) {
        ++handler_calls;
        return seastar::make_ready_future<seastar::xtrader::domain::order_status>(
            seastar::this_shard_id() == 0
                ? seastar::xtrader::domain::order_status::accepted
                : seastar::xtrader::domain::order_status::rejected);
    }).get();
    BOOST_CHECK_THROW(runtime.set_trade_handler(
        [](const seastar::xtrader::domain::trade_report&) {}).get(), std::runtime_error);
    BOOST_REQUIRE(runtime.submit_order({}).get() == seastar::xtrader::domain::order_status::rejected);
    BOOST_REQUIRE_EQUAL(handler_calls.load(), 0);
    if (seastar::this_smp_shard_count() > 1) {
        BOOST_CHECK_THROW(seastar::smp::submit_to(1, [&runtime] {
            return runtime.set_order_handler([](const seastar::xtrader::domain::order_request&) {
                return seastar::make_ready_future<seastar::xtrader::domain::order_status>(
                    seastar::xtrader::domain::order_status::accepted);
            });
        }).get(), std::runtime_error);
        BOOST_CHECK_THROW(seastar::smp::submit_to(1, [&runtime] {
            return runtime.snapshot_positions();
        }).get(), std::runtime_error);
    }
    const auto st = runtime.submit_order(req).get();
    BOOST_REQUIRE(st == seastar::xtrader::domain::order_status::accepted);
    BOOST_REQUIRE_EQUAL(handler_calls.load(), 1);

    auto snapshot = runtime.snapshot_positions().get();
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_REQUIRE_EQUAL(snapshot->long_today, 0);
    BOOST_REQUIRE_EQUAL(snapshot->short_today, 0);

    BOOST_CHECK_THROW(runtime.apply_trade_report({}).get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(runtime.snapshot_positions().get()->long_today, 0);
    BOOST_CHECK_THROW(runtime.apply_trade_report(seastar::xtrader::domain::trade_report {
        .trade_id = "trade-1",
        .order_sys_id = "order-1",
        .instrument_id = "rb9999",
        .direction = seastar::xtrader::domain::side::buy,
        .offset = seastar::xtrader::domain::offset_flag::open,
        .price = 3500.0,
        .volume = 1,
        .trade_time = "09:31:00",
    }).get(), std::runtime_error);

    snapshot = runtime.snapshot_positions().get();
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_REQUIRE_EQUAL(snapshot->long_today, 0);
    BOOST_REQUIRE_EQUAL(snapshot->short_today, 0);

    runtime.stop().get();
    BOOST_REQUIRE(runtime.submit_order(req).get() == seastar::xtrader::domain::order_status::rejected);
    BOOST_CHECK_THROW(runtime.apply_trade_report({}).get(), std::runtime_error);

    runtime.start().get();
    BOOST_REQUIRE(runtime.submit_order(req).get() == seastar::xtrader::domain::order_status::rejected);
    runtime.stop().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_runtime_sharded_rejects_unsupported_shards) {
    seastar::xtrader::runtime_sharded missing_balance;
    BOOST_CHECK_THROW(missing_balance.start().get(), std::runtime_error);
    seastar::xtrader::runtime_sharded invalid_balance({
        .offline_initial_balance = std::numeric_limits<double>::quiet_NaN()});
    BOOST_CHECK_THROW(invalid_balance.start().get(), std::runtime_error);
    seastar::xtrader::runtime_sharded negative_balance({.offline_initial_balance = -1.0});
    BOOST_CHECK_THROW(negative_balance.start().get(), std::runtime_error);

    seastar::xtrader::runtime_sharded mismatched({
        .account_shard = 1, .offline_initial_balance = 1000000.0});
    BOOST_CHECK_THROW(mismatched.start().get(), std::runtime_error);

    seastar::xtrader::runtime_sharded unsupported({
        .account_shard = 1, .offline_initial_balance = 1000000.0});
    unsupported.set_gateway_shard(1);
    BOOST_CHECK_THROW(unsupported.start().get(), std::runtime_error);

    const auto out_of_range = seastar::this_smp_shard_count();
    seastar::xtrader::runtime_sharded invalid({
        .account_shard = out_of_range, .offline_initial_balance = 1000000.0});
    invalid.set_gateway_shard(out_of_range);
    BOOST_CHECK_THROW(invalid.start().get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_runtime_sharded_stop_waits_for_inflight_order) {
    using namespace std::chrono_literals;
    using seastar::xtrader::domain::order_status;
    seastar::xtrader::runtime_sharded runtime({.offline_initial_balance = 1000000.0});
    runtime.start().get();
    runtime.set_order_handler([](const seastar::xtrader::domain::order_request&) {
        return seastar::sleep(20ms).then([] {
            return seastar::make_ready_future<order_status>(order_status::accepted);
        });
    }).get();

    auto pending = runtime.submit_order({
        .strategy_id = "offline-test",
        .instrument_id = "rb9999",
        .volume = 1,
        .price = 3500.0,
    });
    auto stopping = runtime.stop();
    BOOST_REQUIRE(!stopping.available());
    BOOST_REQUIRE(pending.get() == order_status::accepted);
    stopping.get();
}
