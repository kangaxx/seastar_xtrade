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
#include <seastar/xtrader/simulated_order_book.hh>

#include "ledger_validation.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B3 offline matching core: no broker access, account settlement, or live entry.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct planned_fill {
    simulated_order* order;
    double price;
    int volume;
};

using ledger_detail::has_identifier;
using ledger_detail::is_valid_direction;
using ledger_detail::is_valid_hedge;
using ledger_detail::is_valid_offset;

bool is_valid_price(double price) {
    return std::isfinite(price) && price > 0.0;
}

void validate_order_request(const simulated_order_request& request) {
    if (!has_identifier(request.strategy_id) || !has_identifier(request.exchange_id)
        || !has_identifier(request.instrument_id) || !is_valid_direction(request.direction)
        || !is_valid_offset(request.offset) || !is_valid_hedge(request.hedge)
        || request.volume <= 0 || !is_valid_price(request.limit_price)) {
        throw std::invalid_argument("invalid simulated order or missing ownership");
    }
    if (!request.type || *request.type != domain::order_type::limit) {
        throw std::invalid_argument("only explicit limit orders are supported by the simulated order book");
    }
}

void append_side_fills(const std::vector<simulated_order*>& candidates, domain::side direction,
                       double price, int liquidity, std::vector<planned_fill>& fills) {
    if (liquidity == 0) {
        return;
    }
    std::vector<simulated_order*> eligible;
    for (auto* order : candidates) {
        if (*order->request.direction != direction) {
            continue;
        }
        const bool crosses = direction == domain::side::buy
            ? order->request.limit_price >= price : order->request.limit_price <= price;
        if (crosses) {
            eligible.push_back(order);
        }
    }
    std::sort(eligible.begin(), eligible.end(), [direction] (const auto* left, const auto* right) {
        if (left->request.limit_price != right->request.limit_price) {
            return direction == domain::side::buy
                ? left->request.limit_price > right->request.limit_price
                : left->request.limit_price < right->request.limit_price;
        }
        return left->id.sequence < right->id.sequence;
    });
    for (auto* order : eligible) {
        const int volume = std::min(liquidity, order->remaining_volume());
        fills.push_back({order, price, volume});
        liquidity -= volume;
        if (liquidity == 0) {
            break;
        }
    }
}

} // namespace

simulated_order_book::simulated_order_book(uint64_t run_id, simulated_market_mode mode,
                                           std::optional<simulated_bar_liquidity> bar_liquidity)
    : _run_id(run_id), _mode(mode), _bar_liquidity(bar_liquidity) {
    if (run_id == 0 || (mode != simulated_market_mode::tick_quote
                        && mode != simulated_market_mode::next_bar_open)) {
        throw std::invalid_argument("a run identifier and supported simulated market mode are required");
    }
    if (mode == simulated_market_mode::tick_quote && bar_liquidity) {
        throw std::invalid_argument("bar liquidity must not be supplied for quote matching");
    }
    if (mode == simulated_market_mode::next_bar_open
        && (!bar_liquidity || bar_liquidity->buy_volume < 0 || bar_liquidity->sell_volume < 0)) {
        throw std::invalid_argument("explicit nonnegative per-side bar liquidity is required");
    }
}

void simulated_order_book::swap_state(simulated_order_book& other) noexcept {
    using std::swap;
    swap(_last_market_sequence, other._last_market_sequence);
    swap(_last_order_sequence, other._last_order_sequence);
    swap(_last_trade_sequence, other._last_trade_sequence);
    _orders.swap(other._orders);
}

simulated_order simulated_order_book::submit(simulated_order_request request) {
    validate_order_request(request);
    if (_last_order_sequence == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("simulated order identifiers are exhausted");
    }
    const simulated_order_id id{_run_id, _last_order_sequence + 1};
    simulated_order order{
        .id = id,
        .request = std::move(request),
        .submitted_after_event = _last_market_sequence,
    };
    auto result = order;
    if (!_orders.emplace(id.sequence, std::move(order)).second) {
        throw std::logic_error("simulated order identifier collision");
    }
    _last_order_sequence = id.sequence;
    return result;
}

simulated_order simulated_order_book::get_order(const simulated_order_id& id,
                                                 const sstring& strategy_id) const {
    const auto order = _orders.find(id.sequence);
    if (id.run_id != _run_id || id.sequence == 0 || order == _orders.end()
        || order->second.request.strategy_id != strategy_id) {
        throw std::invalid_argument("unknown simulated order, run, or strategy owner");
    }
    return order->second;
}

