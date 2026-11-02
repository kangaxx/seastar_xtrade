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
#include <seastar/core/sharded.hh>
#include <seastar/xtrader/runtime_engine.hh>

#include <functional>
#include <optional>

namespace seastar::xtrader {

class runtime_sharded {
public:
    static constexpr unsigned default_account_shard = 0;
    static constexpr unsigned default_gateway_shard = 0;

    using order_handler_t = runtime_engine::order_handler_t;
    using trade_handler_t = runtime_engine::trade_handler_t;

    explicit runtime_sharded(unsigned account_shard = default_account_shard) noexcept;
    void set_gateway_shard(unsigned shard) noexcept { _gateway_shard = shard; }

    future<> start();
    future<> stop();

    void set_order_handler(order_handler_t handler);
    void set_trade_handler(trade_handler_t handler);

    [[nodiscard]] future<domain::order_status> submit_order(const domain::order_request& request);
    future<> apply_trade_report(const domain::trade_report& report);
    [[nodiscard]] future<std::optional<domain::position_view>> snapshot_positions();

    [[nodiscard]] unsigned account_shard_id() const noexcept { return _account_shard_id; }
    [[nodiscard]] unsigned gateway_shard() const noexcept { return _gateway_shard; }

private:
    sharded<runtime_engine> _engines;
    unsigned _account_shard_id = default_account_shard;
    unsigned _gateway_shard = default_gateway_shard;
};

} // namespace seastar::xtrader
