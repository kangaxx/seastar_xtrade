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

#include <seastar/xtrader/domain_types.hh>

#include <cstddef>
#include <map>
#include <optional>
#include <tuple>

namespace seastar::xtrader {

struct position_account_scope {
    sstring environment;
    sstring broker_id;
    sstring account_id;
    sstring trading_day;

    bool operator==(const position_account_scope&) const = default;
};

struct position_instrument {
    sstring exchange_id;
    sstring instrument_id;
    std::optional<domain::hedge_flag> hedge;

    bool operator==(const position_instrument&) const = default;
    bool operator<(const position_instrument& other) const noexcept;
};

enum class close_position_rule {
    today_first,
    yesterday_first,
};

enum class position_terminal_status {
    filled,
    canceled,
    rejected,
};

enum class position_trade_result {
    applied,
    duplicate,
};

struct position_order {
    position_account_scope account;
    sstring order_id;
    position_instrument instrument;
    std::optional<domain::side> direction;
    std::optional<domain::offset_flag> offset;
    int volume = 0;
    std::optional<close_position_rule> close_rule;
};

struct position_trade {
    position_account_scope account;
    sstring trade_id;
    sstring order_id;
    position_instrument instrument;
    std::optional<domain::side> direction;
    std::optional<domain::offset_flag> offset;
    int volume = 0;
    std::optional<double> price;

    bool operator==(const position_trade&) const = default;
};

struct position_ledger_snapshot {
    domain::position_view total;
    domain::position_view frozen;

    [[nodiscard]] domain::position_view available() const noexcept;
};

// Offline position accounting only; callers serialize access on the owner shard.
// The scope identifies a namespace and does not authenticate a trading environment.
class position_ledger {
public:
    explicit position_ledger(position_account_scope account);

    position_ledger& operator=(const position_ledger&) = delete;
    position_ledger(position_ledger&&) = delete;
    position_ledger& operator=(position_ledger&&) = delete;

    void seed_position(const position_instrument& instrument, const domain::position_view& position);
    void complete_initial_snapshot_without_open_orders();

    void reserve_order(const position_order& order);
    position_trade_result apply_trade(const position_trade& trade);
    void confirm_terminal(const position_account_scope& account, const sstring& order_id,
                          position_terminal_status status, int cumulative_filled);

    [[nodiscard]] position_ledger_snapshot snapshot(const position_instrument& instrument) const;
    [[nodiscard]] bool can_reserve_positions() const noexcept;
    [[nodiscard]] bool is_consistent() const noexcept { return _is_consistent; }
    [[nodiscard]] size_t orders_awaiting_trades() const noexcept { return _orders_awaiting_trades; }

private:
    friend class offline_account_ledger;
    position_ledger(const position_ledger&) = default;
    void swap_state(position_ledger& other) noexcept;

    struct tracked_order {
        position_order request;
        int applied_volume = 0;
        int frozen_today = 0;
        int frozen_yesterday = 0;
        std::optional<position_terminal_status> terminal_status;
        int terminal_filled = 0;
    };

    [[noreturn]] void fail_reconciliation(const char* message);

    position_account_scope _account;
    std::map<position_instrument, position_ledger_snapshot> _positions;
    std::map<sstring, tracked_order> _orders;
    std::map<std::tuple<sstring, sstring>, position_trade> _trades;
    size_t _orders_awaiting_trades = 0;
    bool _snapshot_complete = false;
    bool _is_consistent = true;
};

} // namespace seastar::xtrader
