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
#include <seastar/xtrader/offline_account_ledger.hh>

#include <stdexcept>
#include <utility>

namespace seastar::xtrader {

// B2: stage both ledgers before publishing an offline order or settlement.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_account_ledger::offline_account_ledger(position_account_scope account, sstring money_unit)
    : _positions(account), _funds(std::move(account), std::move(money_unit)) {}

void offline_account_ledger::swap_state(offline_account_ledger& other) noexcept {
    using std::swap;
    _positions.swap_state(other._positions);
    _funds.swap_state(other._funds);
    swap(_is_consistent, other._is_consistent);
}

void offline_account_ledger::commit(position_ledger& positions, offline_fund_ledger& funds) noexcept {
    _positions.swap_state(positions);
    _funds.swap_state(funds);
}

void offline_account_ledger::verify_pending_counts(const position_ledger& positions,
                                                   const offline_fund_ledger& funds) {
    if (positions.orders_awaiting_trades() != funds.orders_awaiting_trades()) {
        _is_consistent = false;
        throw std::runtime_error("position and fund terminal trade counts differ");
    }
}

void offline_account_ledger::initialize_flat(int64_t initial_balance) {
    if (!is_consistent()) {
        throw std::logic_error("offline account is inconsistent");
    }
    position_ledger positions(_positions);
    offline_fund_ledger funds(_funds);
    positions.complete_initial_snapshot_without_open_orders();
    funds.initialize_flat(initial_balance);
    commit(positions, funds);
}

void offline_account_ledger::reserve_order(const offline_fund_order& order) {
    if (!can_reserve_orders()) {
        throw std::logic_error("offline account is not ready for new orders");
    }
    position_ledger positions(_positions);
    offline_fund_ledger funds(_funds);
    positions.reserve_order(order.position);
    funds.reserve_order(order);
    verify_pending_counts(positions, funds);
    commit(positions, funds);
}

offline_account_trade_result offline_account_ledger::apply_trade(const offline_fund_trade& trade) {
    if (!is_consistent()) {
        throw std::logic_error("offline account is inconsistent");
    }
    position_ledger positions(_positions);
    offline_fund_ledger funds(_funds);
    try {
        const auto position_result = positions.apply_trade(trade.position);
        const auto fund_result = funds.apply_trade(trade);
        if ((position_result == position_trade_result::duplicate)
            != (fund_result == offline_fund_trade_result::duplicate)) {
            throw std::runtime_error("position and fund trade deduplication differs");
        }
        verify_pending_counts(positions, funds);
        commit(positions, funds);
        return position_result == position_trade_result::duplicate
            ? offline_account_trade_result::duplicate : offline_account_trade_result::applied;
    } catch (const std::runtime_error&) {
        _is_consistent = false;
        throw;
    }
}

void offline_account_ledger::confirm_terminal(const position_account_scope& account, const sstring& order_id,
                                               position_terminal_status status, int cumulative_filled) {
    if (!is_consistent()) {
        throw std::logic_error("offline account is inconsistent");
    }
    position_ledger positions(_positions);
    offline_fund_ledger funds(_funds);
    try {
        positions.confirm_terminal(account, order_id, status, cumulative_filled);
        funds.confirm_terminal(account, order_id, status, cumulative_filled);
        verify_pending_counts(positions, funds);
        commit(positions, funds);
    } catch (const std::runtime_error&) {
        _is_consistent = false;
        throw;
    }
}

offline_account_snapshot offline_account_ledger::snapshot(const position_instrument& instrument) const {
    return {.funds = _funds.snapshot(), .position = _positions.snapshot(instrument)};
}

int64_t offline_account_ledger::position_margin(const position_instrument& instrument,
                                                 domain::side direction) const {
    return _funds.position_margin(instrument, direction);
}

bool offline_account_ledger::can_reserve_orders() const noexcept {
    return is_consistent() && _positions.can_reserve_positions() && _funds.can_reserve_funds();
}

bool offline_account_ledger::is_consistent() const noexcept {
    return _is_consistent && _positions.is_consistent() && _funds.is_consistent();
}

size_t offline_account_ledger::orders_awaiting_trades() const noexcept {
    return _positions.orders_awaiting_trades();
}

} // namespace seastar::xtrader
