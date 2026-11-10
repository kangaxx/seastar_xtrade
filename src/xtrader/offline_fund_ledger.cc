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
#include <seastar/xtrader/offline_fund_ledger.hh>

#include "ledger_validation.hh"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B2: monetary operations are checked and never infer exchange fee or margin rules.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
int64_t checked_add(int64_t left, int64_t right) {
    constexpr auto MIN = std::numeric_limits<int64_t>::min();
    constexpr auto MAX = std::numeric_limits<int64_t>::max();
    if ((right > 0 && left > MAX - right) || (right < 0 && left < MIN - right)) {
        throw std::overflow_error("offline fund addition overflow");
    }
    return left + right;
}

int64_t checked_subtract(int64_t left, int64_t right) {
    constexpr auto MIN = std::numeric_limits<int64_t>::min();
    constexpr auto MAX = std::numeric_limits<int64_t>::max();
    if ((right > 0 && left < MIN + right) || (right < 0 && left > MAX + right)) {
        throw std::overflow_error("offline fund subtraction overflow");
    }
    return left - right;
}

int64_t checked_reservation(int64_t per_lot, int volume) {
    if (per_lot < 0 || volume < 0) {
        throw std::invalid_argument("fund reservation inputs must be nonnegative");
    }
    if (volume != 0 && per_lot > std::numeric_limits<int64_t>::max() / volume) {
        throw std::overflow_error("offline fund reservation overflow");
    }
    return per_lot * volume;
}

int64_t available_funds(const offline_fund_snapshot& account) {
    return checked_subtract(checked_subtract(checked_subtract(
        account.balance, account.position_margin), account.frozen_margin), account.frozen_commission);
}

bool has_valid_close_rule(const position_order& order) {
    return *order.offset == domain::offset_flag::close
        ? ledger_detail::is_valid_close_rule(order.close_rule) : !order.close_rule;
}

bool is_valid_fund_order(const offline_fund_order& order) {
    const auto& position = order.position;
    return ledger_detail::has_identifier(position.order_id)
        && ledger_detail::is_valid_instrument(position.instrument)
        && ledger_detail::is_valid_direction(position.direction)
        && ledger_detail::is_valid_offset(position.offset) && position.volume > 0
        && has_valid_close_rule(position)
        && order.margin_per_lot && *order.margin_per_lot >= 0
        && order.commission_per_lot && *order.commission_per_lot >= 0
        && (*position.offset == domain::offset_flag::open || *order.margin_per_lot == 0);
}

bool is_valid_fund_trade(const offline_fund_trade& trade) {
    const auto& position = trade.position;
    return ledger_detail::has_identifier(position.order_id)
        && ledger_detail::has_identifier(position.trade_id)
        && ledger_detail::is_valid_instrument(position.instrument)
        && ledger_detail::is_valid_direction(position.direction)
        && ledger_detail::is_valid_offset(position.offset) && position.volume > 0
        && position.price && std::isfinite(*position.price) && *position.price > 0.0
        && trade.commission && *trade.commission >= 0 && trade.margin_change && trade.close_profit;
}

domain::side position_direction(domain::side order_direction, domain::offset_flag offset) {
    if (offset == domain::offset_flag::open) {
        return order_direction;
    }
    return order_direction == domain::side::buy ? domain::side::sell : domain::side::buy;
}

} // namespace

bool offline_fund_ledger::margin_key::operator<(const margin_key& other) const noexcept {
    return std::tie(instrument.exchange_id, instrument.instrument_id, instrument.hedge, direction)
         < std::tie(other.instrument.exchange_id, other.instrument.instrument_id,
                    other.instrument.hedge, other.direction);
}

offline_fund_ledger::offline_fund_ledger(position_account_scope account, sstring money_unit)
    : _account(std::move(account)), _money_unit(std::move(money_unit)) {
    if (!ledger_detail::is_valid_account(_account) || !ledger_detail::has_identifier(_money_unit)) {
        throw std::invalid_argument("fund account scope, trading day, and money unit are required");
    }
}

void offline_fund_ledger::swap_state(offline_fund_ledger& other) noexcept {
    using std::swap;
    swap(_snapshot, other._snapshot);
    _orders.swap(other._orders);
    _trades.swap(other._trades);
    _position_margin.swap(other._position_margin);
    swap(_orders_awaiting_trades, other._orders_awaiting_trades);
    swap(_initialized, other._initialized);
    swap(_is_consistent, other._is_consistent);
}

void offline_fund_ledger::initialize_flat(int64_t initial_balance) {
    if (_initialized || !_is_consistent) {
        throw std::logic_error("offline fund account is already initialized");
    }
    if (initial_balance < 0) {
        throw std::invalid_argument("initial offline balance must be nonnegative");
    }
    _snapshot.initial_balance = initial_balance;
    _snapshot.balance = initial_balance;
    _snapshot.available = initial_balance;
    _initialized = true;
}

