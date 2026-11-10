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
#include <seastar/xtrader/performance_metrics.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/reactor.hh>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace seastar::xtrader {

event_bus::event_bus(event_bus_config cfg)
    : _cfg(std::move(cfg))
{}

event_bus::~event_bus() = default;

future<> event_bus::start() {
    if (_started) {
        return make_ready_future<>();
    }
    if (_stopping || _dispatch_error || _callback_error) {
        return make_exception_future<>(std::runtime_error("event bus is stopping or has failed"));
    }
    if (!_strategy_mgr || !_sub_registry || !_cfg.max_queue_depth ||
        !_cfg.batch_size || !_cfg.dispatch_budget_us) {
        return make_exception_future<>(std::runtime_error("event bus dependencies or configuration are invalid"));
    }
    if (_dispatch_gate.is_closed()) {
        _dispatch_gate = gate{};
    }

    _stopped = false;
    _dispatch_timer.set_callback([this] { schedule_dispatch(); });
    _dispatch_timer.arm_periodic(
        std::chrono::microseconds(_cfg.dispatch_budget_us));

    _started = true;
    return make_ready_future<>();
}

future<> event_bus::stop() {
    if (_stopping) {
        return make_exception_future<>(std::runtime_error("event bus is already stopping"));
    }
    if (!_started) {
        if (!_queue.empty()) {
            return make_exception_future<>(std::runtime_error("cannot stop an unstarted event bus with pending events"));
        }
        _stopped = true;
        if (_dispatch_error) {
            return make_exception_future<>(_dispatch_error);
        }
        return _callback_error ? make_exception_future<>(_callback_error) : make_ready_future<>();
    }

    _dispatch_timer.cancel();
    _started = false;
    _stopping = true;
    _dispatch_done.broadcast();
    schedule_dispatch();
    return _dispatch_gate.close().then([this] {
        _stopping = false;
        _stopped = true;
        if (_dispatch_error) {
            return make_exception_future<>(_dispatch_error);
        }
        if (!_queue.empty()) {
            return make_exception_future<>(std::runtime_error("event bus stopped with pending events"));
        }
        return _callback_error ? make_exception_future<>(_callback_error) : make_ready_future<>();
    });
}

// B3: wait for accepted work and surface strategy callback failures to replay.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
future<> event_bus::drain() {
    if (!_started || _stopping) {
        return make_exception_future<>(std::runtime_error("event bus is not running"));
    }
    return with_gate(_dispatch_gate, [this] {
        schedule_dispatch();
        return _dispatch_done.wait([this] {
            return !_started || _stopping || _dispatch_error || _callback_error ||
                (!_dispatching && _queue.empty());
        }).then([this] {
            if (_dispatch_error) {
                return make_exception_future<>(_dispatch_error);
            }
            if (_callback_error) {
                return make_exception_future<>(_callback_error);
            }
            if (!_started || _stopping) {
                return make_exception_future<>(std::runtime_error("event bus stopped while draining"));
            }
            return make_ready_future<>();
        });
    });
}

bool event_bus::enqueue(const market_event& event) {
    // Phase 6 transport observations; no implication of live trading acceptance.
    // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
    const bool unsupported = event.type != market_event_type::tick && event.type != market_event_type::bar;
    if (unsupported ||
        _stopping || _stopped || _dispatch_error || _callback_error ||
        _queue.size() >= _cfg.max_queue_depth) {
        ++_metrics.dropped;
        performance_metrics::instance().record_dropped("dispatch");
        std::cerr << "[WARN] event_bus: "
                  << (unsupported ? "unsupported event type" : "unavailable or queue full")
                  << ", dropping event for "
                  << event.instrument_id << " seq=" << event.sequence << std::endl;
        return false;
    }

    market_event e = event;
    e.sequence = ++_sequence_counter;
    e.queued_at = performance_metrics::clock::now();
    _queue.push(std::move(e));
    ++_metrics.enqueued;
    _metrics.queue_depth = _queue.size();
    performance_metrics::instance().record_queue_depth("dispatch", _queue.size());
    return true;
}

