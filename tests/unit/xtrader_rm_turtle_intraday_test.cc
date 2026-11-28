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
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
// RM 海龟日内策略行为测试：品种过滤、收盘前禁开、最后两分钟强平、跨日重置、夜盘忽略。
// Source: user request, 2026-11-27.
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>

#include "../../strategies/rm_turtle_intraday.hh"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace seastar::xtrader;
using seastar::sstring;
namespace domain = seastar::xtrader::domain;

struct order_sink {
    std::vector<domain::order_request> orders;
    std::vector<sstring> cancels;
};

strategy_config make_config() {
    strategy_config config;
    config.strategy_id = "rm_intraday";
    config.strategy_name = "RM turtle intraday";
    config.strategy_type = "intraday";
    config.subscribed_instruments = {"RM2601"};
    config.max_queue_depth = 64;
    return config;
}

strategy_context make_context(order_sink& sink) {
    strategy_context ctx;
    ctx.strategy_id = "rm_intraday";
    ctx.trading_day = "20250918";
    ctx.spec.instrument_id = "RM2601";
    ctx.spec.exchange_id = "CZCE";
    ctx.spec.price_tick = 1.0;
    ctx.spec.volume_multiple = 10;
    ctx.submit_order = [&sink] (const domain::order_request& request) {
        sink.orders.push_back(request);
        return seastar::make_ready_future<domain::order_status>(domain::order_status::accepted);
    };
    ctx.cancel_order = [&sink] (const sstring& order_sys_id) {
        sink.cancels.push_back(order_sys_id);
        return seastar::make_ready_future<domain::order_status>(domain::order_status::canceled);
    };
    ctx.subscribe_market_data = [] { return seastar::make_ready_future<>(); };
    ctx.subscribe_bar_data = [] { return seastar::make_ready_future<>(); };
    ctx.query_position = [] { return domain::position_view{}; };
    return ctx;
}

// 模拟 CTP 的 OnRtnOrder + OnRtnTrade：成交回报驱动策略持仓。
void fill_last_order(rm_turtle_intraday& strategy, strategy_context& ctx, order_sink& sink,
                     double price) {
    const auto request = sink.orders.back();
    const std::string id_text = "sim-order-1-" + std::to_string(sink.orders.size());
    const sstring sys_id(id_text.c_str());

    domain::order update{};
    update.strategy_id = request.strategy_id;
    update.order_ref = sys_id;
    update.order_sys_id = sys_id;
    update.exchange_id = ctx.spec.exchange_id;
    update.instrument_id = request.instrument_id;
    update.direction = request.direction;
    update.offset = request.offset;
    update.volume_original = request.volume;
    update.volume_traded = request.volume;
    update.status = domain::order_status::filled;
    update.trading_day = ctx.trading_day;
    strategy.on_order_update(ctx, update).get();

    domain::trade_report trade{};
    trade.trade_id = sys_id;
    trade.order_ref = sys_id;
    trade.order_sys_id = sys_id;
    trade.exchange_id = ctx.spec.exchange_id;
    trade.instrument_id = request.instrument_id;
    trade.direction = request.direction;
    trade.offset = request.offset;
    trade.price = price;
    trade.volume = request.volume;
    trade.trading_day = ctx.trading_day;
    trade.strategy_id = request.strategy_id;
    strategy.on_trade_update(ctx, trade).get();
}

// 模拟委托被撤/拒：没有成交，策略应释放活动委托。
void cancel_last_order(rm_turtle_intraday& strategy, strategy_context& ctx, order_sink& sink) {
    const auto request = sink.orders.back();
    const std::string id_text = "sim-order-1-" + std::to_string(sink.orders.size());
    domain::order update{};
    update.strategy_id = request.strategy_id;
    update.order_ref = sstring(id_text.c_str());
    update.order_sys_id = sstring(id_text.c_str());
    update.exchange_id = ctx.spec.exchange_id;
    update.instrument_id = request.instrument_id;
    update.direction = request.direction;
    update.offset = request.offset;
    update.volume_original = request.volume;
    update.volume_traded = 0;
    update.status = domain::order_status::canceled;
    update.trading_day = ctx.trading_day;
    strategy.on_order_update(ctx, update).get();
}

