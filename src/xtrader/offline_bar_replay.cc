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
#include <seastar/xtrader/offline_bar_replay.hh>

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B3: a thrown strategy callback aborts the run and disables further orders.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
template <typename T>
T require_setting(const std::optional<T>& setting, const char* message) {
    if (!setting) {
        throw std::invalid_argument(message);
    }
    return *setting;
}

class callback_scope {
public:
    explicit callback_scope(bool& active) : _active(active) {
        if (_active) {
            throw std::logic_error("offline replay callbacks cannot be reentered");
        }
        _active = true;
    }
    ~callback_scope() { _active = false; }

private:
    bool& _active;
};

class replay_failure_guard {
public:
    explicit replay_failure_guard(offline_bar_replay_state& state) : _state(state) {}
    ~replay_failure_guard() {
        if (_state == offline_bar_replay_state::running) {
            _state = offline_bar_replay_state::failed;
        }
    }

private:
    offline_bar_replay_state& _state;
};

} // namespace

std::shared_ptr<offline_bar_replay> offline_bar_replay::create(
        offline_bar_replay_config config, const std::vector<offline_bar_record>& records) {
    return std::shared_ptr<offline_bar_replay>(new offline_bar_replay(std::move(config), records));
}

offline_bar_replay::offline_bar_replay(offline_bar_replay_config config,
                                       const std::vector<offline_bar_record>& records)
    : _strategy_id(config.strategy_id), _trading_day(config.account.trading_day),
      _execution(config.account, config.money_unit, config.strategy_id, std::move(config.contracts),
                 require_setting(config.cost_allocation, "an explicit cost allocation is required"),
                 config.run_id, simulated_market_mode::next_bar_open, config.bar_liquidity),
      _events(build_offline_bar_timeline(records)), _context(*this),
      _price_policy(require_setting(config.price_normalization, "an explicit price normalization is required")) {
    _execution.initialize_flat(config.initial_balance);
    prepare_events(_price_policy);
}

void offline_bar_replay::prepare_events(offline_price_normalization policy) {
    for (auto& event : _events) {
        if (event.trading_day != _trading_day) {
            throw std::invalid_argument("offline replay bars must belong to the account trading day");
        }
        auto normalize = [this, &event, policy] (double& price, bool count_adjustment) {
            const auto result = _execution.normalize_price(event.exchange_id, event.instrument_id, price, policy);
            price = result.price;
            if (count_adjustment && result.adjusted) {
                ++_stats.normalized_price_fields;
            }
        };
        if (auto* open = std::get_if<offline_bar_open_data>(&event.data)) {
            normalize(open->open_price, false);
        } else {
            auto& bar = std::get<offline_bar_completed_data>(event.data);
            normalize(bar.open_price, true);
            normalize(bar.high_price, true);
            normalize(bar.low_price, true);
            normalize(bar.close_price, true);
            if (bar.low_price > bar.open_price || bar.high_price < bar.open_price
                || bar.low_price > bar.close_price || bar.high_price < bar.close_price) {
                throw std::invalid_argument("normalized offline bar prices are inconsistent");
            }
        }
    }
}

future<> offline_bar_replay::run(offline_bar_replay_callbacks callbacks) {
    auto keep_alive = shared_from_this();
    if (_state != offline_bar_replay_state::ready) {
        throw std::logic_error("offline bar replay can run only once");
    }
    if (!callbacks.on_bar_open || !callbacks.on_bar_completed) {
        throw std::invalid_argument("bar-open and completed-bar callbacks are required");
    }
    _state = offline_bar_replay_state::running;
    _accept_new_orders = true;
    replay_failure_guard failure_guard(_state);
    if (callbacks.on_start) {
        {
            callback_scope scope(_callback_active);
            co_await callbacks.on_start(_context);
        }
        if (!_execution.is_consistent()) {
            throw std::runtime_error("offline account became inconsistent during callback");
        }
    }
    fail_if_stopped();
    for (const auto& event : _events) {
        fail_if_stopped();
        if (std::holds_alternative<offline_bar_open_data>(event.data)) {
            co_await process_open(event, callbacks);
        } else {
            co_await process_completed(event, callbacks);
        }
        fail_if_stopped();
        co_await coroutine::maybe_yield();
    }
    fail_if_stopped();
    _accept_new_orders = false;
    if (callbacks.on_finish) {
        {
            callback_scope scope(_callback_active);
            co_await callbacks.on_finish(_context);
        }
        if (!_execution.is_consistent()) {
            throw std::runtime_error("offline account became inconsistent during callback");
        }
        fail_if_stopped();
    }
    _state = offline_bar_replay_state::completed;
    co_return;
}

