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
#include <seastar/xtrader/strategy_base.hh>

#include <functional>
#include <unordered_map>
#include <memory>

namespace seastar::xtrader {

class subscription_registry;

struct strategy_context {
    sstring strategy_id;
    sstring trading_day;

    std::function<future<domain::order_status>(const domain::order_request&)> submit_order;
    std::function<future<domain::order_status>(const sstring& order_sys_id)> cancel_order;
    std::function<domain::position_view()> query_position;
    std::function<const domain::trading_account*()> query_account;
    std::function<future<>()> subscribe_market_data;
    subscription_registry* sub_registry = nullptr;

    domain::instrument_spec spec;
};

class strategy_lifecycle_manager {
public:
    using submit_order_handler_t = std::function<future<domain::order_status>(const domain::order_request&)>;
    using cancel_order_handler_t = std::function<future<domain::order_status>(const sstring& order_sys_id)>;
    using position_query_handler_t = std::function<domain::position_view()>;
    using account_query_handler_t = std::function<const domain::trading_account*()>;

    strategy_lifecycle_manager() = default;
    ~strategy_lifecycle_manager() = default;

    strategy_lifecycle_manager(const strategy_lifecycle_manager&) = delete;
    strategy_lifecycle_manager& operator=(const strategy_lifecycle_manager&) = delete;

    future<> register_strategy(std::shared_ptr<strategy_base> strategy);
    future<> init_all();
    future<> start_all();
    future<> stop_all();
    future<> stop_strategy(const sstring& strategy_id);

    void set_submit_order_handler(submit_order_handler_t handler) {
        _submit_order_handler = std::move(handler);
    }

    void set_cancel_order_handler(cancel_order_handler_t handler) {
        _cancel_order_handler = std::move(handler);
    }

    void set_position_query_handler(position_query_handler_t handler) {
        _position_query_handler = std::move(handler);
    }

    void set_account_query_handler(account_query_handler_t handler) {
        _account_query_handler = std::move(handler);
    }

    void set_subscription_registry(subscription_registry* registry) noexcept {
        _subscription_registry = registry;
    }

    bool is_initialized() const noexcept { return _initialized; }
    size_t strategy_count() const noexcept { return _strategies.size(); }

    std::shared_ptr<strategy_base> get_strategy(const sstring& strategy_id);
    strategy_context* get_strategy_context(const sstring& strategy_id);
    const std::unordered_map<sstring, std::shared_ptr<strategy_base>>&
        all_strategies() const noexcept { return _strategies; }

private:
    strategy_context make_default_context(const strategy_base& strategy);

    std::unordered_map<sstring, std::shared_ptr<strategy_base>> _strategies;
    std::unordered_map<sstring, strategy_context> _contexts;
    submit_order_handler_t _submit_order_handler;
    cancel_order_handler_t _cancel_order_handler;
    position_query_handler_t _position_query_handler;
    account_query_handler_t _account_query_handler;
    subscription_registry* _subscription_registry = nullptr;
    bool _initialized = false;
};

} // namespace seastar::xtrader