bool offline_fund_ledger::can_reserve_funds() const noexcept {
    return _initialized && _is_consistent && _orders_awaiting_trades == 0 && _snapshot.available >= 0;
}

void offline_fund_ledger::reserve_order(const offline_fund_order& order) {
    if (!can_reserve_funds()) {
        throw std::logic_error("offline fund ledger is not ready for new orders");
    }
    if (order.position.account != _account || !is_valid_fund_order(order)) {
        throw std::invalid_argument("invalid fund reservation or account scope");
    }
    if (_orders.contains(order.position.order_id)) {
        throw std::invalid_argument("fund order identifier is already in use");
    }
    const auto margin = checked_reservation(*order.margin_per_lot, order.position.volume);
    const auto commission = checked_reservation(*order.commission_per_lot, order.position.volume);
    const auto required = checked_add(margin, commission);
    if (_snapshot.available < required) {
        throw std::invalid_argument("insufficient available funds for the order reservation");
    }
    auto next = _snapshot;
    next.frozen_margin = checked_add(next.frozen_margin, margin);
    next.frozen_commission = checked_add(next.frozen_commission, commission);
    next.available = available_funds(next);
    if (!_orders.emplace(order.position.order_id, tracked_order{.request = order}).second) {
        throw std::logic_error("offline fund order identifier collision");
    }
    _snapshot = next;
}

[[noreturn]] void offline_fund_ledger::fail_reconciliation(const char* message) {
    _is_consistent = false;
    throw std::runtime_error(message);
}

offline_fund_trade_result offline_fund_ledger::apply_trade(const offline_fund_trade& trade) {
    if (!_initialized || !_is_consistent) {
        fail_reconciliation("offline fund ledger is not ready to process trade reports");
    }
    if (trade.position.account != _account || !is_valid_fund_trade(trade)) {
        fail_reconciliation("invalid or misrouted offline fund trade report");
    }
    const auto trade_key = std::make_tuple(trade.position.instrument.exchange_id, trade.position.trade_id);
    if (auto existing = _trades.find(trade_key); existing != _trades.end()) {
        if (existing->second == trade) {
            return offline_fund_trade_result::duplicate;
        }
        fail_reconciliation("conflicting duplicate offline fund trade report");
    }
    auto order = _orders.find(trade.position.order_id);
    if (order == _orders.end() || order->second.request.position.instrument != trade.position.instrument
        || order->second.request.position.direction != trade.position.direction
        || order->second.request.position.offset != trade.position.offset) {
        fail_reconciliation("fund trade report does not match a reserved order");
    }
    auto& tracked = order->second;
    const auto update = prepare_trade(trade, tracked);
    commit_trade(trade, trade_key, tracked, update);
    return offline_fund_trade_result::applied;
}

offline_fund_ledger::trade_update offline_fund_ledger::prepare_trade(
        const offline_fund_trade& trade, const tracked_order& order) {
    const int limit = order.terminal_status ? order.terminal_filled : order.request.position.volume;
    if (trade.position.volume > limit - order.applied_volume) {
        fail_reconciliation("fund trade volume exceeds the confirmed order volume");
    }

    int64_t margin_release = 0;
    int64_t fee_release = 0;
    try {
        margin_release = checked_reservation(*order.request.margin_per_lot, trade.position.volume);
        fee_release = checked_reservation(*order.request.commission_per_lot, trade.position.volume);
    } catch (const std::overflow_error&) {
        fail_reconciliation("offline fund trade reservation overflow");
    }
    const bool is_open = *trade.position.offset == domain::offset_flag::open;
    if (margin_release > _snapshot.frozen_margin || fee_release > _snapshot.frozen_commission
        || *trade.commission > fee_release
        || (is_open && (*trade.margin_change < 0 || *trade.margin_change > margin_release
                        || *trade.close_profit != 0))
        || (!is_open && *trade.margin_change > 0)) {
        fail_reconciliation("trade economics exceed the reserved funds or have invalid signs");
    }

    trade_update update{
        .account = _snapshot,
        .position = {trade.position.instrument,
            position_direction(*trade.position.direction, *trade.position.offset)},
        .applied_volume = order.applied_volume + trade.position.volume,
    };
    auto position = _position_margin.find(update.position);
    if (!is_open && position == _position_margin.end()) {
        fail_reconciliation("close trade has no matching funded position");
    }
    const int64_t previous_margin = position == _position_margin.end() ? 0 : position->second;
    auto& next = update.account;
    try {
        update.position_margin = checked_add(previous_margin, *trade.margin_change);
        next.position_margin = checked_add(next.position_margin, *trade.margin_change);
        next.frozen_margin = checked_subtract(next.frozen_margin, margin_release);
        next.frozen_commission = checked_subtract(next.frozen_commission, fee_release);
        next.commission = checked_add(next.commission, *trade.commission);
        next.close_profit = checked_add(next.close_profit, *trade.close_profit);
        next.balance = checked_add(checked_subtract(next.balance, *trade.commission), *trade.close_profit);
        next.available = available_funds(next);
    } catch (const std::overflow_error&) {
        fail_reconciliation("offline fund trade arithmetic overflow");
    }
    if (update.position_margin < 0 || next.position_margin < 0) {
        fail_reconciliation("trade releases more margin than the matching position holds");
    }
    update.resolves_pending = order.terminal_status && order.applied_volume < order.terminal_filled
        && update.applied_volume == order.terminal_filled;
    if (update.resolves_pending && _orders_awaiting_trades == 0) {
        fail_reconciliation("terminal fund trade accounting is inconsistent");
    }
    return update;
}

