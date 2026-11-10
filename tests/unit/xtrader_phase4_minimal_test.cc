/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *   http://www.apache.org/licenses/LICENSE-2.0
 */
#include <seastar/core/sleep.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/backtest_driver.hh>
#include <seastar/xtrader/event_bus.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>
#include <seastar/xtrader/performance_metrics.hh>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

class phase4_test_strategy final : public seastar::xtrader::strategy_base {
public:
    explicit phase4_test_strategy(bool submit_on_init)
        : seastar::xtrader::strategy_base(seastar::xtrader::strategy_config{
              .strategy_id = "phase4-test",
              .strategy_name = "phase4-test",
              .strategy_type = "test",
              .subscribed_instruments = {"rb9999"},
          })
        , _submit_on_init(submit_on_init) {
    }

    seastar::future<> on_init(seastar::xtrader::strategy_context& ctx) override {
        init_called = true;
        if (!ctx.subscribe_market_data) {
            return seastar::make_ready_future<>();
        }

        return ctx.subscribe_market_data().then([this, &ctx] {
            initial_position = ctx.query_position();
            if (!_submit_on_init) {
                return seastar::make_ready_future<>();
            }

            return ctx.submit_order(seastar::xtrader::domain::order_request{
                .strategy_id = strategy_id(),
                .instrument_id = "rb9999",
                .direction = seastar::xtrader::domain::side::buy,
                .offset = seastar::xtrader::domain::offset_flag::open,
                .hedge = seastar::xtrader::domain::hedge_flag::speculation,
                .volume = 1,
                .price = 3500.0,
            }).then([this](seastar::xtrader::domain::order_status status) {
                last_submit_status = status;
            });
        });
    }

