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
#include <seastar/xtrader/position_ledger.hh>

#include "ledger_validation.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

using ledger_detail::has_identifier;
using ledger_detail::is_valid_account;
using ledger_detail::is_valid_close_rule;
using ledger_detail::is_valid_direction;
using ledger_detail::is_valid_instrument;
using ledger_detail::is_valid_offset;

bool is_nonnegative(const domain::position_view& position) {
    return position.long_today >= 0 && position.long_yesterday >= 0
        && position.short_today >= 0 && position.short_yesterday >= 0;
}

struct close_buckets {
    int& today;
    int& yesterday;
    int& frozen_today;
    int& frozen_yesterday;
};

close_buckets closing_buckets(position_ledger_snapshot& position, domain::side direction) {
    if (direction == domain::side::buy) {
        return {position.total.short_today, position.total.short_yesterday,
                position.frozen.short_today, position.frozen.short_yesterday};
    }
    return {position.total.long_today, position.total.long_yesterday,
            position.frozen.long_today, position.frozen.long_yesterday};
}

bool consumes_today_first(const position_order& order) {
    return *order.offset == domain::offset_flag::close_today
        || (*order.offset == domain::offset_flag::close
            && *order.close_rule == close_position_rule::today_first);
}

} // namespace

bool position_instrument::operator<(const position_instrument& other) const noexcept {
    return std::tie(exchange_id, instrument_id, hedge)
         < std::tie(other.exchange_id, other.instrument_id, other.hedge);
}

domain::position_view position_ledger_snapshot::available() const noexcept {
    return {
        .long_today = total.long_today - frozen.long_today,
        .long_yesterday = total.long_yesterday - frozen.long_yesterday,
        .short_today = total.short_today - frozen.short_today,
        .short_yesterday = total.short_yesterday - frozen.short_yesterday,
    };
}

position_ledger::position_ledger(position_account_scope account)
    : _account(std::move(account)) {
    if (!is_valid_account(_account)) {
        throw std::invalid_argument("account scope and valid trading day are required");
    }
}

// B2: commit a staged offline account transaction without partially updating positions.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
void position_ledger::swap_state(position_ledger& other) noexcept {
    using std::swap;
    _positions.swap(other._positions);
    _orders.swap(other._orders);
    _trades.swap(other._trades);
    swap(_orders_awaiting_trades, other._orders_awaiting_trades);
    swap(_snapshot_complete, other._snapshot_complete);
    swap(_is_consistent, other._is_consistent);
}

void position_ledger::seed_position(const position_instrument& instrument,
                                    const domain::position_view& position) {
    if (_snapshot_complete || !_is_consistent) {
        throw std::logic_error("initial position snapshot is already closed");
    }
    if (!is_valid_instrument(instrument) || !is_nonnegative(position)) {
        throw std::invalid_argument("invalid instrument or initial position");
    }
    if (!_positions.emplace(instrument, position_ledger_snapshot{.total = position}).second) {
        throw std::invalid_argument("instrument is duplicated in the initial position snapshot");
    }
}

void position_ledger::complete_initial_snapshot_without_open_orders() {
    if (_snapshot_complete || !_is_consistent) {
        throw std::logic_error("initial position snapshot is already closed");
    }
    _snapshot_complete = true;
}

bool position_ledger::can_reserve_positions() const noexcept {
    return _snapshot_complete && _is_consistent && _orders_awaiting_trades == 0;
}

