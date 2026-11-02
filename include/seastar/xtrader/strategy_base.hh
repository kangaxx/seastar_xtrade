/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/xtrader/domain_types.hh>

#include <functional>
#include <string>
#include <vector>

namespace seastar::xtrader {

struct strategy_context;
class subscription_registry;

enum class strategy_state {
    created,
    initialized,
    warmup,
    running,
    stopping,
    stopped,
    failed,
};

inline const char* to_string(strategy_state s) {
    switch (s) {
        case strategy_state::created:     return "created";
        case strategy_state::initialized: return "initialized";
        case strategy_state::warmup:      return "warmup";
        case strategy_state::running:     return "running";
        case strategy_state::stopping:    return "stopping";
        case strategy_state::stopped:     return "stopped";
        case strategy_state::failed:      return "failed";
        default: return "unknown";
    }
}

struct strategy_config {
    sstring strategy_id;
    sstring strategy_name;
    sstring strategy_type;
    std::vector<sstring> subscribed_instruments;
    int max_queue_depth = 4096;
};

struct strategy_stats {
    uint64_t signal_count = 0;
    uint64_t order_count = 0;
    uint64_t cancel_count = 0;
    uint64_t trade_count = 0;
    uint64_t reject_count = 0;
    double pnl = 0.0;
    double max_drawdown = 0.0;
    int queue_depth = 0;
};

class strategy_base {
public:
    explicit strategy_base(strategy_config config);
    virtual ~strategy_base();

    strategy_base(const strategy_base&) = delete;
    strategy_base& operator=(const strategy_base&) = delete;

    const sstring& strategy_id() const noexcept { return _config.strategy_id; }
    strategy_state state() const noexcept { return _state; }

    void set_state(strategy_state s) noexcept { _state = s; }
    const strategy_config& config() const noexcept { return _config; }
    strategy_stats& stats() noexcept { return _stats; }

    virtual future<> on_init(strategy_context& ctx) = 0;
    virtual future<> on_warmup(strategy_context& ctx) { return make_ready_future<>(); }
    virtual future<> on_market_data(strategy_context& ctx, const domain::market_data& md) = 0;
    virtual future<> on_order_update(strategy_context& ctx, const domain::order& order) { return make_ready_future<>(); }
    virtual future<> on_trade_update(strategy_context& ctx, const domain::trade_report& trade) { return make_ready_future<>(); }
    virtual future<> on_timer(strategy_context& ctx) { return make_ready_future<>(); }
    virtual future<> on_stop(strategy_context& ctx, const sstring& reason) { return make_ready_future<>(); }

protected:
    strategy_config _config;
    strategy_state _state = strategy_state::created;
    strategy_stats _stats;
};

} // namespace seastar::xtrader
