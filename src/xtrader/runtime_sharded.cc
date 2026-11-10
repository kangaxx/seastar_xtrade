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
#include <seastar/xtrader/runtime_sharded.hh>
#include <seastar/core/smp.hh>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace seastar::xtrader {

runtime_sharded::runtime_sharded(runtime_sharded_config config) noexcept
    : _account_shard_id(config.account_shard)
    , _offline_initial_balance(config.offline_initial_balance)
{}

void runtime_sharded::set_gateway_shard(unsigned shard) {
    if (this_shard_id() != default_gateway_shard) {
        throw std::logic_error("runtime must be controlled from gateway shard 0");
    }
    if (_state != lifecycle_state::stopped) {
        throw std::logic_error("cannot change gateway shard while runtime is active");
    }
    _gateway_shard = shard;
}

// B1: reject invalid shard layouts and await every handler installation.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
future<> runtime_sharded::start() {
    if (this_shard_id() != default_gateway_shard) {
        return make_exception_future<>(std::runtime_error("runtime must be started on gateway shard 0"));
    }
    if (_state == lifecycle_state::running) {
        return make_ready_future<>();
    }
    if (_state != lifecycle_state::stopped) {
        return make_exception_future<>(std::runtime_error("runtime is not stopped"));
    }
    if (_account_shard_id != _gateway_shard ||
        _gateway_shard != default_gateway_shard ||
        _gateway_shard >= this_smp_shard_count()) {
        std::cerr << "[FATAL] runtime_sharded::start: "
                  << "account_shard=" << _account_shard_id
                  << ", gateway_shard=" << _gateway_shard
                  << ". The current CTP gateway and account must both use shard 0."
                  << std::endl;
        return make_exception_future<>(std::runtime_error(
            "account and gateway must use the available gateway shard 0"));
    }
    if (!_offline_initial_balance || !std::isfinite(*_offline_initial_balance) ||
        *_offline_initial_balance < 0.0) {
        return make_exception_future<>(std::runtime_error(
            "an explicit finite offline initial balance is required; live accounts are not supported"));
    }
    if (_requests.is_closed()) {
        _requests = gate{};
    }
    _state = lifecycle_state::starting;
    _order_handler_ready = false;
    return _engines.start().then([this] {
        return _engines.invoke_on_all(
            [account_shard = _account_shard_id,
             gateway_shard = _gateway_shard] (runtime_engine& engine) {
            engine.set_account_shard(account_shard);
            engine.set_gateway_shard(gateway_shard);
            return engine.start();
        });
    }).then([this] {
        return _engines.invoke_on(_account_shard_id, [balance = *_offline_initial_balance](runtime_engine& engine) {
            if (!engine.init_account_service(balance)) {
                throw std::runtime_error("account service initialization failed");
            }
        });
    }).then([this] {
        _state = lifecycle_state::running;
    }).handle_exception([this](std::exception_ptr ep) {
        if (!_engines.local_is_initialized()) {
            _state = lifecycle_state::stopped;
            return make_exception_future<>(ep);
        }
        return _engines.stop().then_wrapped([this, ep](future<> stopped) {
            if (stopped.failed()) {
                std::cerr << "[ERROR] runtime_sharded::start: cleanup failed after startup error"
                          << std::endl;
                stopped.ignore_ready_future();
                _state = lifecycle_state::failed;
            } else {
                stopped.get();
                _state = lifecycle_state::stopped;
            }
            return make_exception_future<>(ep);
        });
    });
}

