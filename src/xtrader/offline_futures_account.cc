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
#include <seastar/xtrader/offline_futures_account.hh>

#include "ledger_validation.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B2: preserve the position, money, and FIFO lot states as one transaction.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
int64_t checked_add(int64_t left, int64_t right) {
    constexpr auto MIN = std::numeric_limits<int64_t>::min();
    constexpr auto MAX = std::numeric_limits<int64_t>::max();
    if ((right > 0 && left > MAX - right) || (right < 0 && left < MIN - right)) {
        throw std::overflow_error("offline futures settlement overflow");
    }
    return left + right;
}

bool matches_order(const position_trade& trade, const position_order& order) {
    return trade.account == order.account && trade.order_id == order.order_id
        && trade.instrument == order.instrument && trade.direction == order.direction
        && trade.offset == order.offset;
}

} // namespace

bool offline_futures_account::lot_key::operator<(const lot_key& other) const noexcept {
    return std::tie(instrument, direction) < std::tie(other.instrument, other.direction);
}

offline_futures_account::offline_futures_account(position_account_scope account, sstring money_unit,
        std::vector<offline_futures_terms> contracts, offline_cost_allocation allocation)
    : _scope(account), _money_unit(money_unit), _account(std::move(account), std::move(money_unit)) {
    if (allocation != offline_cost_allocation::fifo || contracts.empty()) {
        throw std::invalid_argument("explicit FIFO allocation and contract terms are required");
    }
    for (auto& terms : contracts) {
        if (terms.money_unit != _money_unit) {
            throw std::invalid_argument("contract and account money units differ");
        }
        auto instrument = terms.instrument;
        if (!_contracts.emplace(std::move(instrument), offline_futures_economics(std::move(terms))).second) {
            throw std::invalid_argument("offline contract terms are duplicated");
        }
    }
}

void offline_futures_account::swap_state(offline_futures_account& other) noexcept {
    using std::swap;
    _account.swap_state(other._account);
    _orders.swap(other._orders);
    _lots.swap(other._lots);
    _trades.swap(other._trades);
    swap(_is_consistent, other._is_consistent);
}

void offline_futures_account::initialize_flat(int64_t initial_balance) {
    if (!is_consistent()) {
        throw std::logic_error("offline futures account is inconsistent");
    }
    _account.initialize_flat(initial_balance);
}

void offline_futures_account::reserve_order(const offline_futures_order& order) {
    if (!can_reserve_orders()) {
        throw std::logic_error("offline futures account is not ready for new orders");
    }
    const auto contract = _contracts.find(order.position.instrument);
    if (contract == _contracts.end() || !ledger_detail::is_valid_direction(order.position.direction)
        || !ledger_detail::is_valid_offset(order.position.offset)) {
        throw std::invalid_argument("order requires configured contract terms and valid flags");
    }
    static_cast<void>(contract->second.price_at_ticks(order.maximum_price_ticks));
    const auto reserved = contract->second.reserve(*order.position.direction, *order.position.offset,
                                                   order.maximum_price_ticks);
    offline_account_ledger staged_account(_account);
    auto staged_orders = _orders;
    staged_account.reserve_order({
        .position = order.position,
        .margin_per_lot = reserved.margin_per_lot,
        .commission_per_lot = reserved.commission_per_lot,
    });
    if (!staged_orders.emplace(order.position.order_id, order).second) {
        throw std::logic_error("offline futures order identifier collision");
    }
    _account.swap_state(staged_account);
    _orders.swap(staged_orders);
}

offline_fill_value offline_futures_account::close_lots(const offline_futures_economics& economics,
        const offline_futures_trade& trade, int64_t maximum_price_ticks, lot_map& staged_lots) const {
    const auto closing_direction = *trade.position.direction;
    const auto position_direction = closing_direction == domain::side::buy
        ? domain::side::sell : domain::side::buy;
    const auto position = staged_lots.find({trade.position.instrument, position_direction});
    if (position == staged_lots.end()) {
        throw std::runtime_error("close trade has no matching opening lots");
    }

    int remaining = trade.position.volume;
    offline_fill_value total;
    auto& lots = position->second;
    while (remaining > 0 && !lots.empty()) {
        auto& lot = lots.front();
        const int volume = std::min(remaining, lot.volume);
        const auto value = economics.close_fill(closing_direction, *trade.position.offset, lot.price_ticks,
                                               trade.price_ticks, maximum_price_ticks, lot.margin_per_lot, volume);
        total.margin_change = checked_add(total.margin_change, value.margin_change);
        total.commission = checked_add(total.commission, value.commission);
        total.close_profit = checked_add(total.close_profit, value.close_profit);
        remaining -= volume;
        lot.volume -= volume;
        if (lot.volume == 0) {
            lots.pop_front();
        }
    }
    if (remaining != 0) {
        throw std::runtime_error("close trade exceeds the matching opening lots");
    }
    if (lots.empty()) {
        staged_lots.erase(position);
    }
    return total;
}

