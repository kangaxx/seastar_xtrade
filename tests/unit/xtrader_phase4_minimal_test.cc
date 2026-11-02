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

#include <filesystem>
#include <fstream>

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
    seastar::sstring stop_reason;
    int market_data_count = 0;

private:
    bool _submit_on_init = false;
};

} // namespace

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

SEASTAR_THREAD_TEST_CASE(xtrader_phase4_backtest_driver_loads_manifest_data_and_dispatches_raw_md) {
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

    auto strategy = std::make_shared<phase4_test_strategy>(false);
    manager.register_strategy(strategy).get();
    manager.init_all().get();
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
    driver.run().get();

    seastar::sleep(std::chrono::milliseconds(20)).get();

    bus.stop().get();
    manager.stop_all().get();

    BOOST_REQUIRE_GE(strategy->market_data_count, 2);
    BOOST_REQUIRE(strategy->last_market_data.instrument_id == "rb9999");
    BOOST_REQUIRE(strategy->last_market_data.update_time == "09:02:00");
    BOOST_REQUIRE(strategy->last_market_data.trading_day == "20090327");
    BOOST_REQUIRE_EQUAL(strategy->last_market_data.last_price, 3582.0);

    fs::remove_all(temp_root.parent_path().parent_path().parent_path().parent_path());
}