// B1: serialize delivery and drain accepted events before shutdown completes.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
void event_bus::schedule_dispatch() {
    if (_dispatching || _queue.empty() || _dispatch_error) {
        return;
    }
    _dispatching = true;
    (void)with_gate(_dispatch_gate, [this] {
        return dispatch_batch().then_wrapped([this](future<> result) {
            _dispatching = false;
            if (result.failed()) {
                _dispatch_error = result.get_exception();
                performance_metrics::instance().record_error("dispatch");
                try {
                    std::rethrow_exception(_dispatch_error);
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] event_bus: dispatch failed: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[ERROR] event_bus: dispatch failed with unknown error" << std::endl;
                }
            } else {
                result.get();
            }
            _dispatch_done.broadcast();
        });
    });
}

future<> event_bus::dispatch_batch() {
    auto start = seastar::steady_clock_type::now();
    size_t dispatched = 0;

    while (!_queue.empty()) {
        auto elapsed = seastar::steady_clock_type::now() - start;
        if (!_stopping && dispatched > 0 && (dispatched >= _cfg.batch_size ||
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >=
                static_cast<long>(_cfg.dispatch_budget_us))) {
            break;
        }
        if (_stopping && dispatched >= _cfg.batch_size) {
            dispatched = 0;
            start = seastar::steady_clock_type::now();
            co_await seastar::yield();
        }

        auto event = std::move(_queue.front());
        _queue.pop();
        _metrics.queue_depth = _queue.size();
        performance_metrics::instance().record_queue_depth("dispatch", _queue.size());
        performance_metrics::instance().record_dispatch_latency_us(
            std::chrono::duration_cast<std::chrono::microseconds>(
                performance_metrics::clock::now() - event.queued_at).count());

        auto event_k = (event.type == market_event_type::bar)
            ? event_kind::bar : event_kind::tick;

        auto subscribers = _sub_registry->find_subscribers(
            event.instrument_id, event_k);
        if (event.require_subscriber && subscribers.empty()) {
            throw std::runtime_error("replay event has no subscriber: " + event.instrument_id);
        }

        for (const auto& sub : subscribers) {
            auto strategy = _strategy_mgr->get_strategy(sub.strategy_id);
            if (!strategy || strategy->state() != strategy_state::running) {
                if (event.require_subscriber) {
                    throw std::runtime_error("replay subscriber is not running: " + sub.strategy_id);
                }
                continue;
            }

            try {
                if (event.type == market_event_type::bar) {
                    co_await _strategy_mgr->dispatch_bar(sub.strategy_id, domain::bar_data{
                        .instrument_id = event.instrument_id,
                        .exchange_id = event.exchange_id,
                        .calendar_date = event.calendar_date,
                        .timestamp = event.timestamp,
                        .timeframe = event.timeframe,
                        .open_price = event.open_price,
                        .high_price = event.high_price,
                        .low_price = event.low_price,
                        .close_price = event.close_price,
                        .volume = event.volume,
                        .open_interest = event.open_interest,
                    });
                } else {
                    co_await _strategy_mgr->dispatch_market_data(sub.strategy_id, event.raw_md);
                }
                performance_metrics::instance().record_success("dispatch");
            } catch (const std::exception& e) {
                if (!_callback_error) {
                    _callback_error = std::current_exception();
                }
                performance_metrics::instance().record_error("dispatch");
                std::cerr << "[ERROR] event_bus: market callback FAILED, strategy="
                          << sub.strategy_id << ", instrument=" << event.instrument_id
                          << ", error=" << e.what() << std::endl;
            } catch (...) {
                if (!_callback_error) {
                    _callback_error = std::current_exception();
                }
                performance_metrics::instance().record_error("dispatch");
                std::cerr << "[ERROR] event_bus: market callback FAILED, strategy="
                          << sub.strategy_id << ", instrument=" << event.instrument_id
                          << ", unknown error" << std::endl;
            }
        }

        ++dispatched;
        ++_metrics.dispatched;
    }

    _metrics.queue_depth = _queue.size();
    auto& metrics = performance_metrics::instance();
    metrics.record_queue_depth("dispatch", _queue.size());
    const auto snapshot = metrics.snapshot_dispatch();
    _metrics.dispatch_latency_p50_us = snapshot.p50_latency_us;
    _metrics.dispatch_latency_p99_us = snapshot.p99_latency_us;
    co_return;
}

} // namespace seastar::xtrader
