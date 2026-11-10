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
#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sstring.hh>
#include <seastar/xtrader/domain_types.hh>
#include <seastar/xtrader/strategy_base.hh>

#include <functional>
#include <unordered_map>
#include <memory>
#include <stdexcept>

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
    std::function<future<>()> subscribe_bar_data;
    subscription_registry* sub_registry = nullptr;

    domain::instrument_spec spec;
};

class strategy_lifecycle_manager {
public:
    using submit_order_handler_t = std::function<future<domain::order_status>(const domain::order_request&)>;
    using cancel_order_handler_t = std::function<future<domain::order_status>(const sstring& order_sys_id)>;
    using position_query_handler_t = std::function<domain::position_view()>;
    using account_query_handler_t = std::function<const domain::trading_account*()>;
    using context_configurer_t = std::function<void(strategy_context&)>;

    strategy_lifecycle_manager() = default;
    ~strategy_lifecycle_manager() = default;

    strategy_lifecycle_manager(const strategy_lifecycle_manager&) = delete;
    strategy_lifecycle_manager& operator=(const strategy_lifecycle_manager&) = delete;

    future<> register_strategy(std::shared_ptr<strategy_base> strategy);
    future<> init_all();
    future<> start_all();
    future<> stop_all();
    future<> stop_strategy(const sstring& strategy_id);
    future<> dispatch_market_data(const sstring& strategy_id, domain::market_data data);
    future<> dispatch_bar(const sstring& strategy_id, domain::bar_data data);
    future<> dispatch_order_update(const sstring& strategy_id, domain::order order);
    future<> dispatch_trade_update(const sstring& strategy_id, domain::trade_report trade);

    void set_submit_order_handler(submit_order_handler_t handler) {
        require_configurable();
        _submit_order_handler = std::move(handler);
    }

    void set_cancel_order_handler(cancel_order_handler_t handler) {
        require_configurable();
        _cancel_order_handler = std::move(handler);
    }

    void set_position_query_handler(position_query_handler_t handler) {
        require_configurable();
        _position_query_handler = std::move(handler);
    }

    void set_account_query_handler(account_query_handler_t handler) {
        require_configurable();
        _account_query_handler = std::move(handler);
    }

    void set_context_configurer(context_configurer_t configurer) {
        require_configurable();
        _context_configurer = std::move(configurer);
    }

    void set_subscription_registry(subscription_registry* registry) {
        require_configurable();
        _subscription_registry = registry;
    }

    bool is_initialized() const noexcept { return _initialized; }
    size_t strategy_count() const noexcept { return _strategies.size(); }

    std::shared_ptr<strategy_base> get_strategy(const sstring& strategy_id);
    strategy_context* get_strategy_context(const sstring& strategy_id);
    const std::unordered_map<sstring, std::shared_ptr<strategy_base>>&
        all_strategies() const noexcept { return _strategies; }

private:
    using strategy_callback_t = std::function<future<>(strategy_base&, strategy_context&)>;

    // B1: keep strategy context and callback ownership stable across async work.
    // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
    struct strategy_session {
        explicit strategy_session(strategy_context value)
            : context(std::move(value)) {}

        strategy_context context;
        gate callbacks;
        semaphore callback_lock{1};
    };

    void require_configurable() const {
        if (_initialized || _pending_initializations != 0 || !_contexts.empty()) {
            throw std::logic_error("strategy manager configuration is frozen until shutdown");
        }
    }

    strategy_context make_default_context(const strategy_base& strategy);
    future<> dispatch_callback(const sstring& strategy_id, strategy_callback_t callback, bool is_signal = true);
    future<> stop_session(std::shared_ptr<strategy_base> strategy,
                          std::shared_ptr<strategy_session> session,
                          sstring reason, bool preserve_failure);

    std::unordered_map<sstring, std::shared_ptr<strategy_base>> _strategies;
    std::unordered_map<sstring, std::shared_ptr<strategy_session>> _contexts;
    semaphore _lifecycle_lock{1};
    submit_order_handler_t _submit_order_handler;
    cancel_order_handler_t _cancel_order_handler;
    position_query_handler_t _position_query_handler;
    account_query_handler_t _account_query_handler;
    context_configurer_t _context_configurer;
    subscription_registry* _subscription_registry = nullptr;
    bool _initialized = false;
    size_t _pending_initializations = 0;
};

} // namespace seastar::xtrader