void offline_bar_replay::request_stop() noexcept {
    if (_state == offline_bar_replay_state::ready) {
        _state = offline_bar_replay_state::stopped;
    } else if (_state == offline_bar_replay_state::running) {
        _stop_requested = true;
    }
}

void offline_bar_replay::fail_if_stopped() {
    if (_stop_requested) {
        _state = offline_bar_replay_state::stopped;
        throw std::runtime_error("offline bar replay stopped before completion");
    }
}

future<> offline_bar_replay::process_open(const offline_bar_timeline_event& event,
                                         const offline_bar_replay_callbacks& callbacks) {
    const auto price = std::get<offline_bar_open_data>(event.data).open_price;
    const auto normalized = _execution.normalize_price(event.exchange_id, event.instrument_id, price,
                                                        offline_price_normalization::exact);
    auto result = _execution.on_bar_open({
        .sequence = event.sequence,
        .exchange_id = event.exchange_id,
        .instrument_id = event.instrument_id,
        .open_ticks = normalized.price_ticks,
    });
    ++_stats.open_events;
    _stats.fills += result.trades.size();
    const offline_bar_open_notice notice{
        .sequence = event.sequence,
        .timestamp_nanos = event.timestamp_nanos,
        .exchange_id = event.exchange_id,
        .instrument_id = event.instrument_id,
        .timeframe = event.timeframe,
        .trading_day = event.trading_day,
        .open_price = price,
    };
    {
        callback_scope scope(_callback_active);
        co_await callbacks.on_bar_open(_context, notice, result);
    }
    if (!_execution.is_consistent()) {
        throw std::runtime_error("offline account became inconsistent during callback");
    }
    co_return;
}

future<> offline_bar_replay::process_completed(const offline_bar_timeline_event& event,
                                               const offline_bar_replay_callbacks& callbacks) {
    const offline_completed_bar_notice notice{
        .sequence = event.sequence,
        .timestamp_nanos = event.timestamp_nanos,
        .exchange_id = event.exchange_id,
        .instrument_id = event.instrument_id,
        .timeframe = event.timeframe,
        .trading_day = event.trading_day,
        .bar = std::get<offline_bar_completed_data>(event.data),
    };
    {
        callback_scope scope(_callback_active);
        co_await callbacks.on_bar_completed(_context, notice);
    }
    if (!_execution.is_consistent()) {
        throw std::runtime_error("offline account became inconsistent during callback");
    }
    ++_stats.completed_bars;
    co_return;
}

void offline_bar_replay::require_callback() const {
    if (_state != offline_bar_replay_state::running || !_callback_active) {
        throw std::logic_error("offline replay orders require an active callback");
    }
}

void offline_bar_replay::ensure_order_capacity() {
    if (_order_ids.size() == _order_ids.capacity()) {
        if (_order_ids.size() == _order_ids.max_size()) {
            throw std::length_error("too many offline replay orders");
        }
        const auto capacity = _order_ids.capacity();
        const auto growth = std::max<size_t>(capacity, 1);
        _order_ids.reserve(capacity > _order_ids.max_size() - growth ? _order_ids.max_size() : capacity + growth);
    }
}

size_t offline_bar_replay::active_order_count() const {
    return std::count_if(_order_ids.begin(), _order_ids.end(), [this] (const simulated_order_id& id) {
        return _execution.get_order(id, _strategy_id).is_active();
    });
}

offline_account_snapshot offline_bar_replay::snapshot(const position_instrument& instrument) const {
    return _execution.snapshot(instrument);
}

simulated_order offline_bar_replay_context::submit(const offline_simulated_order_request& request) {
    _replay.require_callback();
    if (!_replay._accept_new_orders || _replay._stop_requested) {
        throw std::logic_error("offline replay is stopping and cannot accept orders");
    }
    _replay.ensure_order_capacity();
    auto order = _replay._execution.submit(request);
    _replay._order_ids.push_back(order.id);
    ++_replay._stats.submitted_orders;
    return order;
}

simulated_order offline_bar_replay_context::cancel(const simulated_order_id& id) {
    _replay.require_callback();
    auto order = _replay._execution.cancel(id, _replay._strategy_id);
    ++_replay._stats.canceled_orders;
    return order;
}

simulated_order offline_bar_replay_context::get_order(const simulated_order_id& id) const {
    _replay.require_callback();
    return _replay._execution.get_order(id, _replay._strategy_id);
}

offline_account_snapshot offline_bar_replay_context::snapshot(const position_instrument& instrument) const {
    _replay.require_callback();
    return _replay._execution.snapshot(instrument);
}

offline_normalized_price offline_bar_replay_context::normalize_price(
        const position_instrument& instrument, double price) const {
    _replay.require_callback();
    return _replay._execution.normalize_price(instrument.exchange_id, instrument.instrument_id,
                                             price, _replay._price_policy);
}

} // namespace seastar::xtrader