    seastar::future<> on_warmup(seastar::xtrader::strategy_context&) override {
        warmup_called = true;
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_market_data(
        seastar::xtrader::strategy_context&,
        const seastar::xtrader::domain::market_data& md) override {
        market_data_count++;
        last_market_data = md;
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_bar(
        seastar::xtrader::strategy_context&,
        const seastar::xtrader::domain::bar_data& bar) override {
        ++bar_count;
        last_bar = bar;
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_stop(
        seastar::xtrader::strategy_context&,
        const seastar::sstring& reason) override {
        stop_called = true;
        stop_reason = reason;
        return seastar::make_ready_future<>();
    }

    bool init_called = false;
    bool warmup_called = false;
    bool stop_called = false;
    seastar::xtrader::domain::position_view initial_position{};
    seastar::xtrader::domain::order_status last_submit_status =
        seastar::xtrader::domain::order_status::unknown;
    seastar::xtrader::domain::market_data last_market_data{};
    seastar::xtrader::domain::bar_data last_bar{};
    seastar::sstring stop_reason;
    int market_data_count = 0;
    int bar_count = 0;

private:
    bool _submit_on_init = false;
};

// B1: exercise references retained across asynchronous strategy callbacks.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
class delayed_test_strategy final : public seastar::xtrader::strategy_base {
public:
    explicit delayed_test_strategy(seastar::sstring id, int queue_depth = 2)
        : strategy_base(seastar::xtrader::strategy_config{
              .strategy_id = id,
              .strategy_name = id,
              .strategy_type = "delayed-test",
              .subscribed_instruments = {"rb9999"},
              .max_queue_depth = queue_depth,
          }) {}

    seastar::future<> on_init(seastar::xtrader::strategy_context& ctx) override {
        init_entered = true;
        return init_release.get_future().then([this, &ctx] {
            init_context_id = ctx.strategy_id;
            initial_position = ctx.query_position().long_today;
            if (fail_init) {
                throw std::runtime_error("expected initialization failure");
            }
            return ctx.subscribe_market_data().then([this] {
                if (fail_after_subscribe) {
                    throw std::runtime_error("expected failure after subscribing");
                }
            });
        });
    }

    seastar::future<> on_market_data(
        seastar::xtrader::strategy_context& ctx,
        const seastar::xtrader::domain::market_data& md) override {
        ++market_entered;
        ++active_callbacks;
        max_active_callbacks = std::max(max_active_callbacks, active_callbacks);
        if (defer_first_market && md.last_price == 1.0) {
            return market_release.get_future().then([this, &ctx, &md] {
                return finish_market(ctx, md);
            });
        }
        return finish_market(ctx, md);
    }

    seastar::future<> on_warmup(seastar::xtrader::strategy_context&) override {
        if (fail_warmup) {
            return seastar::make_exception_future<>(std::runtime_error("expected warmup failure"));
        }
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_stop(
        seastar::xtrader::strategy_context& ctx,
        const seastar::sstring& reason) override {
        stop_entered = true;
        queue_depth_at_stop = stats().queue_depth;
        if (defer_stop) {
            return stop_release.get_future().then([this, &ctx, &reason] {
                return finish_stop(ctx, reason);
            });
        }
        return finish_stop(ctx, reason);
    }

    seastar::promise<> init_release;
    seastar::promise<> market_release;
    seastar::promise<> stop_release;
    bool fail_init = false;
    bool fail_after_subscribe = false;
    bool fail_warmup = false;
    bool fail_first_market = false;
    bool fail_stop = false;
    bool defer_first_market = false;
    bool defer_stop = false;
    bool init_entered = false;
    bool stop_entered = false;
    int initial_position = 0;
    int market_entered = 0;
    int active_callbacks = 0;
    int max_active_callbacks = 0;
    int queue_depth_at_stop = -1;
    seastar::sstring init_context_id;
    seastar::sstring stop_context_id;
    seastar::sstring observed_stop_reason;
    std::vector<double> observed_prices;
    std::vector<seastar::sstring> observed_context_ids;

private:
    seastar::future<> finish_market(
        seastar::xtrader::strategy_context& ctx,
        const seastar::xtrader::domain::market_data& md) {
        observed_context_ids.push_back(ctx.strategy_id);
        observed_prices.push_back(md.last_price);
        --active_callbacks;
        if (fail_first_market && md.last_price == 1.0) {
            return seastar::make_exception_future<>(std::runtime_error("expected market callback failure"));
        }
        return seastar::make_ready_future<>();
    }

    seastar::future<> finish_stop(
        seastar::xtrader::strategy_context& ctx,
        const seastar::sstring& reason) {
        stop_context_id = ctx.strategy_id;
        observed_stop_reason = reason;
        if (fail_stop) {
            return seastar::make_exception_future<>(std::runtime_error("expected stop failure"));
        }
        return seastar::make_ready_future<>();
    }
};

void use_empty_positions(seastar::xtrader::strategy_lifecycle_manager& manager) {
    manager.set_position_query_handler([] {
        return seastar::xtrader::domain::position_view{};
    });
}

template <typename Predicate>
void wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    BOOST_REQUIRE(predicate());
}

} // namespace

// Phase 6 transport instrumentation; source: user request, 2026-11-09.
// Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_phase6_event_bus_records_backpressure) {
    auto& metrics = seastar::xtrader::performance_metrics::instance();
    metrics.reset();
    seastar::xtrader::event_bus bus({.max_queue_depth = 1});
    seastar::xtrader::market_event event;
    event.instrument_id = "offline-test";
    BOOST_REQUIRE(bus.enqueue(event));
    BOOST_REQUIRE(!bus.enqueue(event));
    const auto snapshot = metrics.snapshot_dispatch();
    BOOST_REQUIRE_EQUAL(snapshot.queue_depth, 1);
    BOOST_REQUIRE_EQUAL(snapshot.peak_queue_depth, 1);
    BOOST_REQUIRE_EQUAL(snapshot.dropped_events, 1);
    metrics.reset();
}

SEASTAR_THREAD_TEST_CASE(xtrader_phase4_lifecycle_wires_runtime_and_subscriptions) {
    seastar::xtrader::subscription_registry registry;
    seastar::xtrader::strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);

