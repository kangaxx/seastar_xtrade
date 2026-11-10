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

#include <limits>

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

SEASTAR_THREAD_TEST_CASE(xtrader_trading_account_deducts_commission_and_frozen_amounts) {
    using namespace seastar::xtrader::domain;

    trading_account account {
        .pre_balance = 100000.0,
        .balance = 100000.0,
        .curr_margin = 10000.0,
        .frozen_margin = 500.0,
        .frozen_commission = 10.0,
    };

    BOOST_REQUIRE(account.apply_delta(account_delta {
        .offset = offset_flag::open,
        .commission = 2.5,
        .margin_change = 2000.0,
    }));

    BOOST_REQUIRE_CLOSE_FRACTION(account.balance, 99997.5, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.available, 87487.5, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.curr_margin, 12000.0, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.commission, 2.5, 1e-12);

    BOOST_REQUIRE(account.apply_delta(account_delta {
        .offset = offset_flag::close,
        .commission = 1.5,
        .margin_change = -3000.0,
        .close_profit_change = 100.0,
    }));

    BOOST_REQUIRE_CLOSE_FRACTION(account.balance, 100096.0, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.available, 90586.0, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.curr_margin, 9000.0, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.commission, 4.0, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.close_profit, 100.0, 1e-12);

    const auto balance_before_invalid_delta = account.balance;
    const auto margin_before_invalid_delta = account.curr_margin;
    BOOST_REQUIRE(!account.apply_delta(account_delta {
        .offset = offset_flag::open,
        .commission = std::numeric_limits<double>::quiet_NaN(),
    }));
    BOOST_REQUIRE(!account.apply_delta(account_delta {
        .offset = offset_flag::close,
        .margin_change = -10000.0,
    }));
    BOOST_REQUIRE_CLOSE_FRACTION(account.balance, balance_before_invalid_delta, 1e-12);
    BOOST_REQUIRE_CLOSE_FRACTION(account.curr_margin, margin_before_invalid_delta, 1e-12);
}

// B2: malformed account inputs must not partially change settlement state.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_trading_account_rejects_malformed_state_and_deltas) {
    using namespace seastar::xtrader::domain;
    trading_account account {
        .pre_balance = 100.0,
        .balance = 100.0,
        .available = 75.0,
        .curr_margin = 20.0,
        .frozen_margin = 4.0,
        .frozen_commission = 1.0,
    };
    const auto original = account;
    const auto reject = [&account, &original] (const account_delta& delta) {
        BOOST_CHECK(!account.apply_delta(delta));
        BOOST_CHECK_EQUAL(account.balance, original.balance);
        BOOST_CHECK_EQUAL(account.available, original.available);
        BOOST_CHECK_EQUAL(account.curr_margin, original.curr_margin);
        BOOST_CHECK_EQUAL(account.frozen_margin, original.frozen_margin);
        BOOST_CHECK_EQUAL(account.frozen_commission, original.frozen_commission);
        BOOST_CHECK_EQUAL(account.commission, original.commission);
        BOOST_CHECK_EQUAL(account.close_profit, original.close_profit);
    };
    account_delta delta;
    delta.commission = -1.0;
    reject(delta);
    delta = {};
    delta.direction = static_cast<side>(99);
    reject(delta);
    delta = {};
    delta.offset = static_cast<offset_flag>(99);
    reject(delta);
    delta = {};
    delta.close_profit_change = 1.0;
    reject(delta);
    delta = {};
    delta.traded_volume = -1;
    reject(delta);
    delta = {};
    delta.price = std::numeric_limits<double>::quiet_NaN();
    reject(delta);

    auto invalid_state = original;
    invalid_state.frozen_margin = -1.0;
    BOOST_CHECK(!invalid_state.apply_delta({}));
    BOOST_CHECK_EQUAL(invalid_state.balance, original.balance);
    invalid_state = original;
    invalid_state.position_profit = std::numeric_limits<double>::infinity();
    BOOST_CHECK(!invalid_state.apply_delta({}));
    BOOST_CHECK_EQUAL(invalid_state.balance, original.balance);
}
