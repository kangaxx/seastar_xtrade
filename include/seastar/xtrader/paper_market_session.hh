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
#include <seastar/core/gate.hh>
#include <seastar/xtrader/market_data_pipeline.hh>
#include <seastar/xtrader/offline_simulated_execution.hh>

#include <chrono>
#include <functional>

namespace seastar::xtrader {

// B5: market-only input into local matching; no gateway, credentials or CTP API.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
struct paper_market_config {
    position_account_scope account;
    sstring money_unit;
    sstring strategy_id;
    std::vector<offline_futures_terms> contracts;
    offline_cost_allocation allocation;
    uint64_t run_id = 0;
    int64_t initial_balance = 0;
    sstring time_domain;
    std::chrono::milliseconds maximum_quote_age{0};
    offline_price_normalization price_normalization = offline_price_normalization::exact;
};

struct paper_market_result {
    normalized_market_tick tick;
    simulated_match_result matches;
};

// One initially flat account/day. Await every operation on the owner shard.
// Sink completion must mean acknowledged recording, not merely queued work.
// Sinks must use non-blocking I/O; no synchronous Redis/file client is installed.
class paper_market_session {
public:
    using tick_sink = std::function<future<>(const normalized_market_tick&)>;
    using bar_sink = std::function<future<>(const std::vector<recorded_minute_bar>&)>;
    using clock = std::chrono::steady_clock;

    paper_market_session(paper_market_config config, tick_sink record_tick, bar_sink record_final_bars);
    future<paper_market_result> on_tick(market_tick_input tick);
    simulated_order submit(const offline_simulated_order_request& request);
    simulated_order cancel(const simulated_order_id& id, const sstring& strategy_id);
    [[nodiscard]] offline_account_snapshot snapshot(const position_instrument& instrument) const;
    void disconnect();
    future<> stop();
    [[nodiscard]] bool has_failed() const noexcept { return _failed; }

private:
    future<paper_market_result> process_tick(market_tick_input tick, clock::time_point received);
    paper_market_config _config;
    market_data_pipeline _pipeline;
    offline_simulated_execution _execution;
    tick_sink _record_tick;
    bar_sink _record_final_bars;
    std::map<market_data_pipeline::market_key, clock::time_point> _last_quote;
    gate _gate;
    bool _busy = false;
    bool _stopping = false;
    bool _failed = false;
};

} // namespace seastar::xtrader
