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
#include <seastar/core/sleep.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/backtest_driver.hh>
#include <seastar/xtrader/historical_data_manager.hh>

#include "xtrader_replay_test_files.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace seastar::xtrader;
using namespace seastar::xtrader::test;

template <typename Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    return predicate();
}

// B3: observe replay callbacks without attaching any trading backend.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
class replay_recording_strategy final : public strategy_base {
public:
    explicit replay_recording_strategy(std::vector<seastar::sstring> instruments,
                                       seastar::sstring id = "replay-test")
        : strategy_base(strategy_config{
            .strategy_id = id,
            .strategy_name = id,
            .strategy_type = "offline",
            .subscribed_instruments = std::move(instruments),
        }) {}

    seastar::future<> on_init(strategy_context& ctx) override {
        return ctx.subscribe_bar_data();
    }

    seastar::future<> on_market_data(strategy_context&, const domain::market_data&) override {
        ++tick_count;
        return seastar::make_ready_future<>();
    }

    seastar::future<> on_bar(strategy_context&, const domain::bar_data& bar) override {
        ++bar_callbacks_entered;
        if (defer_callback && bar_callbacks_entered == 1) {
            return callback_release.get_future().then([this, &bar] {
                return record_bar(bar);
            });
        }
        return record_bar(bar);
    }

