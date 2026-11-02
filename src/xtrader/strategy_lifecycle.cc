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
#include <seastar/xtrader/strategy_lifecycle.hh>

#include <seastar/core/future-util.hh>
#include <seastar/xtrader/subscription_registry.hh>

#include <iostream>

namespace seastar::xtrader {

strategy_context strategy_lifecycle_manager::make_default_context(
    const strategy_base& strategy) {
    strategy_context ctx;
    const auto& strategy_id = strategy.strategy_id();
    ctx.strategy_id = strategy_id;
    ctx.sub_registry = _subscription_registry;

    if (_submit_order_handler) {
        ctx.submit_order = [handler = _submit_order_handler](const domain::order_request& req) {
            return handler(req);
        };
    } else {
        ctx.submit_order = [strategy_id](const domain::order_request& req) {
            std::cerr << "[WARN] strategy_context[" << strategy_id
                      << "]: submit_order called but not wired to runtime"
                      << std::endl;
            return make_ready_future<domain::order_status>(
                domain::order_status::rejected);
        };
    }

    if (_cancel_order_handler) {
        ctx.cancel_order = [handler = _cancel_order_handler](const sstring& order_sys_id) {
            return handler(order_sys_id);
        };
    } else {
        ctx.cancel_order = [strategy_id](const sstring& sys_id) {
            std::cerr << "[WARN] strategy_context[" << strategy_id
                      << "]: cancel_order called but not wired to runtime"
                      << std::endl;
            return make_ready_future<domain::order_status>(
                domain::order_status::rejected);
        };
    }

    ctx.query_position = _position_query_handler
        ? _position_query_handler
        : [] {
            return domain::position_view{};
        };

    ctx.query_account = _account_query_handler
        ? _account_query_handler
        : [] {
            return nullptr;
        };

    const auto subscribed = strategy.config().subscribed_instruments;
    if (!subscribed.empty()) {
        ctx.spec.instrument_id = subscribed.front();
    }

    ctx.subscribe_market_data = [this, strategy_id, subscribed] {
        if (_subscription_registry == nullptr || subscribed.empty()) {
            return make_ready_future<>();
        }

        future<> chain = make_ready_future<>();
        for (const auto& instrument_id : subscribed) {
            chain = std::move(chain).then([this, strategy_id, instrument_id] {
                return _subscription_registry->subscribe(
                    strategy_id,
                    instrument_id,
                    event_kind::tick);
            });
        }
        return chain;
    };

    return ctx;
}

future<> strategy_lifecycle_manager::register_strategy(
    std::shared_ptr<strategy_base> strategy) {
    if (!strategy) {
        return make_exception_future<>(std::runtime_error("null strategy"));
    }

    const auto& id = strategy->strategy_id();
    if (_strategies.find(id) != _strategies.end()) {
        return make_exception_future<>(
            std::runtime_error("duplicate strategy_id: " + id));
    }

    _strategies[id] = std::move(strategy);
    return make_ready_future<>();
}

future<> strategy_lifecycle_manager::init_all() {
    if (_initialized) {
        return make_ready_future<>();
    }

    return do_for_each(_strategies, [this](auto& item) {
        const auto id = item.first;
        auto strategy = item.second;

        if (strategy->state() == strategy_state::failed) {
            return make_ready_future<>();
        }

        auto ctx = make_default_context(*strategy);
        return futurize_invoke([strategy, &ctx] {
            return strategy->on_init(ctx);
        }).then([this, strategy, id, ctx = std::move(ctx)] () mutable {
            strategy->set_state(strategy_state::initialized);
            _contexts[id] = std::move(ctx);
            std::cout << "[INFO] strategy_lifecycle_manager: on_init OK, strategy="
                      << id << std::endl;
        }).handle_exception([strategy, id](std::exception_ptr ep) {
            strategy->set_state(strategy_state::failed);
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_init FAILED, strategy="
                          << id << ", error=" << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_init FAILED, strategy="
                          << id << ", unknown error" << std::endl;
            }
            return make_ready_future<>();
        });
    }).then([this] {
        _initialized = true;
    });
}

