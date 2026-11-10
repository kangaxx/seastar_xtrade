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

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace seastar::xtrader {

// B3 offline matching core: later quotes or explicitly approximate bar opens.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class simulated_market_mode {
    tick_quote,
    next_bar_open,
};

enum class simulated_fill_source {
    best_quote_snapshot,
    bar_open_approximation,
};

enum class simulated_order_status {
    accepted,
    partially_filled,
    filled,
    canceled,
};

struct simulated_order_id {
    uint64_t run_id = 0;
    uint64_t sequence = 0;

    bool operator==(const simulated_order_id&) const = default;
};

struct simulated_trade_id {
    uint64_t run_id = 0;
    uint64_t sequence = 0;

    bool operator==(const simulated_trade_id&) const = default;
};

struct simulated_order_request {
    sstring strategy_id;
    sstring exchange_id;
    sstring instrument_id;
    std::optional<domain::side> direction;
    std::optional<domain::offset_flag> offset;
    std::optional<domain::hedge_flag> hedge;
    std::optional<domain::order_type> type;
    int volume = 0;
    double limit_price = 0.0;
};

struct simulated_order {
    simulated_order_id id;
    simulated_order_request request;
    uint64_t submitted_after_event = 0;
    int filled_volume = 0;
    simulated_order_status status = simulated_order_status::accepted;

    [[nodiscard]] int remaining_volume() const noexcept {
        return request.volume - filled_volume;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return status == simulated_order_status::accepted
            || status == simulated_order_status::partially_filled;
    }
};

struct simulated_trade {
    simulated_trade_id id;
    simulated_order_id order_id;
    sstring strategy_id;
    sstring exchange_id;
    sstring instrument_id;
    domain::side direction = domain::side::buy;
    domain::offset_flag offset = domain::offset_flag::open;
    domain::hedge_flag hedge = domain::hedge_flag::speculation;
    double price = 0.0;
    int volume = 0;
    uint64_t market_sequence = 0;
    simulated_fill_source source = simulated_fill_source::best_quote_snapshot;
    bool is_approximate = true;
    std::optional<int64_t> commission_units;
    std::optional<int64_t> margin_change_units;
    std::optional<int64_t> close_profit_units;
};

struct simulated_match_result {
    uint64_t market_sequence = 0;
    std::vector<simulated_trade> trades;
    std::vector<simulated_order> order_updates;
};

struct simulated_quote_event {
    uint64_t sequence = 0;
    sstring exchange_id;
    sstring instrument_id;
    double bid_price = 0.0;
    int bid_volume = -1;
    double ask_price = 0.0;
    int ask_volume = -1;
};

struct simulated_bar_open_event {
    uint64_t sequence = 0;
    sstring exchange_id;
    sstring instrument_id;
    double open_price = 0.0;
};

struct simulated_bar_liquidity {
    int buy_volume = -1;
    int sell_volume = -1;
};

// Serialized, offline matching only. The caller must allocate a unique run_id
// and stable increasing event sequences; neither identifies or authenticates an account.
class simulated_order_book {
public:
    simulated_order_book(uint64_t run_id, simulated_market_mode mode,
                         std::optional<simulated_bar_liquidity> bar_liquidity);

    simulated_order_book& operator=(const simulated_order_book&) = delete;
    simulated_order_book(simulated_order_book&&) = delete;
    simulated_order_book& operator=(simulated_order_book&&) = delete;

    simulated_order submit(simulated_order_request request);
    simulated_order cancel(const simulated_order_id& id, const sstring& strategy_id);
    [[nodiscard]] simulated_order get_order(const simulated_order_id& id,
                                            const sstring& strategy_id) const;

    simulated_match_result on_quote(const simulated_quote_event& quote);
    simulated_match_result on_bar_open(const simulated_bar_open_event& bar);

private:
    friend class offline_simulated_execution;
    simulated_order_book(const simulated_order_book&) = default;
    void swap_state(simulated_order_book& other) noexcept;

    struct market_snapshot {
        uint64_t sequence;
        const sstring& exchange_id;
        const sstring& instrument_id;
        double buy_price;
        double sell_price;
        int buy_liquidity;
        int sell_liquidity;
        simulated_fill_source source;
    };

    void validate_market_identity(uint64_t sequence, const sstring& exchange_id,
                                  const sstring& instrument_id) const;
    simulated_match_result match(const market_snapshot& market);

    uint64_t _run_id;
    simulated_market_mode _mode;
    std::optional<simulated_bar_liquidity> _bar_liquidity;
    uint64_t _last_market_sequence = 0;
    uint64_t _last_order_sequence = 0;
    uint64_t _last_trade_sequence = 0;
    std::map<uint64_t, simulated_order> _orders;
};

} // namespace seastar::xtrader
