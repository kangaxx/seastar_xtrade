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
#include <seastar/xtrader/offline_simulated_execution.hh>

#include "ledger_validation.hh"

#include <stdexcept>
#include <string>
#include <utility>

namespace seastar::xtrader {

// B3: commit order-book changes only with their corresponding offline ledger changes.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
offline_simulated_execution::offline_simulated_execution(position_account_scope account, sstring money_unit,
        sstring strategy_id, std::vector<offline_futures_terms> contracts, offline_cost_allocation allocation,
        uint64_t run_id, simulated_market_mode mode, std::optional<simulated_bar_liquidity> bar_liquidity)
    : _scope(account), _strategy_id(std::move(strategy_id)),
      _account(std::move(account), std::move(money_unit), contracts, allocation),
      _book(run_id, mode, bar_liquidity) {
    if (!ledger_detail::has_identifier(_strategy_id)) {
        throw std::invalid_argument("an explicit offline strategy owner is required");
    }
    for (const auto& terms : contracts) {
        const market_key key{terms.instrument.exchange_id, terms.instrument.instrument_id};
        const auto [existing, inserted] = _markets.emplace(key, market_contract{terms.instrument, terms.price_tick});
        if (!inserted && existing->second.price_tick != terms.price_tick) {
            throw std::invalid_argument("hedge variants of a market must use the same price tick");
        }
    }
}

sstring offline_simulated_execution::order_identifier(const simulated_order_id& id) {
    return sstring("sim-order-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence));
}

sstring offline_simulated_execution::trade_identifier(const simulated_trade_id& id) {
    return sstring("sim-trade-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence));
}

const offline_simulated_execution::market_contract& offline_simulated_execution::market_terms(
        const sstring& exchange_id, const sstring& instrument_id) const {
    const auto market = _markets.find({exchange_id, instrument_id});
    if (market == _markets.end()) {
        throw std::invalid_argument("market event requires configured contract terms");
    }
    return market->second;
}

void offline_simulated_execution::initialize_flat(int64_t initial_balance) {
    if (!is_consistent()) {
        throw std::logic_error("offline simulated execution is inconsistent");
    }
    _account.initialize_flat(initial_balance);
    _initialized = true;
}

simulated_order offline_simulated_execution::submit(const offline_simulated_order_request& request) {
    if (!can_submit_orders()) {
        throw std::logic_error("offline simulated execution is not ready for new orders");
    }
    if (request.strategy_id != _strategy_id || request.limit_price_ticks <= 0
        || request.limit_price_ticks > request.maximum_price_ticks) {
        throw std::invalid_argument("order owner, limit, or maximum settlement price is invalid");
    }
    const auto limit_price = _account.price_at_ticks(request.instrument, request.limit_price_ticks);
    simulated_order_book staged_book(_book);
    offline_futures_account staged_account(_account);
    auto order = staged_book.submit({
        .strategy_id = request.strategy_id,
        .exchange_id = request.instrument.exchange_id,
        .instrument_id = request.instrument.instrument_id,
        .direction = request.direction,
        .offset = request.offset,
        .hedge = request.instrument.hedge,
        .type = domain::order_type::limit,
        .volume = request.volume,
        .limit_price = limit_price,
    });
    staged_account.reserve_order({
        .position = {
            .account = _scope,
            .order_id = order_identifier(order.id),
            .instrument = request.instrument,
            .direction = request.direction,
            .offset = request.offset,
            .volume = request.volume,
            .close_rule = request.close_rule,
        },
        .maximum_price_ticks = request.maximum_price_ticks,
    });
    _book.swap_state(staged_book);
    _account.swap_state(staged_account);
    return order;
}

simulated_order offline_simulated_execution::cancel(const simulated_order_id& id, const sstring& strategy_id) {
    if (!is_consistent()) {
        throw std::logic_error("offline simulated execution is inconsistent");
    }
    simulated_order_book staged_book(_book);
    auto order = staged_book.cancel(id, strategy_id);
    offline_futures_account staged_account(_account);
    try {
        staged_account.confirm_terminal(_scope, order_identifier(id),
                                        position_terminal_status::canceled, order.filled_volume);
        _book.swap_state(staged_book);
        _account.swap_state(staged_account);
        return order;
    } catch (const std::logic_error&) {
        _is_consistent = false;
        throw;
    } catch (const std::runtime_error&) {
        _is_consistent = false;
        throw;
    }
}

simulated_order offline_simulated_execution::get_order(const simulated_order_id& id,
                                                         const sstring& strategy_id) const {
    return _book.get_order(id, strategy_id);
}

void offline_simulated_execution::settle(simulated_match_result& result, int64_t buy_ticks,
                                          int64_t sell_ticks, offline_futures_account& staged_account) const {
    if (result.trades.size() != result.order_updates.size()) {
        throw std::runtime_error("simulated trades and order updates differ");
    }
    for (size_t index = 0; index < result.trades.size(); ++index) {
        auto& trade = result.trades[index];
        const auto& update = result.order_updates[index];
        if (trade.order_id != update.id || trade.strategy_id != _strategy_id) {
            throw std::runtime_error("simulated trade ownership or order update differs");
        }
        const position_instrument instrument{trade.exchange_id, trade.instrument_id, trade.hedge};
        const offline_futures_trade report{
            .position = {
                .account = _scope,
                .trade_id = trade_identifier(trade.id),
                .order_id = order_identifier(trade.order_id),
                .instrument = instrument,
                .direction = trade.direction,
                .offset = trade.offset,
                .volume = trade.volume,
                .price = trade.price,
            },
            .price_ticks = trade.direction == domain::side::buy ? buy_ticks : sell_ticks,
        };
        const auto before = staged_account.snapshot(instrument).funds;
        if (staged_account.apply_trade(report) != offline_account_trade_result::applied) {
            throw std::runtime_error("new simulated trade was already settled");
        }
        const auto after = staged_account.snapshot(instrument).funds;
        trade.commission_units = after.commission - before.commission;
        trade.margin_change_units = after.position_margin - before.position_margin;
        trade.close_profit_units = after.close_profit - before.close_profit;
        if (update.status == simulated_order_status::filled) {
            staged_account.confirm_terminal(_scope, order_identifier(update.id),
                                            position_terminal_status::filled, update.filled_volume);
        }
    }
}

simulated_match_result offline_simulated_execution::on_quote(const offline_simulated_quote& quote) {
    return process_event([&] (simulated_order_book& staged_book) {
        if (quote.bid_ticks > quote.ask_ticks) {
            throw std::invalid_argument("best bid ticks cannot exceed best ask ticks");
        }
        const auto& market = market_terms(quote.exchange_id, quote.instrument_id);
        return staged_book.on_quote({
            .sequence = quote.sequence,
            .exchange_id = quote.exchange_id,
            .instrument_id = quote.instrument_id,
            .bid_price = _account.price_at_ticks(market.instrument, quote.bid_ticks),
            .bid_volume = quote.bid_volume,
            .ask_price = _account.price_at_ticks(market.instrument, quote.ask_ticks),
            .ask_volume = quote.ask_volume,
        });
    }, quote.ask_ticks, quote.bid_ticks);
}

simulated_match_result offline_simulated_execution::on_bar_open(const offline_simulated_bar_open& bar) {
    return process_event([&] (simulated_order_book& staged_book) {
        const auto& market = market_terms(bar.exchange_id, bar.instrument_id);
        return staged_book.on_bar_open({
            .sequence = bar.sequence,
            .exchange_id = bar.exchange_id,
            .instrument_id = bar.instrument_id,
            .open_price = _account.price_at_ticks(market.instrument, bar.open_ticks),
        });
    }, bar.open_ticks, bar.open_ticks);
}

offline_account_snapshot offline_simulated_execution::snapshot(const position_instrument& instrument) const {
    return _account.snapshot(instrument);
}

offline_normalized_price offline_simulated_execution::normalize_price(const sstring& exchange_id,
        const sstring& instrument_id, double price, offline_price_normalization policy) const {
    const auto& market = market_terms(exchange_id, instrument_id);
    return _account.normalize_price(market.instrument, price, policy);
}

bool offline_simulated_execution::is_consistent() const noexcept {
    return _is_consistent && _account.is_consistent();
}

bool offline_simulated_execution::can_submit_orders() const noexcept {
    return is_consistent() && _account.can_reserve_orders();
}

} // namespace seastar::xtrader