simulated_order simulated_order_book::cancel(const simulated_order_id& id, const sstring& strategy_id) {
    auto result = get_order(id, strategy_id);
    if (!result.is_active()) {
        throw std::logic_error("only an active simulated order can be canceled");
    }
    result.status = simulated_order_status::canceled;
    _orders.find(id.sequence)->second.status = simulated_order_status::canceled;
    return result;
}

void simulated_order_book::validate_market_identity(uint64_t sequence, const sstring& exchange_id,
                                                     const sstring& instrument_id) const {
    if (sequence == 0 || sequence <= _last_market_sequence
        || !has_identifier(exchange_id) || !has_identifier(instrument_id)) {
        throw std::invalid_argument("market event requires an increasing sequence and instrument identity");
    }
}

simulated_match_result simulated_order_book::on_quote(const simulated_quote_event& quote) {
    if (_mode != simulated_market_mode::tick_quote) {
        throw std::logic_error("quote events are not enabled for this simulated order book");
    }
    validate_market_identity(quote.sequence, quote.exchange_id, quote.instrument_id);
    if (!is_valid_price(quote.bid_price) || !is_valid_price(quote.ask_price)
        || quote.bid_volume < 0 || quote.ask_volume < 0 || quote.bid_price > quote.ask_price) {
        throw std::invalid_argument("complete non-crossed best bid and ask with volumes are required");
    }
    return match({
        .sequence = quote.sequence,
        .exchange_id = quote.exchange_id,
        .instrument_id = quote.instrument_id,
        .buy_price = quote.ask_price,
        .sell_price = quote.bid_price,
        .buy_liquidity = quote.ask_volume,
        .sell_liquidity = quote.bid_volume,
        .source = simulated_fill_source::best_quote_snapshot,
    });
}

simulated_match_result simulated_order_book::on_bar_open(const simulated_bar_open_event& bar) {
    if (_mode != simulated_market_mode::next_bar_open) {
        throw std::logic_error("bar events are not enabled for this simulated order book");
    }
    validate_market_identity(bar.sequence, bar.exchange_id, bar.instrument_id);
    if (!is_valid_price(bar.open_price)) {
        throw std::invalid_argument("a finite positive bar opening price is required");
    }
    return match({
        .sequence = bar.sequence,
        .exchange_id = bar.exchange_id,
        .instrument_id = bar.instrument_id,
        .buy_price = bar.open_price,
        .sell_price = bar.open_price,
        .buy_liquidity = _bar_liquidity->buy_volume,
        .sell_liquidity = _bar_liquidity->sell_volume,
        .source = simulated_fill_source::bar_open_approximation,
    });
}

simulated_match_result simulated_order_book::match(const market_snapshot& market) {
    std::vector<simulated_order*> candidates;
    for (auto& entry : _orders) {
        auto& order = entry.second;
        if (order.is_active() && order.submitted_after_event < market.sequence
            && order.request.exchange_id == market.exchange_id
            && order.request.instrument_id == market.instrument_id) {
            candidates.push_back(&order);
        }
    }
    std::vector<planned_fill> fills;
    fills.reserve(candidates.size());
    append_side_fills(candidates, domain::side::buy, market.buy_price, market.buy_liquidity, fills);
    append_side_fills(candidates, domain::side::sell, market.sell_price, market.sell_liquidity, fills);
    if (fills.size() > std::numeric_limits<uint64_t>::max() - _last_trade_sequence) {
        throw std::overflow_error("simulated trade identifiers are exhausted");
    }

    simulated_match_result result{.market_sequence = market.sequence};
    result.trades.reserve(fills.size());
    result.order_updates.reserve(fills.size());
    for (size_t index = 0; index < fills.size(); ++index) {
        const auto& fill = fills[index];
        const auto& order = *fill.order;
        result.trades.push_back({
            .id = {_run_id, _last_trade_sequence + index + 1},
            .order_id = order.id,
            .strategy_id = order.request.strategy_id,
            .exchange_id = order.request.exchange_id,
            .instrument_id = order.request.instrument_id,
            .direction = *order.request.direction,
            .offset = *order.request.offset,
            .hedge = *order.request.hedge,
            .price = fill.price,
            .volume = fill.volume,
            .market_sequence = market.sequence,
            .source = market.source,
            .is_approximate = true,
        });
        auto update = order;
        update.filled_volume += fill.volume;
        update.status = update.remaining_volume() == 0
            ? simulated_order_status::filled : simulated_order_status::partially_filled;
        result.order_updates.push_back(std::move(update));
    }
    for (size_t index = 0; index < fills.size(); ++index) {
        fills[index].order->filled_volume = result.order_updates[index].filled_volume;
        fills[index].order->status = result.order_updates[index].status;
    }
    _last_trade_sequence += fills.size();
    _last_market_sequence = market.sequence;
    return result;
}

} // namespace seastar::xtrader