void offline_fund_ledger::commit_trade(const offline_fund_trade& trade,
        const std::tuple<sstring, sstring>& trade_key, tracked_order& order, const trade_update& update) {
    auto position = _position_margin.find(update.position);
    std::map<margin_key, int64_t> staged_position;
    if (position == _position_margin.end()) {
        staged_position.emplace(update.position, update.position_margin);
    }
    const auto inserted_trade = _trades.emplace(trade_key, trade);
    if (!inserted_trade.second) {
        fail_reconciliation("offline fund trade identifier collision");
    }
    if (position == _position_margin.end()) {
        auto node = staged_position.extract(staged_position.begin());
        const auto inserted_position = _position_margin.insert(std::move(node));
        if (!inserted_position.inserted) {
            _trades.erase(inserted_trade.first);
            fail_reconciliation("offline fund position margin collision");
        }
    } else {
        position->second = update.position_margin;
    }
    _snapshot = update.account;
    order.applied_volume = update.applied_volume;
    if (update.resolves_pending) {
        --_orders_awaiting_trades;
    }
}

void offline_fund_ledger::confirm_terminal(const position_account_scope& account, const sstring& order_id,
                                           position_terminal_status status, int cumulative_filled) {
    if (!_initialized || !_is_consistent) {
        fail_reconciliation("offline fund ledger is not ready to process order reports");
    }
    if (account != _account || !ledger_detail::has_identifier(order_id)) {
        fail_reconciliation("invalid or misrouted terminal fund order report");
    }
    auto order = _orders.find(order_id);
    if (order == _orders.end()) {
        fail_reconciliation("terminal fund report does not match a reserved order");
    }
    auto& tracked = order->second;
    const int volume = tracked.request.position.volume;
    if ((status != position_terminal_status::filled && status != position_terminal_status::canceled
         && status != position_terminal_status::rejected)
        || cumulative_filled < tracked.applied_volume || cumulative_filled > volume
        || (status == position_terminal_status::filled && cumulative_filled != volume)
        || (status == position_terminal_status::canceled && cumulative_filled == volume)
        || (status == position_terminal_status::rejected && cumulative_filled != 0)) {
        fail_reconciliation("invalid cumulative volume or terminal fund order status");
    }
    if (tracked.terminal_status) {
        if (*tracked.terminal_status == status && tracked.terminal_filled == cumulative_filled) {
            return;
        }
        fail_reconciliation("conflicting terminal fund order reports");
    }
    const int unfilled = volume - cumulative_filled;
    const auto margin_release = checked_reservation(*tracked.request.margin_per_lot, unfilled);
    const auto fee_release = checked_reservation(*tracked.request.commission_per_lot, unfilled);
    if (margin_release > _snapshot.frozen_margin || fee_release > _snapshot.frozen_commission) {
        fail_reconciliation("terminal order release exceeds frozen funds");
    }
    auto next = _snapshot;
    try {
        next.frozen_margin = checked_subtract(next.frozen_margin, margin_release);
        next.frozen_commission = checked_subtract(next.frozen_commission, fee_release);
        next.available = available_funds(next);
    } catch (const std::overflow_error&) {
        fail_reconciliation("terminal fund order arithmetic overflow");
    }
    _snapshot = next;
    tracked.terminal_status = status;
    tracked.terminal_filled = cumulative_filled;
    if (cumulative_filled > tracked.applied_volume) {
        ++_orders_awaiting_trades;
    }
}

offline_fund_snapshot offline_fund_ledger::snapshot() const {
    if (!_initialized) {
        throw std::logic_error("offline fund account is not initialized");
    }
    return _snapshot;
}

int64_t offline_fund_ledger::position_margin(const position_instrument& instrument,
                                             domain::side direction) const {
    if (!_initialized) {
        throw std::logic_error("offline fund account is not initialized");
    }
    if (!ledger_detail::is_valid_instrument(instrument)
        || (direction != domain::side::buy && direction != domain::side::sell)) {
        throw std::invalid_argument("invalid instrument or position direction");
    }
    auto position = _position_margin.find({instrument, direction});
    return position == _position_margin.end() ? 0 : position->second;
}

} // namespace seastar::xtrader
