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
#include <seastar/xtrader/pending_order_manager.hh>

#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace seastar::xtrader;

// B4: exercise pending-order identity and status without a CTP connection.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
pending_order make_order(std::string order_ref) {
    pending_order order;
    order.order_ref = std::move(order_ref);
    order.broker_id = "broker";
    order.investor_id = "investor";
    order.front_id = 17;
    order.session_id = 19;
    order.request.strategy_id = "strategy";
    order.request.instrument_id = "rb9999";
    order.request.volume = 3;
    order.request.price = 100;
    return order;
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_pending_order_rejects_duplicate_and_ambiguous_identities) {
    pending_order_manager manager;
    manager.add_order("ref-1", make_order("ref-1"));
    BOOST_CHECK_THROW(manager.add_order("ref-1", make_order("ref-1")), std::invalid_argument);
    BOOST_CHECK_THROW(manager.add_order("ref-2", make_order("wrong-ref")), std::invalid_argument);
    BOOST_CHECK_THROW(manager.set_order_sys_id("missing", "sys-1", "SHFE", 17, 19), std::invalid_argument);
    BOOST_CHECK_THROW(manager.set_order_sys_id("ref-1", "sys-1", "", 17, 19), std::invalid_argument);

    manager.set_order_sys_id("ref-1", "sys-1", "SHFE", 17, 19);
    manager.set_order_sys_id("ref-1", "sys-1", "SHFE", 17, 19);
    manager.add_order("ref-2", make_order("ref-2"));
    BOOST_CHECK_THROW(manager.set_order_sys_id("ref-2", "sys-1", "DCE", 17, 19), std::runtime_error);
    BOOST_CHECK_THROW(manager.set_order_sys_id("ref-1", "sys-2", "SHFE", 17, 19), std::runtime_error);
    BOOST_CHECK_THROW(manager.set_order_sys_id("ref-1", "sys-1", "DCE", 17, 19), std::runtime_error);
    BOOST_CHECK_THROW(manager.set_order_sys_id("ref-1", "sys-1", "SHFE", 18, 19), std::runtime_error);
    BOOST_REQUIRE(manager.get_by_sys_id("sys-1"));
    BOOST_CHECK_EQUAL(manager.get_by_sys_id("sys-1")->order_ref, "ref-1");
    BOOST_CHECK(manager.get_by_ref("ref-2")->order_sys_id.empty());
}

SEASTAR_THREAD_TEST_CASE(xtrader_pending_order_preserves_monotonic_status_and_volume) {
    pending_order_manager manager;
    manager.add_order("ref-1", make_order("ref-1"));
    BOOST_CHECK(!manager.can_cancel("ref-1"));
    BOOST_CHECK_THROW(manager.remove_order("ref-1"), std::logic_error);
    manager.set_order_sys_id("ref-1", "sys-1", "SHFE", 17, 19);
    manager.update_status("ref-1", domain::order_status::accepted);
    BOOST_CHECK(manager.can_cancel("ref-1"));

    manager.update_traded_volume("ref-1", 1);
    manager.update_status("ref-1", domain::order_status::partially_filled);
    BOOST_CHECK_THROW(manager.update_traded_volume("ref-1", 0), std::runtime_error);
    BOOST_CHECK_THROW(manager.update_traded_volume("ref-1", 4), std::runtime_error);
    BOOST_CHECK_THROW(manager.update_status("ref-1", domain::order_status::accepted), std::runtime_error);
    BOOST_CHECK_THROW(manager.update_status("ref-1", domain::order_status::filled), std::runtime_error);
    BOOST_REQUIRE(manager.get_by_ref("ref-1"));
    BOOST_CHECK_EQUAL(manager.get_by_ref("ref-1")->traded_volume, 1);
    BOOST_CHECK(manager.get_by_ref("ref-1")->status == domain::order_status::partially_filled);

    manager.update_traded_volume("ref-1", 3);
    manager.update_status("ref-1", domain::order_status::filled);
    BOOST_CHECK(manager.is_terminal("ref-1"));
    BOOST_CHECK(!manager.can_cancel("ref-1"));
    BOOST_CHECK_THROW(manager.update_status("ref-1", domain::order_status::canceled), std::runtime_error);
    BOOST_CHECK_THROW(manager.update_traded_volume("ref-1", 2), std::runtime_error);
    BOOST_REQUIRE(manager.get_by_sys_id("sys-1"));
    manager.remove_order("ref-1");
    BOOST_CHECK(!manager.get_by_sys_id("sys-1"));
    BOOST_CHECK_EQUAL(manager.pending_count(), 0);
}
