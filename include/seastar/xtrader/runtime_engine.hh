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
#include <seastar/core/smp.hh>
#include <seastar/xtrader/domain_types.hh>

#include <functional>
#include <memory>

namespace seastar::xtrader {

class trading_account_service;

class runtime_engine {
public:
    using order_handler_t = std::function<future<domain::order_status>(const domain::order_request&)>;
    using trade_handler_t = std::function<void(const domain::trade_report&)>;

    runtime_engine() = default;
    ~runtime_engine();

    runtime_engine(runtime_engine&&) noexcept = default;
    runtime_engine& operator=(runtime_engine&&) noexcept = default;

    runtime_engine(const runtime_engine&) = delete;
    runtime_engine& operator=(const runtime_engine&) = delete;

    explicit runtime_engine(unsigned account_shard);

    future<> start();
    future<> stop();

    [[nodiscard]] future<domain::order_status> submit_order(const domain::order_request& request);

    void apply_trade_to_position(const domain::trade_report& report);
    future<bool> submit_delta_local(const domain::account_delta& delta);

    [[nodiscard]] const domain::position_book& positions() const noexcept;

    trading_account_service* local_account_service() noexcept { return _account_service.get(); }
    void set_account_service(std::unique_ptr<trading_account_service> service);

    bool is_gateway_shard() const noexcept { return _is_gateway_shard; }

    void set_account_shard(unsigned shard_id) noexcept { _account_shard = shard_id; }
    unsigned account_shard() const noexcept { return _account_shard; }

    void set_gateway_shard(unsigned shard_id) noexcept { _gateway_shard = shard_id; }
    unsigned gateway_shard() const noexcept { return _gateway_shard; }

    bool init_account_service(double pre_balance = 1000000.0);

    bool is_account_service_ready() const noexcept {
        return _account_shard == this_shard_id() && _account_service != nullptr;
    }

    void set_order_handler(order_handler_t handler) { _order_handler = std::move(handler); }
    void set_trade_handler(trade_handler_t handler) { _trade_handler = std::move(handler); }

private:
    domain::position_book _positions;
    std::unique_ptr<trading_account_service> _account_service;
    unsigned _account_shard = 1;
    unsigned _gateway_shard = 0;
    bool _is_gateway_shard = false;
    bool _started = false;

    order_handler_t _order_handler;
    trade_handler_t _trade_handler;
};

} // namespace seastar::xtrader
