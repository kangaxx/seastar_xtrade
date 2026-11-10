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

#include <seastar/xtrader/offline_bar_replay.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>
#include <seastar/xtrader/subscription_registry.hh>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <variant>

namespace seastar::xtrader {

// B3: bridge one existing strategy to an isolated, single-contract bar replay.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct offline_calendar_time {
    sstring calendar_date;
    sstring time;
};

struct offline_bar_strategy_config {
    offline_bar_replay_config replay;
    domain::instrument_spec instrument_spec;
    int64_t maximum_price_ticks = 0;
    int64_t money_units_per_currency = 0;
    std::optional<close_position_rule> close_rule;
    size_t max_notifications_per_boundary = 0;
    std::function<offline_calendar_time(int64_t)> format_timestamp;
};

// Account queries fail explicitly: the offline model does not yet calculate
// mark-to-market equity. All methods must run on the owning Seastar shard.
class offline_bar_strategy_replay : public std::enable_shared_from_this<offline_bar_strategy_replay> {
public:
    static std::shared_ptr<offline_bar_strategy_replay> create(
        offline_bar_strategy_config config, std::shared_ptr<strategy_base> strategy,
        const std::vector<offline_bar_record>& records);

    offline_bar_strategy_replay(const offline_bar_strategy_replay&) = delete;
    offline_bar_strategy_replay& operator=(const offline_bar_strategy_replay&) = delete;

    future<> run();
    void request_stop() noexcept;

    [[nodiscard]] offline_bar_replay_state state() const noexcept { return _replay->state(); }
    [[nodiscard]] const offline_bar_replay_stats& stats() const noexcept { return _replay->stats(); }
    [[nodiscard]] size_t active_order_count() const { return _replay->active_order_count(); }
    [[nodiscard]] offline_account_snapshot snapshot() const { return _replay->snapshot(_instrument); }
    [[nodiscard]] size_t normalized_order_prices() const noexcept { return _normalized_order_prices; }

private:
    using notification = std::variant<domain::order, domain::trade_report>;

    offline_bar_strategy_replay(offline_bar_strategy_config config, std::shared_ptr<strategy_base> strategy,
                                const std::vector<offline_bar_record>& records);
    void configure_manager();
    void prepare_time_labels(const std::vector<offline_bar_record>& records);
    offline_bar_replay_context& active_context() const;
    domain::order_status submit_order(const domain::order_request& request);
    domain::order_status cancel_order(const sstring& order_sys_id);
    domain::position_view query_position() const;
    domain::order make_order_update(const simulated_order& order) const;
    domain::trade_report make_trade_update(const simulated_trade& trade, uint64_t event_sequence) const;
    void enqueue(notification value);
    void require_notification_space(size_t count) const;
    future<> flush_notifications();
    future<> start_strategy(offline_bar_replay_context& context);
    future<> dispatch_open(offline_bar_replay_context& context, const offline_bar_open_notice& notice,
                           const simulated_match_result& result);
    future<> dispatch_bar(offline_bar_replay_context& context, const offline_completed_bar_notice& notice);
    future<> finish_strategy(offline_bar_replay_context& context);

    offline_bar_strategy_config _config;
    std::shared_ptr<strategy_base> _strategy;
    position_instrument _instrument;
    std::shared_ptr<offline_bar_replay> _replay;
    subscription_registry _subscriptions;
    strategy_lifecycle_manager _manager;
    std::map<uint64_t, offline_calendar_time> _time_labels;
    std::deque<notification> _pending;
    offline_bar_replay_context* _active_context = nullptr;
    size_t _normalized_order_prices = 0;
    bool _notifications_consistent = true;
    bool _shutdown_phase = false;
    bool _run_started = false;
};

} // namespace seastar::xtrader