future<> strategy_lifecycle_manager::start_all() {
    return do_for_each(_strategies, [this](auto& item) {
        const auto id = item.first;
        auto strategy = item.second;

        if (strategy->state() != strategy_state::initialized) {
            return make_ready_future<>();
        }

        auto it_ctx = _contexts.find(id);
        if (it_ctx == _contexts.end()) {
            strategy->set_state(strategy_state::failed);
            std::cerr << "[ERROR] strategy_lifecycle_manager: no context for strategy="
                      << id << std::endl;
            return make_ready_future<>();
        }

        strategy->set_state(strategy_state::warmup);
        return futurize_invoke([strategy, &ctx = it_ctx->second] {
            return strategy->on_warmup(ctx);
        }).then([strategy, id] {
            strategy->set_state(strategy_state::running);
            std::cout << "[INFO] strategy_lifecycle_manager: started strategy="
                      << id << std::endl;
        }).handle_exception([strategy, id](std::exception_ptr ep) {
            strategy->set_state(strategy_state::failed);
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_warmup FAILED, strategy="
                          << id << ", error=" << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_warmup FAILED, strategy="
                          << id << ", unknown error" << std::endl;
            }
            return make_ready_future<>();
        });
    });
}

future<> strategy_lifecycle_manager::stop_all() {
    const sstring reason = "shutdown";

    return do_for_each(_strategies, [this, reason](auto& item) {
        const auto id = item.first;
        auto strategy = item.second;

        if (strategy->state() != strategy_state::running) {
            return make_ready_future<>();
        }
        strategy->set_state(strategy_state::stopping);

        auto it_ctx = _contexts.find(id);
        if (it_ctx == _contexts.end()) {
            strategy->set_state(strategy_state::stopped);
            return make_ready_future<>();
        }

        return futurize_invoke([strategy, &ctx = it_ctx->second, reason] {
            return strategy->on_stop(ctx, reason);
        }).handle_exception([id](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_stop FAILED, strategy="
                          << id << ", error=" << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ERROR] strategy_lifecycle_manager: on_stop FAILED, strategy="
                          << id << ", unknown error" << std::endl;
            }
            return make_ready_future<>();
        }).then([strategy, id] {
            strategy->set_state(strategy_state::stopped);
            std::cout << "[INFO] strategy_lifecycle_manager: stopped strategy="
                      << id << std::endl;
        });
    }).then([this] {
        _contexts.clear();
        _initialized = false;
    });
}

future<> strategy_lifecycle_manager::stop_strategy(const sstring& strategy_id) {
    auto it = _strategies.find(strategy_id);
    if (it == _strategies.end()) {
        return make_exception_future<>(
            std::runtime_error("strategy not found: " + strategy_id));
    }

    auto& strategy = it->second;
    strategy->set_state(strategy_state::stopping);

    auto it_ctx = _contexts.find(strategy_id);
    if (it_ctx == _contexts.end()) {
        strategy->set_state(strategy_state::stopped);
        std::cout << "[INFO] strategy_lifecycle_manager: stopped strategy="
                  << strategy_id << std::endl;
        return make_ready_future<>();
    }

    return futurize_invoke([strategy, &ctx = it_ctx->second] {
        return strategy->on_stop(ctx, "manual_stop");
    }).handle_exception([](std::exception_ptr) {
        return make_ready_future<>();
    }).then([this, strategy, strategy_id] {
        strategy->set_state(strategy_state::stopped);
        _contexts.erase(strategy_id);
        std::cout << "[INFO] strategy_lifecycle_manager: stopped strategy="
                  << strategy_id << std::endl;
    });
}

std::shared_ptr<strategy_base> strategy_lifecycle_manager::get_strategy(
    const sstring& strategy_id) {
    auto it = _strategies.find(strategy_id);
    if (it != _strategies.end()) {
        return it->second;
    }
    return nullptr;
}

strategy_context* strategy_lifecycle_manager::get_strategy_context(
    const sstring& strategy_id) {
    auto it = _contexts.find(strategy_id);
    if (it != _contexts.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace seastar::xtrader
