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
#include <stdexcept>

namespace seastar::xtrader {

namespace {

void log_strategy_failure(const char* operation, const sstring& id, std::exception_ptr ep) {
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] strategy_lifecycle_manager: " << operation
                  << " FAILED, strategy=" << id << ", error=" << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[ERROR] strategy_lifecycle_manager: " << operation
                  << " FAILED, strategy=" << id << ", unknown error" << std::endl;
    }
}

future<> clear_strategy_subscriptions(const strategy_base& strategy, subscription_registry* registry) {
    if (!registry) {
        return make_ready_future<>();
    }
    return do_for_each(strategy.config().subscribed_instruments,
        [registry, id = sstring(strategy.strategy_id())](const sstring& instrument) {
            return registry->unsubscribe(id, instrument, event_kind::tick).then(
                [registry, id, instrument = sstring(instrument)] {
                    return registry->unsubscribe(id, instrument, event_kind::bar);
                });
        });
}

} // namespace

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
        : []() -> domain::position_view {
            throw std::runtime_error("position query is not wired to runtime");
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

    auto subscribe = [this, strategy_id, subscribed] (event_kind kind) {
        if (subscribed.empty()) {
            return make_ready_future<>();
        }
        if (_subscription_registry == nullptr) {
            return make_exception_future<>(std::runtime_error("subscription registry is not configured"));
        }

        future<> chain = make_ready_future<>();
        for (const auto& instrument_id : subscribed) {
            chain = std::move(chain).then([this, strategy_id, instrument_id, kind] {
                return _subscription_registry->subscribe(
                    strategy_id,
                    instrument_id,
                    kind);
            });
        }
        return chain;
    };
    ctx.subscribe_market_data = [subscribe] { return subscribe(event_kind::tick); };
    ctx.subscribe_bar_data = [subscribe] { return subscribe(event_kind::bar); };

    if (_context_configurer) {
        _context_configurer(ctx);
    }
    return ctx;
}

future<> strategy_lifecycle_manager::register_strategy(
    std::shared_ptr<strategy_base> strategy) {
    return with_semaphore(_lifecycle_lock, 1, [this, strategy = std::move(strategy)] () mutable {
        if (!strategy) {
            return make_exception_future<>(std::runtime_error("null strategy"));
        }
        if (_initialized) {
            return make_exception_future<>(std::runtime_error("cannot register an initialized manager"));
        }
        const auto id = strategy->strategy_id();
        if (_strategies.find(id) != _strategies.end()) {
            return make_exception_future<>(
                std::runtime_error("duplicate strategy_id: " + id));
        }
        _strategies.emplace(id, std::move(strategy));
        return make_ready_future<>();
    });
}

// B1: retain the same context until async initialization and callbacks finish.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
future<> strategy_lifecycle_manager::init_all() {
    ++_pending_initializations;
    return with_semaphore(_lifecycle_lock, 1, [this] {
        if (_initialized) {
            return make_ready_future<>();
        }
        return do_for_each(_strategies, [this](auto& item) {
            const auto id = item.first;
            auto strategy = item.second;
            if (strategy->state() == strategy_state::failed) {
                return make_ready_future<>();
            }

            auto session = std::make_shared<strategy_session>(make_default_context(*strategy));
            return futurize_invoke([strategy, session] {
                return strategy->on_init(session->context);
            }).then_wrapped([this, strategy, session, id](future<> result) -> future<> {
                try {
                    result.get();
                    _contexts.emplace(id, session);
                    strategy->set_state(strategy_state::initialized);
                    std::cout << "[INFO] strategy_lifecycle_manager: on_init OK, strategy="
                              << id << std::endl;
                    return make_ready_future<>();
                } catch (...) {
                    strategy->set_state(strategy_state::failed);
                    log_strategy_failure("on_init", id, std::current_exception());
                    return clear_strategy_subscriptions(*strategy, session->context.sub_registry);
                }
            });
        }).then([this] {
            _initialized = true;
        });
    }).finally([this] {
        --_pending_initializations;
    });
}