    backtest_driver* driver = nullptr;
    bool fail_callback = false;
    bool defer_callback = false;
    int tick_count = 0;
    int bar_callbacks_entered = 0;
    seastar::promise<> callback_release;
    std::vector<domain::bar_data> bars;
    std::vector<int64_t> clock_times;
    std::vector<seastar::sstring>* callback_order = nullptr;

private:
    seastar::future<> record_bar(const domain::bar_data& bar) {
        bars.push_back(bar);
        if (callback_order) {
            callback_order->push_back(strategy_id());
        }
        if (driver) {
            clock_times.push_back(driver->clock().now_nanos());
        }
        if (fail_callback) {
            return seastar::make_exception_future<>(std::runtime_error("expected bar callback failure"));
        }
        return seastar::make_ready_future<>();
    }
};

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_replay_requires_explicit_demo_data) {
    replay_test_directory directory;
    backtest_config cfg;
    cfg.data_root = (directory.path() / "missing").string();
    backtest_driver historical(cfg);
    BOOST_CHECK_THROW(historical.load_data("rb9999").get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(historical.stats().total_ticks, 0);
    BOOST_CHECK_THROW(historical.run().get(), std::runtime_error);

    cfg.data_mode = backtest_data_mode::embedded_demo;
    backtest_driver demo(cfg);
    demo.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(demo.stats().total_ticks, 2);
    demo.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(demo.stats().total_ticks, 2);
    demo.start().get();
    BOOST_CHECK_THROW(demo.run().get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_exports_bars_only_with_explicit_time_contract) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:02,102,110,90,103,7,50\n"
        "2024/2/29 9:01,100,110,90,101,6,40\n");
    backtest_config config;
    config.data_root = directory.path().string();
    backtest_driver driver(config);
    driver.load_data("rb9999").get();

    backtest_bar_export_options options;
    BOOST_CHECK_THROW(static_cast<void>(driver.export_offline_bars(options)), std::invalid_argument);
    options.timestamp_kind = offline_bar_timestamp::open;
    options.duration_nanos = 60'000'000'000LL;
    options.time_domain = "synthetic-naive-local";
    options.resolve_trading_day = [] (const seastar::sstring& exchange, const seastar::sstring& instrument,
                                      const seastar::sstring& date, const seastar::sstring&) {
        BOOST_CHECK_EQUAL(exchange, "XSGE");
        BOOST_CHECK_EQUAL(instrument, "rb9999");
        BOOST_CHECK_EQUAL(date, "20240229");
        return seastar::sstring("20240229");
    };
    const auto bars = driver.export_offline_bars(options);
    BOOST_REQUIRE_EQUAL(bars.size(), 2);
    BOOST_CHECK(bars[0].timestamp_nanos < bars[1].timestamp_nanos);
    BOOST_CHECK_EQUAL(bars[0].open_price, 100);
    BOOST_CHECK_EQUAL(bars[0].trading_day, "20240229");
    BOOST_CHECK_EQUAL(bars[0].time_domain, "synthetic-naive-local");
    BOOST_CHECK(bars[0].timestamp_kind == offline_bar_timestamp::open);

    options.resolve_trading_day = [] (const auto&, const auto&, const auto&, const auto&) {
        return seastar::sstring("20240230");
    };
    BOOST_CHECK_THROW(static_cast<void>(driver.export_offline_bars(options)), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_rejects_corrupt_data_and_allows_retry) {
    replay_test_directory directory;
    const std::string valid = "2024/2/29 9:01,100,101,99,100,10,20\n";
    const auto csv = write_dataset(directory.path(), "XSGE", valid);

    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(driver.stats().total_bars, 1);

    const std::vector<std::string> invalid_rows{
        "2024/2/29 9:02,100,101,99,100,bad,20\n",
        "2024/2/29 9:02,100,101,99,100,10x,20\n",
        "2024/2/29 9:02,100,101,99,nan,10,20\n",
        "2024/2/29 9:02,100,101,99,102,10,20\n",
        "2024/2/29 9:02,100,101,99,100,1.5,20\n",
        "2024/2/29 9:02,100,101,99,100,2147483648,20\n",
        "2026/2/29 9:02,100,101,99,100,10,20\n",
        "2024/2/29 25:02,100,101,99,100,10,20\n",
        "2024/2/29 9:02,100,101\n",
    };
    for (const auto& invalid : invalid_rows) {
        write_text(csv, "date,open,high,low,close,volume,open_interest\n" + valid + invalid);
        BOOST_CHECK_THROW(driver.load_data("rb9999").get(), std::runtime_error);
        BOOST_REQUIRE_EQUAL(driver.stats().total_bars, 1);
        try {
            driver.run().get();
            BOOST_FAIL("replay should remain blocked after a failed data load");
        } catch (const std::runtime_error& error) {
            BOOST_CHECK(std::string(error.what()).find("data load failed") != std::string::npos);
        }
    }

    write_text(csv, "date,open,high,low,close,volume,open_interest\n" + valid
        + "2026/2/29 9:02,100,101,99,100,10,20\n");
    try {
        driver.load_data("rb9999").get();
        BOOST_FAIL("invalid date should be rejected");
    } catch (const std::runtime_error& error) {
        BOOST_CHECK(std::string(error.what()).find("line 3") != std::string::npos);
    }

    write_text(csv, "date,open,high,low,close,volume,open_interest\n" + valid);
    driver.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(driver.stats().total_bars, 1);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_rejects_ambiguous_dataset_selection) {
    replay_test_directory directory;
    const std::string valid = "2024/2/29 9:01,100,101,99,100,10,20\n";
    write_dataset(directory.path(), "XSGE", valid);
    write_dataset(directory.path(), "XDCE", valid);

    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver ambiguous(cfg);
    BOOST_CHECK_THROW(ambiguous.load_data("rb9999").get(), std::runtime_error);

    cfg.dataset_exchange = "xsge";
    cfg.dataset_timeframe = "1m";
    backtest_driver selected(cfg);
    selected.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(selected.stats().total_bars, 1);
    BOOST_CHECK_THROW(selected.load_data("missing").get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_historical_strict_load_rejects_partial_data) {
    replay_test_directory directory;
    const auto csv = write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:01,100,101,99,100,10,20\n"
        "2024/2/29 9:02,100,101,99,bad,10,20\n");
    historical_data_manager manager(directory.path());
    std::string warning;
    const auto datasets = manager.scan_datasets(&warning);
    BOOST_REQUIRE(warning.empty());
    BOOST_REQUIRE_EQUAL(datasets.size(), 1);

    std::vector<historical_bar> bars;
    BOOST_REQUIRE(manager.load_bars(datasets.front(), bars, &warning));
    BOOST_REQUIRE_EQUAL(bars.size(), 1);
    BOOST_REQUIRE(!manager.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict));
    BOOST_REQUIRE(bars.empty());
    BOOST_CHECK(warning.find("line 3") != std::string::npos);

    fs::remove(csv);
    BOOST_REQUIRE(!manager.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict));
    BOOST_CHECK(warning.find("missing") != std::string::npos);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_reports_event_bus_backpressure) {
    replay_test_directory directory;
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    cfg.data_mode = backtest_data_mode::embedded_demo;
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();

    subscription_registry registry;
    strategy_lifecycle_manager manager;
    event_bus bus({.max_queue_depth = 1, .batch_size = 1, .dispatch_budget_us = 500});
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    driver.set_event_bus(&bus);
    driver.start().get();
    market_event queued;
    queued.instrument_id = "already-queued";
    BOOST_REQUIRE(bus.enqueue(queued));
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    bus.stop().get();
    BOOST_REQUIRE_EQUAL(bus.metrics().enqueued, 1);
    BOOST_REQUIRE_EQUAL(bus.metrics().dropped, 1);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_orders_bars_globally_and_drains_callbacks) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE",
        "2024/3/1 9:02,100,101,99,100,10,20\n"
        "2024/2/29 9:01:00.500,100,101,99,100,11,21\n");
    write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:01:00.500,200,201,199,200,12,22\n"
        "2024/2/29 9:01:00.100,200,201,199,200,13,23\n", "cu9999");

    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    driver.load_data("cu9999").get();

    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    auto strategy = std::make_shared<replay_recording_strategy>(
        std::vector<seastar::sstring>{"rb9999", "cu9999"});
    strategy->driver = &driver;
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.start_all().get();

    event_bus bus({.max_queue_depth = 1, .batch_size = 1, .dispatch_budget_us = 500});
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    driver.set_event_bus(&bus);
    driver.start().get();
    driver.run().get();

    BOOST_REQUIRE_EQUAL(driver.stats().total_ticks, 0);
    BOOST_REQUIRE_EQUAL(driver.stats().total_bars, 4);
    BOOST_REQUIRE_EQUAL(strategy->tick_count, 0);
    BOOST_REQUIRE_EQUAL(strategy->bars.size(), 4);
    BOOST_REQUIRE(strategy->bars[0].instrument_id == "cu9999");
    BOOST_REQUIRE(strategy->bars[0].timestamp == "09:01:00.100");
    BOOST_REQUIRE(strategy->bars[1].instrument_id == "cu9999");
    BOOST_REQUIRE(strategy->bars[2].instrument_id == "rb9999");
    BOOST_REQUIRE(strategy->bars[3].calendar_date == "20240301");
    BOOST_REQUIRE_EQUAL(strategy->clock_times[1] - strategy->clock_times[0], 400'000'000);
    BOOST_REQUIRE_EQUAL(strategy->clock_times[2], strategy->clock_times[1]);
    BOOST_REQUIRE_GT(strategy->clock_times[3], strategy->clock_times[2]);
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    bus.stop().get();
    manager.stop_all().get();
    BOOST_REQUIRE(!registry.has_subscriber("replay-test", "cu9999", event_kind::bar));
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_filters_calendar_dates_and_rejects_invalid_ranges) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE",
        "2024/2/28 9:01,100,101,99,100,10,20\n"
        "2024/2/29 9:01,100,101,99,100,10,20\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    cfg.start_date = "20240229";
    cfg.end_date = "2024-02-29";
    backtest_driver selected(cfg);
    selected.load_data("rb9999").get();
    BOOST_REQUIRE_EQUAL(selected.stats().total_bars, 1);

    cfg.start_date = "20240230";
    backtest_driver invalid(cfg);
    BOOST_CHECK_THROW(invalid.load_data("rb9999").get(), std::invalid_argument);
    cfg.start_date = "20240301";
    backtest_driver reversed(cfg);
    BOOST_CHECK_THROW(reversed.load_data("rb9999").get(), std::invalid_argument);
    cfg.end_date = "20240302";
    backtest_driver empty(cfg);
    BOOST_CHECK_THROW(empty.load_data("rb9999").get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_waits_for_async_bar_callback) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:01,100,101,99,100,10,20\n"
        "2024/2/29 9:02,100,101,99,100,10,20\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    auto strategy = std::make_shared<replay_recording_strategy>(
        std::vector<seastar::sstring>{"rb9999"});
    strategy->defer_callback = true;
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.start_all().get();
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    driver.set_event_bus(&bus);
    driver.start().get();

    auto replay = driver.run();
    BOOST_CHECK(!replay.available());
    const bool entered = wait_until([&] { return strategy->bar_callbacks_entered == 1 || replay.available(); });
    BOOST_CHECK(entered);
    BOOST_CHECK_EQUAL(strategy->bar_callbacks_entered, 1);
    BOOST_CHECK(strategy->bars.empty());
    BOOST_CHECK_THROW(driver.load_data("rb9999").get(), std::runtime_error);
    auto stopping = driver.stop();
    BOOST_CHECK(!stopping.available());
    BOOST_CHECK_THROW(driver.start().get(), std::runtime_error);
    strategy->callback_release.set_value();
    BOOST_CHECK_THROW(replay.get(), std::runtime_error);
    stopping.get();
    BOOST_REQUIRE_EQUAL(strategy->bars.size(), 1);
    driver.stop().get();
    bus.stop().get();
    manager.stop_all().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_propagates_bar_callback_failures) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE", "2024/2/29 9:01,100,101,99,100,10,20\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    auto strategy = std::make_shared<replay_recording_strategy>(
        std::vector<seastar::sstring>{"rb9999"});
    strategy->fail_callback = true;
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.start_all().get();
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    driver.set_event_bus(&bus);
    driver.start().get();

    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    BOOST_REQUIRE(strategy->state() == strategy_state::failed);
    BOOST_CHECK_THROW(bus.stop().get(), std::runtime_error);
    manager.stop_all().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_rejects_unstarted_bus_and_missing_subscribers) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE", "2024/2/29 9:01,100,101,99,100,10,20\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    driver.set_event_bus(&bus);
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    driver.start().get();
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(bus.metrics().enqueued, 0);

    bus.start().get();
    BOOST_CHECK_THROW(driver.run().get(), std::runtime_error);
    BOOST_CHECK_THROW(bus.stop().get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_bus_stop_waits_for_outstanding_drain) {
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    auto strategy = std::make_shared<replay_recording_strategy>(
        std::vector<seastar::sstring>{"rb9999"});
    strategy->defer_callback = true;
    manager.register_strategy(strategy).get();
    manager.init_all().get();
    manager.start_all().get();
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    market_event event;
    event.type = market_event_type::bar;
    event.instrument_id = "rb9999";
    event.calendar_date = "20240229";
    event.close_price = 100.0;
    BOOST_REQUIRE(bus.enqueue(event));
    auto draining = bus.drain();
    BOOST_CHECK(!draining.available());
    BOOST_CHECK(wait_until([&] { return strategy->bar_callbacks_entered == 1 || draining.available(); }));
    BOOST_CHECK_EQUAL(strategy->bar_callbacks_entered, 1);
    auto stopping = bus.stop();
    BOOST_CHECK(!stopping.available());
    BOOST_CHECK_THROW(draining.get(), std::runtime_error);
    strategy->callback_release.set_value();
    stopping.get();
    BOOST_REQUIRE_EQUAL(strategy->bars.size(), 1);
    BOOST_REQUIRE(strategy->bars.front().calendar_date == "20240229");
    manager.stop_all().get();
}

