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

#include <seastar/xtrader/offline_futures_account.hh>
#include <seastar/xtrader/simulated_order_book.hh>

#include <map>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {

// B3: synchronous, offline-only matching and account coordination.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct offline_simulated_order_request {
    sstring strategy_id;
    position_instrument instrument;
    std::optional<domain::side> direction;
    std::optional<domain::offset_flag> offset;
    std::optional<close_position_rule> close_rule;
    int volume = 0;
    int64_t limit_price_ticks = 0;
    int64_t maximum_price_ticks = 0;
};

struct offline_simulated_quote {
    uint64_t sequence = 0;
    sstring exchange_id;
    sstring instrument_id;
    int64_t bid_ticks = 0;
    int bid_volume = -1;
    int64_t ask_ticks = 0;
    int ask_volume = -1;
};

struct offline_simulated_bar_open {
    uint64_t sequence = 0;
    sstring exchange_id;
    sstring instrument_id;
    int64_t open_ticks = 0;
};

// One strategy and one initially flat account for one trading day. This
// component owns no gateway and does not expose a production trading entry.
class offline_simulated_execution {
public:
    offline_simulated_execution(position_account_scope account, sstring money_unit, sstring strategy_id,
                                std::vector<offline_futures_terms> contracts, offline_cost_allocation allocation,
                                uint64_t run_id, simulated_market_mode mode,
                                std::optional<simulated_bar_liquidity> bar_liquidity);

    offline_simulated_execution(const offline_simulated_execution&) = delete;
    offline_simulated_execution& operator=(const offline_simulated_execution&) = delete;
    offline_simulated_execution(offline_simulated_execution&&) = delete;
    offline_simulated_execution& operator=(offline_simulated_execution&&) = delete;

    void initialize_flat(int64_t initial_balance);
    simulated_order submit(const offline_simulated_order_request& request);
    simulated_order cancel(const simulated_order_id& id, const sstring& strategy_id);
    [[nodiscard]] simulated_order get_order(const simulated_order_id& id, const sstring& strategy_id) const;
    simulated_match_result on_quote(const offline_simulated_quote& quote);
    simulated_match_result on_bar_open(const offline_simulated_bar_open& bar);

    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;
    [[nodiscard]] offline_normalized_price normalize_price(const sstring& exchange_id, const sstring& instrument_id,
                                                           double price, offline_price_normalization policy) const;
    [[nodiscard]] bool is_consistent() const noexcept;
    [[nodiscard]] bool can_submit_orders() const noexcept;

private:
    using market_key = std::pair<sstring, sstring>;

    struct market_contract {
        position_instrument instrument;
        double price_tick;
    };

    static sstring order_identifier(const simulated_order_id& id);
    static sstring trade_identifier(const simulated_trade_id& id);
    [[nodiscard]] const market_contract& market_terms(const sstring& exchange_id,
                                                      const sstring& instrument_id) const;
    void settle(simulated_match_result& result, int64_t buy_ticks, int64_t sell_ticks,
                offline_futures_account& staged_account) const;

    template <typename Advance>
    simulated_match_result process_event(Advance&& advance, int64_t buy_ticks, int64_t sell_ticks) {
        if (!is_consistent()) {
            throw std::logic_error("offline simulated execution is inconsistent");
        }
        if (!_initialized) {
            throw std::logic_error("offline simulated execution is not initialized");
        }
        try {
            simulated_order_book staged_book(_book);
            offline_futures_account staged_account(_account);
            auto result = advance(staged_book);
            settle(result, buy_ticks, sell_ticks, staged_account);
            _book.swap_state(staged_book);
            _account.swap_state(staged_account);
            return result;
        } catch (const std::logic_error&) {
            _is_consistent = false;
            throw;
        } catch (const std::runtime_error&) {
            _is_consistent = false;
            throw;
        }
    }

    position_account_scope _scope;
    sstring _strategy_id;
    offline_futures_account _account;
    simulated_order_book _book;
    std::map<market_key, market_contract> _markets;
    bool _initialized = false;
    bool _is_consistent = true;
};

} // namespace seastar::xtrader