future<> strategy_lifecycle_manager::start_all() {
    return with_semaphore(_lifecycle_lock, 1, [this] {
        if (!_initialized) {
            return make_exception_future<>(std::runtime_error("strategy manager is not initialized"));
        }
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

            auto session = it_ctx->second;
            strategy->set_state(strategy_state::warmup);
            return futurize_invoke([strategy, session] {
                return strategy->on_warmup(session->context);
            }).then_wrapped([strategy, session, id](future<> result) {
                try {
                    result.get();
                    strategy->set_state(strategy_state::running);
                    std::cout << "[INFO] strategy_lifecycle_manager: started strategy="
                              << id << std::endl;
                } catch (...) {
                    strategy->set_state(strategy_state::failed);
                    log_strategy_failure("on_warmup", id, std::current_exception());
                }
            });
        });
    });
}

future<> strategy_lifecycle_manager::stop_session(
    std::shared_ptr<strategy_base> strategy,
    std::shared_ptr<strategy_session> session,
    sstring reason, bool preserve_failure) {
    auto owned_reason = std::make_shared<sstring>(std::move(reason));
    return session->callbacks.close().then([strategy, session, owned_reason, preserve_failure] {
        const bool failed = preserve_failure || strategy->state() == strategy_state::failed;
        return futurize_invoke([strategy, session, owned_reason] {
            return strategy->on_stop(session->context, *owned_reason);
        }).then_wrapped([strategy, session, owned_reason, failed](future<> result) -> future<> {
            try {
                result.get();
            } catch (...) {
                strategy->set_state(strategy_state::failed);
                log_strategy_failure("on_stop", strategy->strategy_id(), std::current_exception());
                return current_exception_as_future();
            }
            strategy->set_state(failed ? strategy_state::failed : strategy_state::stopped);
            std::cout << "[INFO] strategy_lifecycle_manager: stop callback completed for strategy="
                      << strategy->strategy_id() << std::endl;
            return make_ready_future<>();
        }).finally([strategy, session] {
            return clear_strategy_subscriptions(*strategy, session->context.sub_registry);
        });
    });
}

future<> strategy_lifecycle_manager::stop_all() {
    return with_semaphore(_lifecycle_lock, 1, [this] {
        for (auto& [id, strategy] : _strategies) {
            if (_contexts.contains(id) && strategy->state() != strategy_state::failed) {
                strategy->set_state(strategy_state::stopping);
            }
        }

        auto failure = std::make_shared<std::exception_ptr>();
        return do_for_each(_strategies, [this, failure](auto& item) {
            auto strategy = item.second;
            auto it_ctx = _contexts.find(item.first);
            if (it_ctx == _contexts.end()) {
                if (strategy->state() == strategy_state::stopping) {
                    strategy->set_state(strategy_state::failed);
                    auto ep = std::make_exception_ptr(std::runtime_error("stopping strategy has no context"));
                    log_strategy_failure("stop", item.first, ep);
                    if (!*failure) {
                        *failure = ep;
                    }
                }
                return make_ready_future<>();
            }
            const bool preserve_failure = strategy->state() == strategy_state::failed;
            return stop_session(std::move(strategy), it_ctx->second, "shutdown", preserve_failure)
                .handle_exception([failure](std::exception_ptr ep) {
                    if (!*failure) {
                        *failure = ep;
                    }
                    return make_ready_future<>();
                });
        }).then([this, failure] {
            _contexts.clear();
            _initialized = false;
            return *failure ? make_exception_future<>(*failure) : make_ready_future<>();
        });
    });
}

future<> strategy_lifecycle_manager::stop_strategy(const sstring& strategy_id) {
    return with_semaphore(_lifecycle_lock, 1, [this, id = sstring(strategy_id)] {
        auto it = _strategies.find(id);
        if (it == _strategies.end()) {
            return make_exception_future<>(std::runtime_error("strategy not found: " + id));
        }
        auto strategy = it->second;
        const bool preserve_failure = strategy->state() == strategy_state::failed;
        auto it_ctx = _contexts.find(id);
        if (it_ctx == _contexts.end()) {
            if (!preserve_failure) {
                strategy->set_state(strategy_state::stopped);
            }
            return make_ready_future<>();
        }
        if (!preserve_failure) {
            strategy->set_state(strategy_state::stopping);
        }
        return stop_session(strategy, it_ctx->second, "manual_stop", preserve_failure).finally([this, id] {
            _contexts.erase(id);
        });
    });
}

