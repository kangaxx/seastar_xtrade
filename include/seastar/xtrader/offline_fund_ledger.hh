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

#include <seastar/xtrader/position_ledger.hh>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

namespace seastar::xtrader {

// B2 offline fund reservations with explicit integer monetary units.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct offline_fund_order {
    position_order position;
    std::optional<int64_t> margin_per_lot;
    std::optional<int64_t> commission_per_lot;
};

struct offline_fund_trade {
    position_trade position;
    std::optional<int64_t> commission;
    std::optional<int64_t> margin_change;
    std::optional<int64_t> close_profit;

    bool operator==(const offline_fund_trade&) const = default;
};

struct offline_fund_snapshot {
    int64_t initial_balance = 0;
    int64_t balance = 0;
    int64_t available = 0;
    int64_t position_margin = 0;
    int64_t frozen_margin = 0;
    int64_t frozen_commission = 0;
    int64_t commission = 0;
    int64_t close_profit = 0;
};

enum class offline_fund_trade_result {
    applied,
    duplicate,
};

// Callers serialize access and provide precomputed economics. The scope and
// unit are namespaces, not environment authentication or a fee schedule.
class offline_fund_ledger {
public:
    offline_fund_ledger(position_account_scope account, sstring money_unit);

    offline_fund_ledger& operator=(const offline_fund_ledger&) = delete;
    offline_fund_ledger(offline_fund_ledger&&) = delete;
    offline_fund_ledger& operator=(offline_fund_ledger&&) = delete;

    void initialize_flat(int64_t initial_balance);
    void reserve_order(const offline_fund_order& order);
    offline_fund_trade_result apply_trade(const offline_fund_trade& trade);
    void confirm_terminal(const position_account_scope& account, const sstring& order_id,
                          position_terminal_status status, int cumulative_filled);

    [[nodiscard]] offline_fund_snapshot snapshot() const;
    // buy identifies a long position; sell identifies a short position.
    [[nodiscard]] int64_t position_margin(const position_instrument& instrument,
                                          domain::side position_direction) const;
    [[nodiscard]] const sstring& money_unit() const noexcept { return _money_unit; }
    [[nodiscard]] bool can_reserve_funds() const noexcept;
    [[nodiscard]] bool is_consistent() const noexcept { return _is_consistent; }
    [[nodiscard]] size_t orders_awaiting_trades() const noexcept { return _orders_awaiting_trades; }

private:
    friend class offline_account_ledger;
    offline_fund_ledger(const offline_fund_ledger&) = default;
    void swap_state(offline_fund_ledger& other) noexcept;

    struct margin_key {
        position_instrument instrument;
        domain::side direction;

        bool operator<(const margin_key& other) const noexcept;
    };

    struct tracked_order {
        offline_fund_order request;
        int applied_volume = 0;
        std::optional<position_terminal_status> terminal_status;
        int terminal_filled = 0;
    };

    struct trade_update {
        offline_fund_snapshot account;
        margin_key position;
        int64_t position_margin = 0;
        int applied_volume = 0;
        bool resolves_pending = false;
    };

    trade_update prepare_trade(const offline_fund_trade& trade, const tracked_order& order);
    void commit_trade(const offline_fund_trade& trade, const std::tuple<sstring, sstring>& trade_key,
                      tracked_order& order, const trade_update& update);
    [[noreturn]] void fail_reconciliation(const char* message);

    position_account_scope _account;
    sstring _money_unit;
    offline_fund_snapshot _snapshot;
    std::map<sstring, tracked_order> _orders;
    std::map<std::tuple<sstring, sstring>, offline_fund_trade> _trades;
    std::map<margin_key, int64_t> _position_margin;
    size_t _orders_awaiting_trades = 0;
    bool _initialized = false;
    bool _is_consistent = true;
};

} // namespace seastar::xtrader