domain::market_data make_tick(const std::string& time, double price, int last_volume = 10) {
    domain::market_data tick;
    tick.instrument_id = "RM2601";
    tick.update_time = time;
    tick.update_millisec = 0;
    tick.last_price = price;
    tick.last_volume = last_volume;
    tick.open_interest = 1000;
    tick.trading_day = "20250918";
    return tick;
}

std::string hhmmss(int hour, int minute, int second) {
    auto text = std::string();
    text += static_cast<char>('0' + hour / 10);
    text += static_cast<char>('0' + hour % 10);
    text += ':';
    text += static_cast<char>('0' + minute / 10);
    text += static_cast<char>('0' + minute % 10);
    text += ':';
    text += static_cast<char>('0' + second / 10);
    text += static_cast<char>('0' + second % 10);
    return text;
}

std::string hhmm(int hour, int minute) {
    std::string text;
    text += static_cast<char>('0' + hour / 10);
    text += static_cast<char>('0' + hour % 10);
    text += ':';
    text += static_cast<char>('0' + minute / 10);
    text += static_cast<char>('0' + minute % 10);
    text += ":00";
    return text;
}

domain::bar_data make_bar(const std::string& timestamp, double open, double high, double low,
                          double close, const char* calendar_date = "20250918",
                          const char* instrument = "RM2601", const char* exchange = "CZCE") {
    domain::bar_data bar;
    bar.instrument_id = instrument;
    bar.exchange_id = exchange;
    bar.calendar_date = calendar_date;
    bar.timestamp = timestamp;
    bar.timeframe = "1m";
    bar.open_price = open;
    bar.high_price = high;
    bar.low_price = low;
    bar.close_price = close;
    bar.volume = 100;
    bar.open_interest = 1000;
    return bar;
}

} // namespace

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_accepts_only_rm_on_allowed_exchanges) {
    rm_turtle_intraday strategy(make_config(), {});

    BOOST_REQUIRE(strategy.accepts_instrument("RM2601", "CZCE"));
    BOOST_REQUIRE(strategy.accepts_instrument("RM2601", "XZCE"));
    BOOST_REQUIRE(strategy.accepts_instrument("rm601", "czce"));   // 大小写不敏感
    BOOST_REQUIRE(!strategy.accepts_instrument("IF2601", "CFFEX")); // 非 RM
    BOOST_REQUIRE(!strategy.accepts_instrument("RM2601", "SHFE"));  // 交易所不在白名单
    BOOST_REQUIRE(!strategy.accepts_instrument("RM", "CZCE"));      // 缺合约月份
    BOOST_REQUIRE(!strategy.accepts_instrument("RM26A1", "CZCE"));  // 含非数字
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_rejects_invalid_configuration) {
    // 强制平仓窗口必须晚于禁开窗口
    BOOST_REQUIRE_THROW((rm_turtle_intraday(make_config(),
        {.no_entry_before_close_minutes = 2, .flatten_before_close_minutes = 5})),
        std::invalid_argument);
    // 周期非法
    BOOST_REQUIRE_THROW((rm_turtle_intraday(make_config(), {.entry_period = 0})),
        std::invalid_argument);
    // 手数非法
    BOOST_REQUIRE_THROW((rm_turtle_intraday(make_config(), {.volume = 0})),
        std::invalid_argument);
    // 订阅了非 RM 合约
    auto config = make_config();
    config.subscribed_instruments = {"IF2601"};
    BOOST_REQUIRE_THROW(rm_turtle_intraday(config, {}), std::invalid_argument);
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_ignores_night_and_foreign_bars) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 3, .exit_period = 2, .atr_period = 2});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    // 夜盘 bar：不处理
    strategy.on_bar(ctx, make_bar("21:30:00", 100, 120, 100, 115)).get();
    BOOST_REQUIRE_EQUAL(strategy.day_bars(), 0u);
    BOOST_REQUIRE(sink.orders.empty());

    // 非 RM 合约：忽略并计数
    strategy.on_bar(ctx, make_bar("09:05:00", 100, 120, 100, 115, "20250918", "IF2601", "CFFEX")).get();
    BOOST_REQUIRE_EQUAL(strategy.day_bars(), 0u);
    BOOST_REQUIRE_EQUAL(strategy.rejected_bars(), 1u);
    BOOST_REQUIRE(sink.orders.empty());
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_opens_on_breakout_within_entry_window) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    double price = 100.0;
    for (int i = 1; i <= 10; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), price, price + 1, price - 1, price)).get();
    }
    BOOST_REQUIRE(sink.orders.empty());

    // 09:15 收盘价突破过去 5 根最高价 → 开多
    strategy.on_bar(ctx, make_bar(hhmm(9, 15), price, price + 8, price, price + 6)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);
    BOOST_REQUIRE(sink.orders.front().offset == domain::offset_flag::open);
    BOOST_REQUIRE(sink.orders.front().direction == domain::side::buy);
    BOOST_REQUIRE(sink.orders.front().instrument_id == "RM2601");
    // 下单不等于成交：只有成交回报才更新策略持仓。
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 0);
    fill_last_order(strategy, ctx, sink, price + 6);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 1);
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_blocks_entry_in_last_five_minutes) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    double price = 100.0;
    for (int i = 1; i <= 30; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), price, price + 1, price - 1, price)).get();
    }
    // 14:56 大幅突破，但已进入收盘前 5 分钟禁开窗口 → 不得开仓
    strategy.on_bar(ctx, make_bar(hhmm(14, 56), price, price + 40, price, price + 30)).get();
    BOOST_REQUIRE(sink.orders.empty());
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 0);
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_flattens_in_last_two_minutes) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    double price = 100.0;
    for (int i = 1; i <= 10; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), price, price + 1, price - 1, price)).get();
    }
    strategy.on_bar(ctx, make_bar(hhmm(9, 15), price, price + 8, price, price + 6)).get();
    fill_last_order(strategy, ctx, sink, price + 6);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 1);
    const auto opened = sink.orders.size();

    // 14:58 进入最后 2 分钟 → 无论是否触发止损/反转都必须平仓
    strategy.on_bar(ctx, make_bar(hhmm(14, 58), price + 6, price + 6, price + 4, price + 5)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), opened + 1);
    BOOST_REQUIRE(sink.orders.back().offset == domain::offset_flag::close);
    BOOST_REQUIRE(sink.orders.back().direction == domain::side::sell);
    // 平仓也要等成交回报，不能在下单瞬间清零。
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 1);
    fill_last_order(strategy, ctx, sink, price + 5);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 0);

    // 收盘 bar 不再产生新订单（已空仓，且禁开）
    strategy.on_bar(ctx, make_bar("15:00:00", price + 5, price + 6, price + 4, price + 5)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), opened + 1);
}

SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_resets_state_on_new_trading_day) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    for (int i = 1; i <= 5; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), 100, 101, 99, 100, "20250918")).get();
    }
    BOOST_REQUIRE_EQUAL(strategy.day_bars(), 5u);
    BOOST_REQUIRE(strategy.current_day() == "20250918");

    // 次日首根 bar：日内状态重置，指标不可复用
    strategy.on_bar(ctx, make_bar(hhmm(9, 1), 100, 140, 100, 130, "20250919")).get();
    BOOST_REQUIRE_EQUAL(strategy.day_bars(), 1u);
    BOOST_REQUIRE(strategy.current_day() == "20250919");
    BOOST_REQUIRE(sink.orders.empty());  // 指标未就绪，不能凭一根 bar 开仓
}

// 复现并防回归：开仓限价未成交时，策略不得按“想象中的持仓”发平仓单。
SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_does_not_close_unfilled_entry) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {
        .entry_period = 5, .exit_period = 3, .atr_period = 3, .allow_short = false});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    double price = 100.0;
    for (int i = 1; i <= 10; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), price, price + 1, price - 1, price)).get();
    }
    strategy.on_bar(ctx, make_bar(hhmm(9, 15), price, price + 8, price, price + 6)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);

    // 开仓被撤（未成交），策略持仓仍为 0；后续下跌不应产生平仓单。
    cancel_last_order(strategy, ctx, sink);
    BOOST_REQUIRE(!strategy.has_working_order());
    strategy.on_bar(ctx, make_bar(hhmm(9, 16), 106, 106, 90, 90)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);
    BOOST_REQUIRE(sink.orders.back().offset == domain::offset_flag::open);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 0);
}

