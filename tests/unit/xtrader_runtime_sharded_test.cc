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
#include <seastar/xtrader/runtime_sharded.hh>

SEASTAR_THREAD_TEST_CASE(xtrader_runtime_sharded_start_submit_stop) {
    seastar::xtrader::runtime_sharded runtime;

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

    const auto st = runtime.submit_order(req).get();
    BOOST_REQUIRE(st == seastar::xtrader::domain::order_status::accepted);

    auto snapshot = runtime.snapshot_positions().get();
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_REQUIRE_EQUAL(snapshot->long_today, 0);
    BOOST_REQUIRE_EQUAL(snapshot->short_today, 0);

    runtime.apply_trade_report(seastar::xtrader::domain::trade_report {
        .trade_id = "trade-1",
        .order_sys_id = "order-1",
        .instrument_id = "rb9999",
        .direction = seastar::xtrader::domain::side::buy,
        .offset = seastar::xtrader::domain::offset_flag::open,
        .price = 3500.0,
        .volume = 1,
        .trade_time = "09:31:00",
    }).get();

    snapshot = runtime.snapshot_positions().get();
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_REQUIRE_EQUAL(snapshot->long_today, 1);
    BOOST_REQUIRE_EQUAL(snapshot->short_today, 0);

    runtime.stop().get();
}