future<> runtime_sharded::stop() {
    if (this_shard_id() != default_gateway_shard) {
        return make_exception_future<>(std::runtime_error("runtime must be stopped on gateway shard 0"));
    }
    if (_state == lifecycle_state::stopped) {
        return make_ready_future<>();
    }
    if (_state != lifecycle_state::running) {
        return make_exception_future<>(std::runtime_error("runtime cannot stop in its current state"));
    }
    _state = lifecycle_state::stopping;
    _order_handler_ready = false;
    return _requests.close().then([this] {
        return _engines.stop();
    }).then_wrapped([this](future<> stopped) {
        if (stopped.failed()) {
            _state = lifecycle_state::failed;
            return make_exception_future<>(stopped.get_exception());
        }
        stopped.get();
        _state = lifecycle_state::stopped;
        return make_ready_future<>();
    });
}

future<> runtime_sharded::set_order_handler(order_handler_t handler) {
    if (this_shard_id() != default_gateway_shard ||
        _state != lifecycle_state::running || !handler || _order_handler_installing) {
        return make_exception_future<>(std::runtime_error(
            "runtime is not running, order handler is empty, or installation is in progress"));
    }
    _order_handler_ready = false;
    _order_handler_installing = true;
    return try_with_gate(_requests, [this, handler = std::move(handler)] () mutable {
        return _engines.invoke_on(_gateway_shard, [handler = std::move(handler)] (runtime_engine& engine) mutable {
            engine.set_order_handler(std::move(handler));
        }).then_wrapped([this](future<> installed) {
            _order_handler_installing = false;
            if (installed.failed()) {
                return make_exception_future<>(installed.get_exception());
            }
            installed.get();
            if (_state == lifecycle_state::running) {
                _order_handler_ready = true;
            }
            return make_ready_future<>();
        });
    });
}

future<> runtime_sharded::set_trade_handler(trade_handler_t handler) {
    if (this_shard_id() != default_gateway_shard ||
        _state != lifecycle_state::running || !handler) {
        return make_exception_future<>(std::runtime_error("runtime is not running or trade handler is empty"));
    }
    return make_exception_future<>(std::runtime_error(
        "trade callbacks are unavailable until position and account settlement are integrated"));
}

future<domain::order_status> runtime_sharded::submit_order(const domain::order_request& request) {
    if (this_shard_id() != default_gateway_shard ||
        _state != lifecycle_state::running || !_order_handler_ready) {
        std::cerr << "[ERROR] runtime_sharded::submit_order: runtime or order handler is not ready"
                  << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }
    if (!request.is_valid() || !std::isfinite(request.price)) {
        std::cerr << "[ERROR] runtime_sharded::submit_order: invalid order request" << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }
    return try_with_gate(_requests, [this, request] {
        return _engines.invoke_on(_gateway_shard, [request] (runtime_engine& engine) {
            return engine.submit_order(request);
        });
    });
}

future<> runtime_sharded::apply_trade_report(const domain::trade_report& report) {
    if (this_shard_id() != default_gateway_shard || _state != lifecycle_state::running) {
        return make_exception_future<>(std::runtime_error("runtime is not running"));
    }
    if (report.trade_id.empty() || report.instrument_id.empty() || report.volume <= 0 ||
        !std::isfinite(report.price) || report.price <= 0.0 ||
        !std::isfinite(report.commission) || report.commission < 0.0) {
        return make_exception_future<>(std::runtime_error("invalid trade report"));
    }

    return make_exception_future<>(std::runtime_error(
        "trade settlement is unavailable until position and account accounting are integrated"));
}

future<std::optional<domain::position_view>> runtime_sharded::snapshot_positions() {
    if (this_shard_id() != default_gateway_shard) {
        return make_exception_future<std::optional<domain::position_view>>(
            std::runtime_error("runtime must be queried on gateway shard 0"));
    }
    if (_state != lifecycle_state::running) {
        return make_ready_future<std::optional<domain::position_view>>(std::nullopt);
    }
    return try_with_gate(_requests, [this] {
        return _engines.invoke_on(_account_shard_id, [] (runtime_engine& engine) {
            return make_ready_future<std::optional<domain::position_view>>(
                engine.positions().snapshot());
        });
    });
}

} // namespace seastar::xtrader