// 未成交委托超过 N 根 bar 必须撤单，避免实盘挂单长期滞留。
SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_cancels_stale_order) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {
        .entry_period = 5, .exit_period = 3, .atr_period = 3, .order_timeout_bars = 1});
    auto ctx = make_context(sink);
    strategy.on_init(ctx).get();

    double price = 100.0;
    for (int i = 1; i <= 10; ++i) {
        strategy.on_bar(ctx, make_bar(hhmm(9, i), price, price + 1, price - 1, price)).get();
    }
    strategy.on_bar(ctx, make_bar(hhmm(9, 15), price, price + 8, price, price + 6)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);

    // 模拟 OnRtnOrder 绑定 OrderSysID，但不成交。
    const auto request = sink.orders.back();
    domain::order accepted{};
    accepted.strategy_id = request.strategy_id;
    accepted.order_ref = "sim-order-1-1";
    accepted.order_sys_id = "sim-order-1-1";
    accepted.exchange_id = ctx.spec.exchange_id;
    accepted.instrument_id = request.instrument_id;
    accepted.direction = request.direction;
    accepted.offset = request.offset;
    accepted.volume_original = request.volume;
    accepted.volume_traded = 0;
    accepted.status = domain::order_status::accepted;
    accepted.trading_day = ctx.trading_day;
    strategy.on_order_update(ctx, accepted).get();

    strategy.on_bar(ctx, make_bar(hhmm(9, 16), price + 6, price + 6, price + 5, price + 5)).get();
    BOOST_REQUIRE_EQUAL(sink.cancels.size(), 1u);
    BOOST_REQUIRE(sink.cancels.front() == "sim-order-1-1");
}

// 启动时已有持仓（重启/隔夜）：先平仓，不得在指标未就绪时继续持有。
SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_flattens_initial_position) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    ctx.query_position = [] {
        domain::position_view position;
        position.long_today = 1;
        return position;
    };
    strategy.on_init(ctx).get();

    strategy.on_bar(ctx, make_bar(hhmm(9, 1), 100, 100, 100, 100)).get();
    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);
    BOOST_REQUIRE(sink.orders.front().offset == domain::offset_flag::close);
    BOOST_REQUIRE(sink.orders.front().direction == domain::side::sell);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 1);
    fill_last_order(strategy, ctx, sink, 100);
    BOOST_REQUIRE_EQUAL(strategy.position_volume(), 0);
}

// 实盘 tick 行情：策略自行聚合 1 分钟 bar，和回测走同一套 process_bar。
SEASTAR_THREAD_TEST_CASE(rm_turtle_intraday_aggregates_ticks_for_live) {
    order_sink sink;
    rm_turtle_intraday strategy(make_config(), {.entry_period = 5, .exit_period = 3, .atr_period = 3});
    auto ctx = make_context(sink);
    ctx.tick_market_data = true;
    strategy.on_init(ctx).get();

    for (int minute = 1; minute <= 6; ++minute) {
        strategy.on_market_data(ctx, make_tick(hhmmss(9, minute, 30), 100.0)).get();
    }
    // 右标签聚合：09:07:30 的 tick 归入 09:08 bar，需要下一笔行情来结算 09:08。
    strategy.on_market_data(ctx, make_tick(hhmmss(9, 7, 30), 106.0)).get();
    strategy.on_market_data(ctx, make_tick(hhmmss(9, 8, 30), 106.0)).get();

    BOOST_REQUIRE_EQUAL(sink.orders.size(), 1u);
    BOOST_REQUIRE(sink.orders.front().offset == domain::offset_flag::open);
    BOOST_REQUIRE(sink.orders.front().direction == domain::side::buy);
}