future<> strategy_lifecycle_manager::dispatch_market_data(
    const sstring& strategy_id, domain::market_data data) {
    return dispatch_callback(strategy_id,
        [data = std::move(data)] (strategy_base& strategy, strategy_context& ctx) mutable {
            return do_with(std::move(data), [&strategy, &ctx] (domain::market_data& owned) {
                return futurize_invoke([&strategy, &ctx, &owned] {
                    return strategy.on_market_data(ctx, owned);
                });
            });
        });
}

future<> strategy_lifecycle_manager::dispatch_bar(
    const sstring& strategy_id, domain::bar_data data) {
    return dispatch_callback(strategy_id,
        [data = std::move(data)] (strategy_base& strategy, strategy_context& ctx) mutable {
            return do_with(std::move(data), [&strategy, &ctx] (domain::bar_data& owned) {
                return futurize_invoke([&strategy, &ctx, &owned] {
                    return strategy.on_bar(ctx, owned);
                });
            });
        });
}

future<> strategy_lifecycle_manager::dispatch_order_update(const sstring& strategy_id, domain::order order) {
    return dispatch_callback(strategy_id,
        [order = std::move(order)] (strategy_base& strategy, strategy_context& ctx) mutable {
            return do_with(std::move(order), [&strategy, &ctx] (domain::order& owned) {
                return futurize_invoke([&strategy, &ctx, &owned] {
                    return strategy.on_order_update(ctx, owned);
                });
            });
        }, false);
}

future<> strategy_lifecycle_manager::dispatch_trade_update(const sstring& strategy_id, domain::trade_report trade) {
    return dispatch_callback(strategy_id,
        [trade = std::move(trade)] (strategy_base& strategy, strategy_context& ctx) mutable {
            return do_with(std::move(trade), [&strategy, &ctx] (domain::trade_report& owned) {
                return futurize_invoke([&strategy, &ctx, &owned] {
                    return strategy.on_trade_update(ctx, owned);
                });
            });
        }, false);
}

future<> strategy_lifecycle_manager::dispatch_callback(
    const sstring& strategy_id, strategy_callback_t callback, bool is_signal) {
    auto strategy = get_strategy(strategy_id);
    auto it_ctx = _contexts.find(strategy_id);
    if (!strategy || strategy->state() != strategy_state::running || it_ctx == _contexts.end()) {
        return make_exception_future<>(std::runtime_error("strategy is not running: " + strategy_id));
    }
    if (strategy->config().max_queue_depth <= 0 ||
        strategy->stats().queue_depth >= strategy->config().max_queue_depth) {
        return make_exception_future<>(std::runtime_error("strategy callback queue is full: " + strategy_id));
    }

    auto session = it_ctx->second;
    if (session->callbacks.is_closed()) {
        return make_exception_future<>(std::runtime_error("strategy callbacks are stopping: " + strategy_id));
    }
    auto callback_holder = session->callbacks.hold();
    ++strategy->stats().queue_depth;
    return with_semaphore(session->callback_lock, 1,
                          [session, strategy, callback = std::move(callback), is_signal] () mutable {
        if (strategy->state() == strategy_state::failed) {
            return make_exception_future<>(std::runtime_error("strategy failed before callback delivery"));
        }
        return futurize_invoke([&callback, strategy, session] {
            return callback(*strategy, session->context);
        }).then([session, strategy, is_signal] {
            if (is_signal) {
                ++strategy->stats().signal_count;
            }
        });
    }).handle_exception([strategy](std::exception_ptr ep) {
        strategy->set_state(strategy_state::failed);
        return make_exception_future<>(ep);
    }).finally([strategy, session, callback_holder = std::move(callback_holder)] () mutable {
        --strategy->stats().queue_depth;
        callback_holder.release();
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
        return &it->second->context;
    }
    return nullptr;
}

} // namespace seastar::xtrader
