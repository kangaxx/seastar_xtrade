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

#include <seastar/xtrader/offline_fund_ledger.hh>

namespace seastar::xtrader {

// B2: coordinate offline position and fund changes as one staged transaction.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct offline_account_snapshot {
    offline_fund_snapshot funds;
    position_ledger_snapshot position;
};

enum class offline_account_trade_result {
    applied,
    duplicate,
};

// Single-owner offline primitive; caller-provided economics and scope are not
// authenticated and must not be used as live trading authorization.
class offline_account_ledger {
public:
    offline_account_ledger(position_account_scope account, sstring money_unit);

    offline_account_ledger& operator=(const offline_account_ledger&) = delete;
    offline_account_ledger(offline_account_ledger&&) = delete;
    offline_account_ledger& operator=(offline_account_ledger&&) = delete;

    void initialize_flat(int64_t initial_balance);
    void reserve_order(const offline_fund_order& order);
    offline_account_trade_result apply_trade(const offline_fund_trade& trade);
    void confirm_terminal(const position_account_scope& account, const sstring& order_id,
                          position_terminal_status status, int cumulative_filled);

    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;
    [[nodiscard]] int64_t position_margin(const position_instrument& instrument,
                                          domain::side position_direction) const;
    [[nodiscard]] bool can_reserve_orders() const noexcept;
    [[nodiscard]] bool is_consistent() const noexcept;
    [[nodiscard]] size_t orders_awaiting_trades() const noexcept;

private:
    friend class offline_futures_account;
    offline_account_ledger(const offline_account_ledger&) = default;
    void swap_state(offline_account_ledger& other) noexcept;
    void commit(position_ledger& positions, offline_fund_ledger& funds) noexcept;
    void verify_pending_counts(const position_ledger& positions, const offline_fund_ledger& funds);

    position_ledger _positions;
    offline_fund_ledger _funds;
    bool _is_consistent = true;
};

} // namespace seastar::xtrader
