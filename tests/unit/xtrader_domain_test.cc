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
#include <seastar/xtrader/domain_types.hh>

SEASTAR_THREAD_TEST_CASE(xtrader_position_open_close_today) {
    using namespace seastar::xtrader::domain;

    position_book book;
    book.apply_trade(position_delta {
        .direction = side::buy,
        .offset = offset_flag::open,
        .traded_volume = 3,
    });

    auto s = book.snapshot();
    BOOST_REQUIRE_EQUAL(s.long_today, 3);
    BOOST_REQUIRE_EQUAL(s.short_today, 0);

    book.apply_trade(position_delta {
        .direction = side::sell,
        .offset = offset_flag::close_today,
        .traded_volume = 2,
    });

    s = book.snapshot();
    BOOST_REQUIRE_EQUAL(s.long_today, 1);
    BOOST_REQUIRE_EQUAL(s.short_today, 0);
}
