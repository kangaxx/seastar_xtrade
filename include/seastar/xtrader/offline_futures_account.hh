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

#include <seastar/xtrader/offline_account_ledger.hh>
#include <seastar/xtrader/offline_futures_economics.hh>

#include <deque>
#include <map>
#include <tuple>
#include <vector>

namespace seastar::xtrader {

// B2: bind explicit offline economics and opening lots to atomic account state.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class offline_cost_allocation {
    fifo,
};

struct offline_futures_order {
    position_order position;
    int64_t maximum_price_ticks = 0;
};

struct offline_futures_trade {
    position_trade position;
    int64_t price_ticks = 0;

    bool operator==(const offline_futures_trade&) const = default;
};

// Serialized, one-day, initially flat offline account. FIFO is an explicitly
// selected simulation model, not an exchange rule or trading authorization.
class offline_futures_account {
public:
    offline_futures_account(position_account_scope account, sstring money_unit,
                            std::vector<offline_futures_terms> contracts,
                            offline_cost_allocation allocation);

    offline_futures_account& operator=(const offline_futures_account&) = delete;
    offline_futures_account(offline_futures_account&&) = delete;
    offline_futures_account& operator=(offline_futures_account&&) = delete;

    void initialize_flat(int64_t initial_balance);
    void reserve_order(const offline_futures_order& order);
    offline_account_trade_result apply_trade(const offline_futures_trade& trade);
    void confirm_terminal(const position_account_scope& account, const sstring& order_id,
                          position_terminal_status status, int cumulative_filled);

    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;
    [[nodiscard]] int64_t position_margin(const position_instrument& instrument,
                                          domain::side position_direction) const;
    [[nodiscard]] double price_at_ticks(const position_instrument& instrument, int64_t price_ticks) const;
    [[nodiscard]] offline_normalized_price normalize_price(const position_instrument& instrument, double price,
                                                           offline_price_normalization policy) const;
    [[nodiscard]] bool is_consistent() const noexcept;
    [[nodiscard]] bool can_reserve_orders() const noexcept;
    [[nodiscard]] size_t orders_awaiting_trades() const noexcept;

private:
    friend class offline_simulated_execution;
    offline_futures_account(const offline_futures_account&) = default;
    void swap_state(offline_futures_account& other) noexcept;

    struct lot_key {
        position_instrument instrument;
        domain::side direction;

        bool operator<(const lot_key& other) const noexcept;
    };

    struct opening_lot {
        int64_t price_ticks;
        int64_t margin_per_lot;
        int volume;
    };

    using lot_map = std::map<lot_key, std::deque<opening_lot>>;
    using trade_key = std::tuple<sstring, sstring>;

    offline_account_trade_result apply_trade_impl(const offline_futures_trade& trade);
    offline_fill_value close_lots(const offline_futures_economics& economics,
                                  const offline_futures_trade& trade, int64_t maximum_price_ticks,
                                  lot_map& staged_lots) const;

    position_account_scope _scope;
    sstring _money_unit;
    offline_account_ledger _account;
    std::map<position_instrument, offline_futures_economics> _contracts;
    std::map<sstring, offline_futures_order> _orders;
    lot_map _lots;
    std::map<trade_key, offline_futures_trade> _trades;
    bool _is_consistent = true;
};

} // namespace seastar::xtrader