void position_ledger::reserve_order(const position_order& order) {
    if (!can_reserve_positions()) {
        throw std::logic_error("position ledger is not ready for new orders");
    }
    if (order.account != _account || !has_identifier(order.order_id)
        || !is_valid_instrument(order.instrument) || !is_valid_direction(order.direction)
        || !is_valid_offset(order.offset) || order.volume <= 0) {
        throw std::invalid_argument("invalid position order or account scope");
    }
    if ((*order.offset == domain::offset_flag::close && !is_valid_close_rule(order.close_rule))
        || (*order.offset != domain::offset_flag::close && order.close_rule)) {
        throw std::invalid_argument("generic close orders require an explicit allocation rule");
    }
    if (_orders.contains(order.order_id)) {
        throw std::invalid_argument("position order identifier is already in use");
    }

    auto existing = _positions.find(order.instrument);
    position_ledger_snapshot updated = existing == _positions.end()
        ? position_ledger_snapshot{} : existing->second;
    tracked_order tracked{.request = order};
    if (*order.offset != domain::offset_flag::open) {
        auto buckets = closing_buckets(updated, *order.direction);
        const int available_today = buckets.today - buckets.frozen_today;
        const int available_yesterday = buckets.yesterday - buckets.frozen_yesterday;
        const long long available = *order.offset == domain::offset_flag::close_today
            ? available_today : *order.offset == domain::offset_flag::close_yesterday
            ? available_yesterday : static_cast<long long>(available_today) + available_yesterday;
        if (order.volume > available) {
            throw std::invalid_argument("insufficient unfrozen position for close order");
        }
        if (consumes_today_first(order)) {
            tracked.frozen_today = std::min(order.volume, available_today);
            tracked.frozen_yesterday = order.volume - tracked.frozen_today;
        } else {
            tracked.frozen_yesterday = std::min(order.volume, available_yesterday);
            tracked.frozen_today = order.volume - tracked.frozen_yesterday;
        }
        buckets.frozen_today += tracked.frozen_today;
        buckets.frozen_yesterday += tracked.frozen_yesterday;
    }

    auto position = _positions.try_emplace(order.instrument).first;
    _orders.emplace(order.order_id, std::move(tracked));
    position->second = updated;
}

[[noreturn]] void position_ledger::fail_reconciliation(const char* message) {
    _is_consistent = false;
    throw std::runtime_error(message);
}

position_trade_result position_ledger::apply_trade(const position_trade& trade) {
    if (!_snapshot_complete || !_is_consistent) {
        fail_reconciliation("position ledger is not ready to process trade reports");
    }
    if (trade.account != _account || !has_identifier(trade.trade_id)
        || !has_identifier(trade.order_id) || !is_valid_instrument(trade.instrument)
        || !is_valid_direction(trade.direction) || !is_valid_offset(trade.offset)
        || trade.volume <= 0 || !trade.price || *trade.price <= 0.0
        || !std::isfinite(*trade.price)) {
        fail_reconciliation("invalid or misrouted position trade report");
    }

    const auto trade_key = std::make_tuple(trade.instrument.exchange_id, trade.trade_id);
    if (auto existing = _trades.find(trade_key); existing != _trades.end()) {
        if (existing->second == trade) {
            return position_trade_result::duplicate;
        }
        fail_reconciliation("conflicting duplicate trade report");
    }

    auto order = _orders.find(trade.order_id);
    if (order == _orders.end() || order->second.request.instrument != trade.instrument
        || order->second.request.direction != trade.direction
        || order->second.request.offset != trade.offset) {
        fail_reconciliation("trade report does not match a reserved order");
    }
    auto& tracked = order->second;
    const int limit = tracked.terminal_status ? tracked.terminal_filled : tracked.request.volume;
    if (trade.volume > limit - tracked.applied_volume) {
        fail_reconciliation("trade volume exceeds the confirmed order volume");
    }
    auto position = _positions.find(trade.instrument);
    if (position == _positions.end()) {
        fail_reconciliation("position for reserved order is missing");
    }

    auto updated = position->second;
    int consumed_today = 0;
    int consumed_yesterday = 0;
    if (*trade.offset == domain::offset_flag::open) {
        int& today = *trade.direction == domain::side::buy
            ? updated.total.long_today : updated.total.short_today;
        if (trade.volume > std::numeric_limits<int>::max() - today) {
            fail_reconciliation("position volume overflow");
        }
        today += trade.volume;
    } else {
        if (consumes_today_first(tracked.request)) {
            consumed_today = std::min(trade.volume, tracked.frozen_today);
            consumed_yesterday = trade.volume - consumed_today;
        } else {
            consumed_yesterday = std::min(trade.volume, tracked.frozen_yesterday);
            consumed_today = trade.volume - consumed_yesterday;
        }
        auto buckets = closing_buckets(updated, *trade.direction);
        if (consumed_today > tracked.frozen_today || consumed_yesterday > tracked.frozen_yesterday
            || consumed_today > buckets.today || consumed_yesterday > buckets.yesterday
            || consumed_today > buckets.frozen_today || consumed_yesterday > buckets.frozen_yesterday) {
            fail_reconciliation("trade exceeds the reserved close position");
        }
        buckets.today -= consumed_today;
        buckets.yesterday -= consumed_yesterday;
        buckets.frozen_today -= consumed_today;
        buckets.frozen_yesterday -= consumed_yesterday;
    }

    const int new_applied = tracked.applied_volume + trade.volume;
    const bool resolves_pending = tracked.terminal_status
        && tracked.applied_volume < tracked.terminal_filled
        && new_applied == tracked.terminal_filled;
    if (resolves_pending && _orders_awaiting_trades == 0) {
        fail_reconciliation("terminal trade accounting is inconsistent");
    }
    _trades.emplace(trade_key, trade);
    position->second = updated;
    tracked.applied_volume = new_applied;
    tracked.frozen_today -= consumed_today;
    tracked.frozen_yesterday -= consumed_yesterday;
    if (resolves_pending) {
        --_orders_awaiting_trades;
    }
    return position_trade_result::applied;
}