SEASTAR_THREAD_TEST_CASE(xtrader_historical_strict_load_checks_schema) {
    replay_test_directory directory;
    const auto csv = write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:01,100,101,99,100,10,20\n");
    historical_data_manager manager(directory.path());
    std::string warning;
    const auto datasets = manager.scan_datasets(&warning);
    BOOST_REQUIRE_EQUAL(datasets.size(), 1);
    std::vector<historical_bar> bars;

    write_text(csv, "date,high,open,low,close,volume,open_interest\n"
        "2024/2/29 9:01,100,101,99,100,10,20\n");
    BOOST_REQUIRE(!manager.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict));
    BOOST_CHECK(warning.find("header") != std::string::npos);
    write_text(csv, "date,open,high,low,close,volume,open_interest,money,source_symbol\n"
        "2024/2/29 9:01,100,101,99,100,10,20,,\n");
    BOOST_REQUIRE(manager.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict));
    BOOST_REQUIRE_EQUAL(bars.size(), 1);
    write_text(csv, "date,open,high,low,close,volume,open_interest,money,source_symbol\n"
        "2024/2/29 9:01,100,101,99,100,10,20\n");
    BOOST_REQUIRE(!manager.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict));
    BOOST_REQUIRE(bars.empty());
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_rejects_tick_manifest_for_bar_loader) {
    replay_test_directory directory;
    const auto csv = write_dataset(directory.path(), "XSGE",
        "2024/2/29 9:01,100,101,99,100,10,20\n");
    write_text(csv.parent_path() / "dataset-manifest.conf",
        "symbol=rb9999\nexchange=XSGE\ntimeframe=tick\nactive_file=bars.csv\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    BOOST_CHECK_THROW(driver.load_data("rb9999").get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_rejects_unverified_execution_simulator) {
    backtest_config cfg;
    execution_simulator simulator(cfg);
    domain::order_request order;
    order.strategy_id = "offline";
    order.instrument_id = "rb9999";
    order.volume = 1;
    order.price = 100.0;
    market_event event;
    event.instrument_id = "rb9999";
    event.last_price = 99.0;
    double slippage = 0.0;
    BOOST_CHECK_THROW(static_cast<void>(simulator.simulate_fill(order, event, slippage)), std::runtime_error);
    BOOST_CHECK_THROW(static_cast<void>(simulator.simulate_limit_order(order, event)), std::runtime_error);
    backtest_driver driver(cfg);
    BOOST_CHECK_THROW(driver.set_execution_simulator(&simulator), std::logic_error);
}

SEASTAR_THREAD_TEST_CASE(xtrader_replay_delivers_equal_priority_strategies_deterministically) {
    replay_test_directory directory;
    write_dataset(directory.path(), "XSGE", "2024/2/29 9:01,100,101,99,100,10,20\n");
    backtest_config cfg;
    cfg.data_root = directory.path().string();
    backtest_driver driver(cfg);
    driver.load_data("rb9999").get();
    subscription_registry registry;
    strategy_lifecycle_manager manager;
    manager.set_subscription_registry(&registry);
    std::vector<seastar::sstring> observed;
    for (const auto& id : {"beta", "zeta", "alpha"}) {
        auto strategy = std::make_shared<replay_recording_strategy>(
            std::vector<seastar::sstring>{"rb9999"}, id);
        strategy->callback_order = &observed;
        manager.register_strategy(strategy).get();
    }
    manager.init_all().get();
    manager.start_all().get();
    registry.subscribe("zeta", "rb9999", event_kind::bar, 10).get();
    event_bus bus;
    bus.set_strategy_manager(&manager);
    bus.set_subscription_registry(&registry);
    bus.start().get();
    driver.set_event_bus(&bus);
    driver.start().get();
    driver.run().get();
    bus.stop().get();
    manager.stop_all().get();
    BOOST_REQUIRE_EQUAL(observed.size(), 3);
    BOOST_REQUIRE(observed[0] == "zeta");
    BOOST_REQUIRE(observed[1] == "alpha");
    BOOST_REQUIRE(observed[2] == "beta");
}
