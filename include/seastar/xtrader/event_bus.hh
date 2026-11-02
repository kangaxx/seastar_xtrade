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
#include <seastar/core/timer.hh>
#include <seastar/xtrader/domain_types.hh>
#include <seastar/xtrader/subscription_registry.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>

#include <memory>
#include <queue>
#include <vector>
#include <variant>

namespace seastar::xtrader {

enum class market_event_type {
    tick,
    bar,
    depth,
};

struct market_event {
    market_event_type type = market_event_type::tick;
    sstring instrument_id;
    sstring trading_day;
    sstring timestamp;
    double last_price = 0.0;
    double open_price = 0.0;
    double high_price = 0.0;
    double low_price = 0.0;
    double close_price = 0.0;
    int volume = 0;
    double turnover = 0.0;
    double open_interest = 0.0;
    domain::market_data raw_md;

    uint64_t sequence = 0;
};

struct event_bus_config {
    size_t max_queue_depth = 65536;
    size_t batch_size = 256;
    size_t dispatch_budget_us = 500;
};

struct event_bus_metrics {
    uint64_t enqueued = 0;
    uint64_t dispatched = 0;
    uint64_t dropped = 0;
    size_t queue_depth = 0;
    double dispatch_latency_p50_us = 0.0;
    double dispatch_latency_p99_us = 0.0;
};

class event_bus {
public:
    explicit event_bus(event_bus_config cfg = {});
    ~event_bus();

    event_bus(const event_bus&) = delete;
    event_bus& operator=(const event_bus&) = delete;

    future<> start();
    future<> stop();

    bool enqueue(const market_event& event);

    void set_strategy_manager(strategy_lifecycle_manager* mgr) { _strategy_mgr = mgr; }
    void set_subscription_registry(subscription_registry* reg) { _sub_registry = reg; }

    const event_bus_metrics& metrics() const noexcept { return _metrics; }

private:
    void dispatch_batch();

    event_bus_config _cfg;
    strategy_lifecycle_manager* _strategy_mgr = nullptr;
    subscription_registry* _sub_registry = nullptr;

    std::queue<market_event> _queue;
    timer<> _dispatch_timer;
    event_bus_metrics _metrics;
    uint64_t _sequence_counter = 0;
    bool _started = false;
};

} // namespace seastar::xtrader