void position_ledger::confirm_terminal(const position_account_scope& account, const sstring& order_id,
                                       position_terminal_status status, int cumulative_filled) {
    if (!_snapshot_complete || !_is_consistent) {
        fail_reconciliation("position ledger is not ready to process order reports");
    }
    if (account != _account || !has_identifier(order_id)) {
        fail_reconciliation("invalid or misrouted terminal order report");
    }
    auto order = _orders.find(order_id);
    if (order == _orders.end()) {
        fail_reconciliation("terminal report does not match a reserved order");
    }
    auto& tracked = order->second;
    if ((status != position_terminal_status::filled && status != position_terminal_status::canceled
         && status != position_terminal_status::rejected)
        || cumulative_filled < tracked.applied_volume || cumulative_filled > tracked.request.volume
        || (status == position_terminal_status::filled && cumulative_filled != tracked.request.volume)
        || (status == position_terminal_status::canceled && cumulative_filled == tracked.request.volume)
        || (status == position_terminal_status::rejected && cumulative_filled != 0)) {
        fail_reconciliation("invalid cumulative volume or terminal order status");
    }
    if (tracked.terminal_status) {
        if (*tracked.terminal_status == status && tracked.terminal_filled == cumulative_filled) {
            return;
        }
        fail_reconciliation("conflicting terminal order reports");
    }

    auto position = _positions.find(tracked.request.instrument);
    if (position == _positions.end()) {
        fail_reconciliation("position for terminal order is missing");
    }
    auto updated = position->second;
    int release_today = 0;
    int release_yesterday = 0;
    if (*tracked.request.offset != domain::offset_flag::open) {
        const int unfilled = tracked.request.volume - cumulative_filled;
        if (consumes_today_first(tracked.request)) {
            release_yesterday = std::min(unfilled, tracked.frozen_yesterday);
            release_today = unfilled - release_yesterday;
        } else {
            release_today = std::min(unfilled, tracked.frozen_today);
            release_yesterday = unfilled - release_today;
        }
        auto buckets = closing_buckets(updated, *tracked.request.direction);
        if (release_today > tracked.frozen_today || release_yesterday > tracked.frozen_yesterday
            || release_today > buckets.frozen_today || release_yesterday > buckets.frozen_yesterday) {
            fail_reconciliation("terminal order release exceeds the frozen position");
        }
        buckets.frozen_today -= release_today;
        buckets.frozen_yesterday -= release_yesterday;
    }
    position->second = updated;
    tracked.frozen_today -= release_today;
    tracked.frozen_yesterday -= release_yesterday;
    tracked.terminal_status = status;
    tracked.terminal_filled = cumulative_filled;
    if (cumulative_filled > tracked.applied_volume) {
        ++_orders_awaiting_trades;
    }
}

position_ledger_snapshot position_ledger::snapshot(const position_instrument& instrument) const {
    if (!_snapshot_complete) {
        throw std::logic_error("initial position snapshot is not complete");
    }
    if (!is_valid_instrument(instrument)) {
        throw std::invalid_argument("invalid position instrument");
    }
    auto position = _positions.find(instrument);
    return position == _positions.end() ? position_ledger_snapshot{} : position->second;
}

} // namespace seastar::xtrader