    seastar::xtrader::domain::order_request captured_request;
    manager.set_submit_order_handler([&captured_request](const seastar::xtrader::domain::order_request& request) {
        captured_request = request;
        return seastar::make_ready_future<seastar::xtrader::domain::order_status>(
            seastar::xtrader::domain::order_status::accepted);
    });
    manager.set_position_query_handler([] {
        return seastar::xtrader::domain::position_view{
            .long_today = 2,
            .long_yesterday = 1,
        };
    });

    auto strategy = std::make_shared<phase4_test_strategy>(true);
    manager.register_strategy(strategy).get();
    manager.init_all().get();

    BOOST_REQUIRE(strategy->init_called);
    BOOST_REQUIRE(registry.has_subscriber("phase4-test", "rb9999", seastar::xtrader::event_kind::tick));
    BOOST_REQUIRE(captured_request.instrument_id == "rb9999");
    BOOST_REQUIRE(strategy->last_submit_status == seastar::xtrader::domain::order_status::accepted);
    BOOST_REQUIRE_EQUAL(strategy->initial_position.long_today, 2);

    manager.start_all().get();
    BOOST_REQUIRE(strategy->warmup_called);
    BOOST_REQUIRE(strategy->state() == seastar::xtrader::strategy_state::running);

    manager.stop_all().get();
    BOOST_REQUIRE(strategy->stop_called);
    BOOST_REQUIRE(strategy->state() == seastar::xtrader::strategy_state::stopped);
    BOOST_REQUIRE(strategy->stop_reason == "shutdown");
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_delayed_initialization_isolated_and_serialized) {
    using namespace seastar::xtrader;
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    manager.set_position_query_handler([] {
        return domain::position_view{.long_today = 7};
    });

    auto healthy = std::make_shared<delayed_test_strategy>("healthy");
    auto failed = std::make_shared<delayed_test_strategy>("failed");
    auto failed_early = std::make_shared<delayed_test_strategy>("failed-early");
    failed->fail_after_subscribe = true;
    failed_early->fail_init = true;
    manager.register_strategy(healthy).get();
    manager.register_strategy(failed).get();
    manager.register_strategy(failed_early).get();

    auto initializing = manager.init_all();
    BOOST_REQUIRE(!initializing.available());
    BOOST_CHECK_THROW(manager.set_subscription_registry(&registry), std::logic_error);
    healthy->init_release.set_value();
    failed->init_release.set_value();
    failed_early->init_release.set_value();
    initializing.get();

    BOOST_REQUIRE(healthy->init_context_id == "healthy");
    BOOST_REQUIRE_EQUAL(healthy->initial_position, 7);
    BOOST_REQUIRE(failed->init_context_id == "failed");
    BOOST_REQUIRE(failed->state() == strategy_state::failed);
    BOOST_REQUIRE(failed_early->state() == strategy_state::failed);
    BOOST_REQUIRE(manager.get_strategy_context("failed") == nullptr);
    BOOST_REQUIRE(!registry.has_subscriber("failed", "rb9999", event_kind::tick));
    BOOST_REQUIRE(registry.has_subscriber("healthy", "rb9999", event_kind::tick));
    manager.start_all().get();
    BOOST_REQUIRE(healthy->state() == strategy_state::running);
    manager.stop_all().get();
    BOOST_REQUIRE(healthy->observed_stop_reason == "shutdown");
    BOOST_REQUIRE(!registry.has_subscriber("healthy", "rb9999", event_kind::tick));
    manager.set_subscription_registry(&registry);

    strategy_lifecycle_manager second_manager;
    subscription_registry second_registry;
    second_manager.set_subscription_registry(&second_registry);
    use_empty_positions(second_manager);
    auto delayed = std::make_shared<delayed_test_strategy>("stop-during-init");
    second_manager.register_strategy(delayed).get();
    auto second_init = second_manager.init_all();
    auto stopping = second_manager.stop_all();
    BOOST_REQUIRE(!stopping.available());
    delayed->init_release.set_value();
    second_init.get();
    stopping.get();
    BOOST_REQUIRE(delayed->init_context_id == "stop-during-init");
    BOOST_REQUIRE(delayed->observed_stop_reason == "shutdown");
    BOOST_REQUIRE(!second_manager.is_initialized());
    BOOST_REQUIRE(second_manager.get_strategy_context("stop-during-init") == nullptr);
    second_manager.set_subscription_registry(&second_registry);
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_bus_drains_async_callbacks_and_reports_failures) {
    using namespace seastar::xtrader;
    auto& metrics = performance_metrics::instance();
    metrics.reset();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto strategy = std::make_shared<delayed_test_strategy>("bus-test");
    strategy->defer_first_market = true;
    strategy->defer_stop = true;
    manager.register_strategy(strategy).get();
    strategy->init_release.set_value();
    manager.init_all().get();
    manager.start_all().get();

    event_bus bus({.max_queue_depth = 1, .batch_size = 1, .dispatch_budget_us = 1});
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    market_event event;
    event.instrument_id = "rb9999";
    event.raw_md.instrument_id = "rb9999";
    event.raw_md.last_price = 1.0;
    BOOST_REQUIRE(bus.enqueue(event));
    wait_for([&] { return strategy->market_entered == 1; });

    event.raw_md.last_price = 2.0;
    BOOST_REQUIRE(bus.enqueue(event));
    auto stopping = bus.stop();
    BOOST_REQUIRE(!stopping.available());
    BOOST_REQUIRE(!bus.enqueue(event));
    strategy->market_release.set_value();
    stopping.get();
    BOOST_REQUIRE_EQUAL(strategy->observed_prices.size(), 2);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices[0], 1.0);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices[1], 2.0);
    BOOST_REQUIRE(strategy->observed_context_ids[0] == "bus-test");
    BOOST_REQUIRE_EQUAL(strategy->max_active_callbacks, 1);
    BOOST_REQUIRE_EQUAL(bus.metrics().enqueued, 2);
    BOOST_REQUIRE_EQUAL(bus.metrics().dispatched, 2);
    BOOST_REQUIRE_EQUAL(bus.metrics().dropped, 1);
    BOOST_REQUIRE_EQUAL(metrics.snapshot_dispatch().error_count, 0);

    bus.start().get();
    BOOST_REQUIRE(bus.enqueue(event));
    bus.stop().get();
    BOOST_REQUIRE_EQUAL(strategy->observed_prices.size(), 3);

    auto strategy_stopping = manager.stop_all();
    wait_for([&] { return strategy->stop_entered; });
    BOOST_REQUIRE(!strategy_stopping.available());
    strategy->stop_release.set_value();
    strategy_stopping.get();
    BOOST_REQUIRE(strategy->stop_context_id == "bus-test");
    BOOST_REQUIRE(strategy->observed_stop_reason == "shutdown");
    metrics.reset();
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_callback_failure_stops_new_events_and_is_reported) {
    using namespace seastar::xtrader;
    auto& metrics = performance_metrics::instance();
    metrics.reset();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto strategy = std::make_shared<delayed_test_strategy>("callback-failed");
    strategy->fail_first_market = true;
    manager.register_strategy(strategy).get();
    strategy->init_release.set_value();
    manager.init_all().get();
    manager.start_all().get();

    event_bus bus({.max_queue_depth = 2, .batch_size = 1, .dispatch_budget_us = 100});
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    market_event event;
    event.instrument_id = "rb9999";
    event.raw_md.instrument_id = "rb9999";
    event.raw_md.last_price = 1.0;
    BOOST_REQUIRE(bus.enqueue(event));
    event.raw_md.last_price = 2.0;
    BOOST_REQUIRE(bus.enqueue(event));
    BOOST_CHECK_THROW(bus.stop().get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(bus.metrics().dispatched, 2);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices.size(), 1);
    BOOST_REQUIRE(strategy->state() == strategy_state::failed);
    BOOST_REQUIRE_GE(metrics.snapshot_dispatch().error_count, 1);
    BOOST_REQUIRE(!bus.enqueue(event));
    BOOST_CHECK_THROW(bus.start().get(), std::runtime_error);
    manager.stop_all().get();
    BOOST_REQUIRE(strategy->stop_entered);
    BOOST_REQUIRE(strategy->state() == strategy_state::failed);
    metrics.reset();
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_single_strategy_stop_waits_for_queued_callbacks) {
    using namespace seastar::xtrader;
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto strategy = std::make_shared<delayed_test_strategy>("single", 2);
    strategy->defer_first_market = true;
    strategy->defer_stop = true;
    manager.register_strategy(strategy).get();
    strategy->init_release.set_value();
    manager.init_all().get();
    manager.start_all().get();

    domain::market_data first;
    first.last_price = 1.0;
    domain::market_data second;
    second.last_price = 2.0;
    auto first_result = manager.dispatch_market_data("single", first);
    auto second_result = manager.dispatch_market_data("single", second);
    BOOST_REQUIRE_EQUAL(strategy->stats().queue_depth, 2);
    BOOST_CHECK_THROW(manager.dispatch_market_data("single", second).get(), std::runtime_error);
    auto stopping = manager.stop_strategy("single");
    BOOST_REQUIRE(!stopping.available());
    BOOST_CHECK_THROW(manager.dispatch_market_data("single", second).get(), std::runtime_error);

    strategy->market_release.set_value();
    first_result.get();
    second_result.get();
    wait_for([&] { return strategy->stop_entered; });
    BOOST_REQUIRE(!stopping.available());
    strategy->stop_release.set_value();
    stopping.get();
    BOOST_REQUIRE_EQUAL(strategy->stats().queue_depth, 0);
    BOOST_REQUIRE_EQUAL(strategy->queue_depth_at_stop, 0);
    BOOST_REQUIRE_EQUAL(strategy->max_active_callbacks, 1);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices.size(), 2);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices[0], 1.0);
    BOOST_REQUIRE_EQUAL(strategy->observed_prices[1], 2.0);
    BOOST_REQUIRE(strategy->observed_stop_reason == "manual_stop");
    BOOST_REQUIRE(manager.get_strategy_context("single") == nullptr);
    BOOST_REQUIRE(!registry.has_subscriber("single", "rb9999", event_kind::tick));
    manager.stop_all().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_stop_failures_are_reported_after_other_strategies_stop) {
    using namespace seastar::xtrader;
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto failed = std::make_shared<delayed_test_strategy>("stop-failed");
    auto healthy = std::make_shared<delayed_test_strategy>("stop-healthy");
    failed->fail_stop = true;
    manager.register_strategy(failed).get();
    manager.register_strategy(healthy).get();
    failed->init_release.set_value();
    healthy->init_release.set_value();
    manager.init_all().get();
    manager.start_all().get();

    BOOST_CHECK_THROW(manager.stop_all().get(), std::runtime_error);
    BOOST_REQUIRE(failed->state() == strategy_state::failed);
    BOOST_REQUIRE(healthy->state() == strategy_state::stopped);
    BOOST_REQUIRE(failed->stop_entered);
    BOOST_REQUIRE(healthy->stop_entered);
}