offline_account_trade_result offline_futures_account::apply_trade(const offline_futures_trade& trade) {
    if (!is_consistent()) {
        throw std::logic_error("offline futures account is inconsistent");
    }
    try {
        return apply_trade_impl(trade);
    } catch (const std::logic_error&) {
        _is_consistent = false;
        throw;
    } catch (const std::runtime_error&) {
        _is_consistent = false;
        throw;
    }
}

offline_account_trade_result offline_futures_account::apply_trade_impl(const offline_futures_trade& trade) {
    const auto& position = trade.position;
    if (position.account != _scope || !ledger_detail::has_identifier(position.trade_id)
        || !ledger_detail::has_identifier(position.order_id)
        || !ledger_detail::is_valid_instrument(position.instrument)
        || !ledger_detail::is_valid_direction(position.direction)
        || !ledger_detail::is_valid_offset(position.offset) || position.volume <= 0 || !position.price) {
        throw std::runtime_error("invalid or misrouted offline futures trade");
    }
    const trade_key key{position.instrument.exchange_id, position.trade_id};
    if (const auto existing = _trades.find(key); existing != _trades.end()) {
        if (existing->second == trade) {
            return offline_account_trade_result::duplicate;
        }
        throw std::runtime_error("conflicting duplicate offline futures trade");
    }
    const auto order = _orders.find(position.order_id);
    if (order == _orders.end() || !matches_order(position, order->second.position)) {
        throw std::runtime_error("offline futures trade does not match a reserved order");
    }
    const auto& economics = _contracts.at(position.instrument);
    if (*position.price != economics.price_at_ticks(trade.price_ticks)) {
        throw std::runtime_error("trade price does not match its normalized tick count");
    }

    offline_account_ledger staged_account(_account);
    auto staged_lots = _lots;
    auto staged_trades = _trades;
    offline_fill_value value;
    if (*position.offset == domain::offset_flag::open) {
        value = economics.open_fill(*position.direction, trade.price_ticks,
                                    order->second.maximum_price_ticks, position.volume);
        staged_lots[{position.instrument, *position.direction}].push_back({
            .price_ticks = trade.price_ticks,
            .margin_per_lot = value.margin_change / position.volume,
            .volume = position.volume,
        });
    } else {
        value = close_lots(economics, trade, order->second.maximum_price_ticks, staged_lots);
    }
    const offline_fund_trade valued_trade{
        .position = position,
        .commission = value.commission,
        .margin_change = value.margin_change,
        .close_profit = value.close_profit,
    };
    if (staged_account.apply_trade(valued_trade) != offline_account_trade_result::applied
        || !staged_trades.emplace(key, trade).second) {
        throw std::runtime_error("offline futures trade deduplication is inconsistent");
    }
    _account.swap_state(staged_account);
    _lots.swap(staged_lots);
    _trades.swap(staged_trades);
    return offline_account_trade_result::applied;
}

void offline_futures_account::confirm_terminal(const position_account_scope& account, const sstring& order_id,
                                                position_terminal_status status, int cumulative_filled) {
    if (!is_consistent()) {
        throw std::logic_error("offline futures account is inconsistent");
    }
    offline_account_ledger staged_account(_account);
    try {
        staged_account.confirm_terminal(account, order_id, status, cumulative_filled);
        _account.swap_state(staged_account);
    } catch (const std::runtime_error&) {
        _is_consistent = false;
        throw;
    }
}

offline_account_snapshot offline_futures_account::snapshot(const position_instrument& instrument) const {
    return _account.snapshot(instrument);
}

int64_t offline_futures_account::position_margin(const position_instrument& instrument,
                                                  domain::side direction) const {
    return _account.position_margin(instrument, direction);
}

double offline_futures_account::price_at_ticks(const position_instrument& instrument, int64_t price_ticks) const {
    const auto contract = _contracts.find(instrument);
    if (contract == _contracts.end()) {
        throw std::invalid_argument("price requires configured contract terms");
    }
    return contract->second.price_at_ticks(price_ticks);
}

offline_normalized_price offline_futures_account::normalize_price(
        const position_instrument& instrument, double price, offline_price_normalization policy) const {
    const auto contract = _contracts.find(instrument);
    if (contract == _contracts.end()) {
        throw std::invalid_argument("price requires configured contract terms");
    }
    return contract->second.normalize_price(price, policy);
}

bool offline_futures_account::is_consistent() const noexcept {
    return _is_consistent && _account.is_consistent();
}

bool offline_futures_account::can_reserve_orders() const noexcept {
    return is_consistent() && _account.can_reserve_orders();
}

size_t offline_futures_account::orders_awaiting_trades() const noexcept {
    return _account.orders_awaiting_trades();
}

} // namespace seastar::xtrader
