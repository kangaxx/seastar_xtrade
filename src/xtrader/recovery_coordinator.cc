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
#include <seastar/xtrader/recovery_coordinator.hh>

#include "ledger_validation.hh"

#include <array>
#include <stdexcept>

namespace seastar::xtrader {

recovery_coordinator::recovery_coordinator(position_account_scope account,
                                           std::chrono::milliseconds approval_window)
    : _account(std::move(account)), _approval_window(approval_window) {
    if (!ledger_detail::is_valid_account(_account) || approval_window.count() <= 0) {
        throw std::invalid_argument("recovery requires an explicit account and positive approval window");
    }
}

void recovery_coordinator::observe_time(clock::time_point now) {
    if (_last_time && now < *_last_time) {
        reject("recovery clock regressed");
    }
    _last_time = now;
}

void recovery_coordinator::begin(uint64_t generation, clock::time_point now) {
    observe_time(now);
    if (generation == 0 || generation <= _generation) {
        reject("recovery requires a new connection generation");
    }
    _generation = generation;
    _state = recovery_state::reconciling;
    _reconciliation_id = {};
    _authorization_audit_id = {};
    _reconciled_at.reset();
    _reason = "queries and durable state reconciliation pending";
}

void recovery_coordinator::reconcile(const ctp_query_tracker& queries, recovery_evidence evidence,
                                      clock::time_point now) {
    observe_time(now);
    if (_state != recovery_state::reconciling || queries.session_generation() != _generation
            || !queries.is_consistent() || !queries.required_queries_completed()
            || (queries.active() && queries.active()->state == ctp_query_state::waiting)) {
        reject("recovery query session is incomplete or inconsistent");
    }
    constexpr std::array REQUIRED{ctp_query_kind::instruments, ctp_query_kind::account,
        ctp_query_kind::positions, ctp_query_kind::orders, ctp_query_kind::trades};
    for (const auto kind : REQUIRED) {
        const auto summary = queries.latest(kind);
        if (!summary || summary->state != ctp_query_state::completed) {
            reject("recovery is missing a required completed query");
        }
    }
    if (evidence.account != _account || !ledger_detail::has_identifier(evidence.reconciliation_id)
            || !evidence.authenticated || !evidence.settlement_confirmed || !evidence.subscriptions_confirmed) {
        reject("recovery identity, settlement or subscriptions are unconfirmed");
    }
    if (evidence.durable_sequence == 0 || evidence.durable_sequence != evidence.applied_sequence
            || evidence.snapshot_sequence > evidence.applied_sequence
            || evidence.unresolved_orders != 0 || evidence.position_differences != 0
            || evidence.fund_differences != 0) {
        reject("recovery has journal gaps, uncertain orders or accounting differences");
    }
    _reconciliation_id = std::move(evidence.reconciliation_id);
    _reconciled_at = now;
    _state = recovery_state::awaiting_approval;
    _reason = "explicit externally verified resume authorization required";
}

void recovery_coordinator::approve(uint64_t generation, const sstring& reconciliation_id,
                                    sstring authorization_audit_id, clock::time_point now) {
    observe_time(now);
    if (_state != recovery_state::awaiting_approval || generation != _generation
            || reconciliation_id != _reconciliation_id || !_reconciled_at
            || now - *_reconciled_at > _approval_window
            || !ledger_detail::has_identifier(authorization_audit_id)) {
        reject("resume approval is missing, expired or does not bind to reconciliation");
    }
    _authorization_audit_id = std::move(authorization_audit_id);
    _state = recovery_state::running;
    _reason = {};
}

void recovery_coordinator::interrupt(sstring reason) {
    _state = recovery_state::blocked;
    _reconciled_at.reset();
    _authorization_audit_id = {};
    _reason = ledger_detail::has_identifier(reason) ? std::move(reason) : sstring("unspecified interruption");
}

void recovery_coordinator::invalidate_reconciliation(sstring reason) {
    interrupt(std::move(reason));
}

bool recovery_coordinator::can_cancel_orders(bool transport_authenticated,
                                              bool order_identity_known) const noexcept {
    return transport_authenticated && order_identity_known;
}

void recovery_coordinator::reject(const char* reason) {
    _state = recovery_state::manual_intervention;
    _reason = reason;
    _reconciled_at.reset();
    _authorization_audit_id = {};
    throw std::logic_error(reason);
}

} // namespace seastar::xtrader
