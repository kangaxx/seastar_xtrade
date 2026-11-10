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
#pragma once

#include <seastar/xtrader/ctp_query_tracker.hh>
#include <seastar/xtrader/position_ledger.hh>

namespace seastar::xtrader {

// B6: fail-closed recovery protocol, independent of transport and persistence.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
enum class recovery_state {
    blocked,
    reconciling,
    awaiting_approval,
    running,
    manual_intervention,
};

struct recovery_evidence {
    position_account_scope account;
    sstring reconciliation_id;
    uint64_t durable_sequence = 0;
    uint64_t applied_sequence = 0;
    uint64_t snapshot_sequence = 0;
    size_t unresolved_orders = 0;
    size_t position_differences = 0;
    size_t fund_differences = 0;
    bool authenticated = false;
    bool settlement_confirmed = false;
    bool subscriptions_confirmed = false;
};

// Evidence fields are reports from future durable/reconciliation adapters,
// not authentication or proof of fsync. No API in this class sends/retries orders.
class recovery_coordinator {
public:
    using clock = std::chrono::steady_clock;

    recovery_coordinator(position_account_scope account, std::chrono::milliseconds approval_window);
    void begin(uint64_t generation, clock::time_point now);
    void reconcile(const ctp_query_tracker& queries, recovery_evidence evidence, clock::time_point now);
    void approve(uint64_t generation, const sstring& reconciliation_id,
                 sstring authorization_audit_id, clock::time_point now);
    void interrupt(sstring reason);
    // Must be called when callbacks mutate state after reconciliation.
    void invalidate_reconciliation(sstring reason);

    [[nodiscard]] bool can_submit_orders() const noexcept { return _state == recovery_state::running; }
    // Cancel permission is deliberately independent of new-order permission.
    [[nodiscard]] bool can_cancel_orders(bool transport_authenticated, bool order_identity_known) const noexcept;
    [[nodiscard]] recovery_state state() const noexcept { return _state; }
    [[nodiscard]] const sstring& reason() const noexcept { return _reason; }
    [[nodiscard]] const sstring& authorization_audit_id() const noexcept { return _authorization_audit_id; }
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

private:
    void observe_time(clock::time_point now);
    [[noreturn]] void reject(const char* reason);

    position_account_scope _account;
    std::chrono::milliseconds _approval_window;
    recovery_state _state = recovery_state::blocked;
    uint64_t _generation = 0;
    std::optional<clock::time_point> _last_time;
    std::optional<clock::time_point> _reconciled_at;
    sstring _reconciliation_id;
    sstring _authorization_audit_id;
    sstring _reason = "initial reconciliation required";
};

} // namespace seastar::xtrader
