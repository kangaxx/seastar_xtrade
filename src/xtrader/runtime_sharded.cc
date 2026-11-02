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

#include <stdexcept>
#include <memory>

namespace seastar::xtrader {

runtime_sharded::runtime_sharded(unsigned account_shard) noexcept
    : _account_shard_id(account_shard)
{}

future<> runtime_sharded::start() {
    if (_engines.local_is_initialized()) {
        return make_ready_future<>();
    }

    if (_account_shard_id != _gateway_shard) {
        std::cerr << "[FATAL] runtime_sharded::start: "
                  << "account_shard(" << _account_shard_id
                  << ") != gateway_shard(" << _gateway_shard
                  << "). Phase 3 requires same-shard configuration. "
                  << "Set both to a single shard (e.g. 0)." << std::endl;
        return make_exception_future<>(std::runtime_error(
            "account_shard must equal gateway_shard in Phase 3"));
    }

    auto failures = std::make_shared<unsigned>(0);

    return _engines.start().then([this, failures] {
        return _engines.invoke_on_all(
            [account_shard = _account_shard_id,
             gateway_shard = _gateway_shard,
             failures] (runtime_engine& engine) {
            engine.set_account_shard(account_shard);
            engine.set_gateway_shard(gateway_shard);

            if (this_shard_id() == account_shard) {
                if (!engine.init_account_service(1000000.0)) {
                    std::cerr << "[FATAL] runtime_sharded::start: "
                              << "account service initialization failed on shard "
                              << this_shard_id()
                              << ". Cannot start - will cause position/account mismatch!"
                              << std::endl;
                    ++(*failures);
                }
            }

            return engine.start();
        }).then([failures] {
            if (*failures > 0) {
                std::cerr << "[FATAL] runtime_sharded::start: "
                          << *failures
                          << " shard(s) failed to initialize. Aborting start!"
                          << std::endl;
                return make_exception_future<>(
                    std::runtime_error("Account service initialization failed"));
            }
            return make_ready_future<>();
        });
    });
}

future<> runtime_sharded::stop() {
    if (!_engines.local_is_initialized()) {
        return make_ready_future<>();
    }

    return _engines.stop();
}

void runtime_sharded::set_order_handler(order_handler_t handler) {
    (void)_engines.invoke_on_all(
        [handler = std::move(handler)] (runtime_engine& engine) mutable {
        engine.set_order_handler(std::move(handler));
    });
}

void runtime_sharded::set_trade_handler(trade_handler_t handler) {
    (void)_engines.invoke_on_all(
        [handler = std::move(handler)] (runtime_engine& engine) mutable {
        engine.set_trade_handler(std::move(handler));
    });
}

future<domain::order_status> runtime_sharded::submit_order(const domain::order_request& request) {
    if (!_engines.local_is_initialized()) {
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    return _engines.invoke_on(_gateway_shard, [request] (runtime_engine& engine) {
        return engine.submit_order(request);
    });
}

future<> runtime_sharded::apply_trade_report(const domain::trade_report& report) {
    if (!_engines.local_is_initialized()) {
        return make_ready_future<>();
    }

    auto account_shard = _account_shard_id;

    return _engines.invoke_on(account_shard, [report] (runtime_engine& engine) {
        engine.apply_trade_to_position(report);

        domain::account_delta delta;
        delta.instrument_id = report.instrument_id;
        delta.direction = report.direction;
        delta.offset = report.offset;
        delta.traded_volume = report.volume;
        delta.price = report.price;
        delta.commission = report.commission;

        return engine.submit_delta_local(delta).then([](bool ok) {
            if (!ok) {
                std::cerr << "[ERROR] runtime_sharded::apply_trade_report: "
                          << "submit_delta_local failed" << std::endl;
            }
            return make_ready_future<>();
        });
    });
}

future<std::optional<domain::position_view>> runtime_sharded::snapshot_positions() {
    if (!_engines.local_is_initialized()) {
        return make_ready_future<std::optional<domain::position_view>>(std::nullopt);
    }

    return _engines.invoke_on(_gateway_shard, [] (runtime_engine& engine) {
        return make_ready_future<std::optional<domain::position_view>>(
            engine.positions().snapshot());
    });
}

} // namespace seastar::xtrader
