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
#include <seastar/xtrader/event_bus.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/reactor.hh>

#include <chrono>
#include <iostream>

namespace seastar::xtrader {

event_bus::event_bus(event_bus_config cfg)
    : _cfg(std::move(cfg))
{}

event_bus::~event_bus() = default;

future<> event_bus::start() {
    if (_started) {
        return make_ready_future<>();
    }

    _dispatch_timer.set_callback([this] { dispatch_batch(); });
    _dispatch_timer.arm_periodic(
        std::chrono::microseconds(_cfg.dispatch_budget_us));

    _started = true;
    return make_ready_future<>();
}

future<> event_bus::stop() {
    _dispatch_timer.cancel();
    _started = false;
    return make_ready_future<>();
}

bool event_bus::enqueue(const market_event& event) {
    if (_queue.size() >= _cfg.max_queue_depth) {
        ++_metrics.dropped;
        std::cerr << "[WARN] event_bus: queue full, dropping event for "
                  << event.instrument_id << " seq=" << event.sequence << std::endl;
        return false;
    }

    market_event e = event;
    e.sequence = ++_sequence_counter;
    _queue.push(std::move(e));
    ++_metrics.enqueued;
    _metrics.queue_depth = _queue.size();
    return true;
}

void event_bus::dispatch_batch() {
    if (!_strategy_mgr || !_sub_registry) {
        return;
    }

    auto start = seastar::steady_clock_type::now();
    size_t dispatched = 0;

    while (dispatched < _cfg.batch_size && !_queue.empty()) {
        auto elapsed = seastar::steady_clock_type::now() - start;
        if (static_cast<size_t>(elapsed.count() / 1000) > _cfg.dispatch_budget_us) {
            break;
        }

        auto event = std::move(_queue.front());
        _queue.pop();

        auto event_k = (event.type == market_event_type::bar)
            ? event_kind::bar : event_kind::tick;

        auto subscribers = _sub_registry->find_subscribers(
            event.instrument_id, event_k);

        for (const auto& sub : subscribers) {
            auto strategy = _strategy_mgr->get_strategy(sub.strategy_id);
            if (!strategy || strategy->state() != strategy_state::running) {
                continue;
            }

            auto* ctx = _strategy_mgr->get_strategy_context(sub.strategy_id);
            if (!ctx) {
                continue;
            }

            futurize_invoke([strategy, ctx, md = event.raw_md] () mutable {
                return strategy->on_market_data(*ctx, md);
            }).then([strategy] {
                strategy->stats().signal_count++;
            }).handle_exception([strategy_id = sub.strategy_id,
                                 instrument_id = event.instrument_id](std::exception_ptr ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] event_bus: on_market_data FAILED, strategy="
                              << strategy_id << ", instrument="
                              << instrument_id << ", error=" << e.what()
                              << std::endl;
                } catch (...) {
                    std::cerr << "[ERROR] event_bus: on_market_data FAILED, strategy="
                              << strategy_id << ", instrument="
                              << instrument_id << ", unknown error"
                              << std::endl;
                }
                return make_ready_future<>();
            });

            strategy->stats().queue_depth++;
            ++dispatched;
        }

        ++_metrics.dispatched;
    }

    _metrics.queue_depth = _queue.size();
}

} // namespace seastar::xtrader