SEASTAR_THREAD_TEST_CASE(xtrader_b1_missing_dependencies_and_warmup_cleanup) {
    using namespace seastar::xtrader;
    strategy_lifecycle_manager missing_registry;
    use_empty_positions(missing_registry);
    auto no_registry = std::make_shared<delayed_test_strategy>("no-registry");
    missing_registry.register_strategy(no_registry).get();
    BOOST_CHECK_THROW(missing_registry.start_all().get(), std::runtime_error);
    no_registry->init_release.set_value();
    missing_registry.init_all().get();
    BOOST_REQUIRE(no_registry->state() == strategy_state::failed);
    missing_registry.stop_all().get();

    subscription_registry registry;
    strategy_lifecycle_manager missing_positions;
    missing_positions.set_subscription_registry(&registry);
    auto no_positions = std::make_shared<delayed_test_strategy>("no-positions");
    missing_positions.register_strategy(no_positions).get();
    no_positions->init_release.set_value();
    missing_positions.init_all().get();
    BOOST_REQUIRE(no_positions->state() == strategy_state::failed);
    missing_positions.stop_all().get();

    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto failed = std::make_shared<delayed_test_strategy>("warmup-failed");
    failed->fail_warmup = true;
    manager.register_strategy(failed).get();
    failed->init_release.set_value();
    manager.init_all().get();
    manager.start_all().get();
    BOOST_REQUIRE(failed->state() == strategy_state::failed);
    manager.stop_all().get();
    BOOST_REQUIRE(failed->stop_entered);
    BOOST_REQUIRE(failed->state() == strategy_state::failed);
    BOOST_REQUIRE(!registry.has_subscriber("warmup-failed", "rb9999", event_kind::tick));
}

