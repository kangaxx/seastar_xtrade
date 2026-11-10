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

#include <seastar/core/future.hh>
#include <seastar/xtrader/offline_bar_timeline.hh>
#include <seastar/xtrader/offline_simulated_execution.hh>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace seastar::xtrader {

// B3: isolated bar replay with one strategy and one offline account.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct offline_bar_replay_config {
    position_account_scope account;
    sstring money_unit;
    sstring strategy_id;
    std::vector<offline_futures_terms> contracts;
    std::optional<offline_cost_allocation> cost_allocation;
    std::optional<offline_price_normalization> price_normalization;
    std::optional<simulated_bar_liquidity> bar_liquidity;
    uint64_t run_id = 0;
    int64_t initial_balance = -1;
};

struct offline_bar_open_notice {
    uint64_t sequence = 0;
    int64_t timestamp_nanos = 0;
    sstring exchange_id;
    sstring instrument_id;
    sstring timeframe;
    sstring trading_day;
    double open_price = 0.0;
};

struct offline_completed_bar_notice {
    uint64_t sequence = 0;
    int64_t timestamp_nanos = 0;
    sstring exchange_id;
    sstring instrument_id;
    sstring timeframe;
    sstring trading_day;
    offline_bar_completed_data bar;
};

enum class offline_bar_replay_state {
    ready,
    running,
    completed,
    stopped,
    failed,
};

struct offline_bar_replay_stats {
    size_t open_events = 0;
    size_t completed_bars = 0;
    size_t submitted_orders = 0;
    size_t canceled_orders = 0;
    size_t fills = 0;
    size_t normalized_price_fields = 0;
};

class offline_bar_replay;

// The context is valid until its asynchronous replay callback completes.
class offline_bar_replay_context {
public:
    offline_bar_replay_context(const offline_bar_replay_context&) = delete;
    offline_bar_replay_context& operator=(const offline_bar_replay_context&) = delete;

    simulated_order submit(const offline_simulated_order_request& request);
    simulated_order cancel(const simulated_order_id& id);
    [[nodiscard]] simulated_order get_order(const simulated_order_id& id) const;
    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;
    [[nodiscard]] offline_normalized_price normalize_price(const position_instrument& instrument, double price) const;

private:
    friend class offline_bar_replay;
    explicit offline_bar_replay_context(offline_bar_replay& replay) : _replay(replay) {}
    offline_bar_replay& _replay;
};

struct offline_bar_replay_callbacks {
    std::function<future<>(offline_bar_replay_context&)> on_start;
    std::function<future<>(offline_bar_replay_context&, const offline_bar_open_notice&,
                           const simulated_match_result&)> on_bar_open;
    std::function<future<>(offline_bar_replay_context&, const offline_completed_bar_notice&)> on_bar_completed;
    std::function<future<>(offline_bar_replay_context&)> on_finish;
};

// All methods and callbacks must run on the owning Seastar shard.
class offline_bar_replay : public std::enable_shared_from_this<offline_bar_replay> {
public:
    static std::shared_ptr<offline_bar_replay> create(offline_bar_replay_config config,
                                                       const std::vector<offline_bar_record>& records);

    offline_bar_replay(const offline_bar_replay&) = delete;
    offline_bar_replay& operator=(const offline_bar_replay&) = delete;
    offline_bar_replay(offline_bar_replay&&) = delete;
    offline_bar_replay& operator=(offline_bar_replay&&) = delete;

    future<> run(offline_bar_replay_callbacks callbacks);
    void request_stop() noexcept;

    [[nodiscard]] offline_bar_replay_state state() const noexcept { return _state; }
    [[nodiscard]] const offline_bar_replay_stats& stats() const noexcept { return _stats; }
    [[nodiscard]] size_t active_order_count() const;
    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;

private:
    friend class offline_bar_replay_context;

    offline_bar_replay(offline_bar_replay_config config, const std::vector<offline_bar_record>& records);
    void require_callback() const;
    void fail_if_stopped();
    void prepare_events(offline_price_normalization policy);
    future<> process_open(const offline_bar_timeline_event& event, const offline_bar_replay_callbacks& callbacks);
    future<> process_completed(const offline_bar_timeline_event& event, const offline_bar_replay_callbacks& callbacks);
    void ensure_order_capacity();

    sstring _strategy_id;
    sstring _trading_day;
    offline_simulated_execution _execution;
    std::vector<offline_bar_timeline_event> _events;
    std::vector<simulated_order_id> _order_ids;
    offline_bar_replay_context _context;
    offline_price_normalization _price_policy;
    offline_bar_replay_stats _stats;
    offline_bar_replay_state _state = offline_bar_replay_state::ready;
    bool _callback_active = false;
    bool _stop_requested = false;
    bool _accept_new_orders = false;
};

} // namespace seastar::xtrader
