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
#include <seastar/core/smp.hh>
#include <seastar/xtrader/runtime_engine.hh>
#include <seastar/xtrader/trading_account_service.hh>

#include <limits>

// B2: reject invalid offline balances, repeated initialization and invalid risk inputs.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_account_service_requires_valid_single_initialization) {
    seastar::xtrader::trading_account_service service;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    BOOST_CHECK(!service.is_initialized());
    BOOST_CHECK(!service.has_sufficient_balance(0.0));
    BOOST_CHECK(!service.initialize(-1.0));
    BOOST_CHECK(!service.initialize(nan));
    BOOST_CHECK(!service.initialize(infinity));
    BOOST_CHECK(!service.is_initialized());

    BOOST_REQUIRE(service.initialize(100.0));
    BOOST_CHECK(!service.initialize(999.0));
    BOOST_CHECK_EQUAL(service.account().balance, 100.0);
    BOOST_CHECK(service.has_sufficient_balance(0.0));
    BOOST_CHECK(service.has_sufficient_balance(100.0));
    BOOST_CHECK(!service.has_sufficient_balance(101.0));
    BOOST_CHECK(!service.has_sufficient_balance(-1.0));
    BOOST_CHECK(!service.has_sufficient_balance(nan));
    BOOST_CHECK(!service.has_sufficient_balance(infinity));
}

SEASTAR_THREAD_TEST_CASE(xtrader_runtime_account_init_failure_does_not_expose_a_ready_service) {
    seastar::xtrader::runtime_engine engine(seastar::this_shard_id());
    BOOST_CHECK(!engine.init_account_service(std::numeric_limits<double>::quiet_NaN()));
    BOOST_CHECK(!engine.is_account_service_ready());
    BOOST_CHECK(engine.local_account_service() == nullptr);
    BOOST_REQUIRE(engine.init_account_service(100.0));
    BOOST_CHECK(engine.is_account_service_ready());
    BOOST_REQUIRE(engine.local_account_service() != nullptr);
    BOOST_CHECK_EQUAL(engine.local_account_service()->account().balance, 100.0);
    BOOST_CHECK(!engine.init_account_service(200.0));
    BOOST_CHECK_EQUAL(engine.local_account_service()->account().balance, 100.0);
}