SEASTAR_THREAD_TEST_CASE(xtrader_phase4_backtest_driver_dispatches_historical_bars) {
    namespace fs = std::filesystem;

    const auto temp_root = fs::temp_directory_path() / "xtrader_phase4_minimal_test" / "live_capture" / "XSGE" / "rb9999" / "1m";
    fs::create_directories(temp_root);

    {
        std::ofstream manifest(temp_root / "dataset-manifest.conf");
        manifest << "symbol=rb9999\n";
        manifest << "exchange=XSGE\n";
        manifest << "timeframe=1m\n";
        manifest << "active_file=rb9999.XSGE.1m.csv\n";
    }

    {
        std::ofstream csv(temp_root / "rb9999.XSGE.1m.csv");
        csv << "date,open,high,low,close,volume,open_interest\n";
        csv << "2009/3/27 9:01,3550,3662,3550,3660,10528,7910\n";
        csv << "2009/3/27 9:02,3651,3661,3582,3582,6326,10226\n";
    }

    seastar::xtrader::subscription_registry registry;
    seastar::xtrader::strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);

    auto strategy = std::make_shared<phase4_test_strategy>(false);
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.get_strategy_context("phase4-test")->subscribe_bar_data().get();
    manager.start_all().get();

    seastar::xtrader::event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();

    seastar::xtrader::backtest_config cfg;
    cfg.data_root = temp_root.parent_path().parent_path().parent_path().parent_path().string();
    seastar::xtrader::backtest_driver driver(cfg);
    driver.set_event_bus(&bus);
    driver.load_data("rb9999").get();
    driver.start().get();
    driver.run().get();

    bus.stop().get();
    manager.stop_all().get();

    BOOST_REQUIRE_EQUAL(strategy->market_data_count, 0);
    BOOST_REQUIRE_EQUAL(strategy->bar_count, 2);
    BOOST_REQUIRE(strategy->last_bar.instrument_id == "rb9999");
    BOOST_REQUIRE(strategy->last_bar.exchange_id == "XSGE");
    BOOST_REQUIRE(strategy->last_bar.timeframe == "1m");
    BOOST_REQUIRE(strategy->last_bar.timestamp == "09:02:00");
    BOOST_REQUIRE(strategy->last_bar.calendar_date == "20090327");
    BOOST_REQUIRE_EQUAL(strategy->last_bar.close_price, 3582.0);
    BOOST_REQUIRE(!registry.has_subscriber("phase4-test", "rb9999", seastar::xtrader::event_kind::bar));

    fs::remove_all(temp_root.parent_path().parent_path().parent_path().parent_path());
}

SEASTAR_THREAD_TEST_CASE(xtrader_phase4_explicit_demo_replays_ticks) {
    using namespace seastar::xtrader;
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    use_empty_positions(manager);
    auto strategy = std::make_shared<phase4_test_strategy>(false);
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.start_all().get();
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    backtest_config cfg;
    cfg.data_mode = backtest_data_mode::embedded_demo;
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    driver.set_event_bus(&bus);
    driver.start().get();
    driver.run().get();
    bus.stop().get();
    manager.stop_all().get();
    BOOST_REQUIRE_EQUAL(strategy->market_data_count, 2);
    BOOST_REQUIRE_EQUAL(strategy->bar_count, 0);
    BOOST_REQUIRE(strategy->last_market_data.update_time == "09:32:00");
    BOOST_REQUIRE_EQUAL(driver.stats().total_ticks, 2);
    BOOST_REQUIRE_EQUAL(driver.stats().total_bars, 0);
}