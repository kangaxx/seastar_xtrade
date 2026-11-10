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
#include <seastar/xtrader/recovery_coordinator.hh>

#include "xtrader_offline_account_fixture.hh"

#include <stdexcept>

namespace {

using namespace seastar::xtrader;
using namespace seastar::xtrader::test;
using namespace std::chrono_literals;

// B6: protocol fixtures are not a durable journal or authenticated CTP session.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
ctp_query_tracker completed_queries(uint64_t generation, bool all = true) {
    std::vector<ctp_query_kind> kinds{ctp_query_kind::account};
    if (all) {
        kinds.insert(kinds.end(), {ctp_query_kind::instruments, ctp_query_kind::positions,
                                  ctp_query_kind::orders, ctp_query_kind::trades});
    }
    ctp_query_tracker queries(kinds, 10, 10);
    queries.start_session(generation);
    const auto now = ctp_query_tracker::clock::time_point{};
    int id = 0;
    for (const auto kind : kinds) {
        queries.begin({.kind = kind, .request_id = ++id, .minimum_rows = 1,
                       .maximum_rows = 10, .deadline = now + 1s}, now);
        queries.observe({.session_generation = generation, .kind = kind, .request_id = id,
                         .has_row = true, .is_last = true}, now);
    }
    return queries;
}

recovery_evidence evidence() {
    return {
        .account = test_account(), .reconciliation_id = "synthetic-reconciliation",
        .durable_sequence = 8, .applied_sequence = 8, .snapshot_sequence = 3,
        .authenticated = true, .settlement_confirmed = true, .subscriptions_confirmed = true,
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_recovery_requires_bound_approval_after_all_queries) {
    recovery_coordinator coordinator(test_account(), 1s);
    const auto now = recovery_coordinator::clock::time_point{};
    BOOST_CHECK(!coordinator.can_submit_orders());
    coordinator.begin(1, now);
    coordinator.reconcile(completed_queries(1), evidence(), now);
    BOOST_CHECK(coordinator.state() == recovery_state::awaiting_approval);
    BOOST_CHECK(!coordinator.can_submit_orders());
    coordinator.approve(1, "synthetic-reconciliation", "externally-checked-audit", now);
    BOOST_CHECK(coordinator.can_submit_orders());
    coordinator.interrupt("synthetic disconnect");
    BOOST_CHECK(!coordinator.can_submit_orders());
    BOOST_CHECK(coordinator.authorization_audit_id().empty());
    BOOST_CHECK(coordinator.can_cancel_orders(true, true));
    BOOST_CHECK(!coordinator.can_cancel_orders(false, true));
    BOOST_CHECK_THROW(coordinator.begin(1, now), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_recovery_rejects_incomplete_queries_and_account_differences) {
    const auto now = recovery_coordinator::clock::time_point{};
    for (int scenario = 0; scenario < 7; ++scenario) {
        recovery_coordinator coordinator(test_account(), 1s);
        coordinator.begin(2, now);
        auto report = evidence();
        if (scenario == 1) { report.unresolved_orders = 1; }
        if (scenario == 2) { report.fund_differences = 1; }
        if (scenario == 3) { report.position_differences = 1; }
        if (scenario == 4) { report.durable_sequence = 7; }
        if (scenario == 5) { report.account.account_id = "other"; }
        if (scenario == 6) { report.subscriptions_confirmed = false; }
        BOOST_CHECK_THROW(coordinator.reconcile(completed_queries(2, scenario != 0), report, now),
                          std::logic_error);
        BOOST_CHECK(coordinator.state() == recovery_state::manual_intervention);
        BOOST_CHECK(!coordinator.can_submit_orders());
    }
}

SEASTAR_THREAD_TEST_CASE(xtrader_recovery_invalidated_or_expired_approval_cannot_resume) {
    const auto now = recovery_coordinator::clock::time_point{};
    for (int scenario = 0; scenario < 4; ++scenario) {
        recovery_coordinator coordinator(test_account(), 1s);
        coordinator.begin(1, now);
        coordinator.reconcile(completed_queries(1), evidence(), now);
        if (scenario == 0) { coordinator.invalidate_reconciliation("late trade"); }
        BOOST_CHECK_THROW(coordinator.approve(scenario == 1 ? 2 : 1,
            "synthetic-reconciliation", scenario == 3 ? "" : "audit",
            scenario == 2 ? now + 2s : now), std::logic_error);
        BOOST_CHECK(!coordinator.can_submit_orders());
    }
}
