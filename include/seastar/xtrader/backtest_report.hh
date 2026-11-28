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
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#pragma once

#include <seastar/xtrader/domain_types.hh>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace seastar::xtrader {

// 回测结果统计：逐笔成交（L2）、开平配对（L3）、逐日权益（L4）、汇总指标（L5）。
// 金额统一以离线定点单位记录，写出时按 money_units_per_currency 换算。
// Source: user request, 2026-11-28.

struct backtest_trade_record {
    std::string trade_id;
    std::string order_sys_id;
    std::string strategy_id;
    std::string exchange_id;
    std::string instrument_id;
    std::string trading_day;
    std::string calendar_date;
    std::string trade_time;
    int64_t timestamp_nanos = 0;
    domain::side direction = domain::side::buy;
    domain::offset_flag offset = domain::offset_flag::open;
    domain::hedge_flag hedge = domain::hedge_flag::speculation;
    double price = 0.0;
    int volume = 0;
    int64_t commission_units = 0;
    int64_t margin_change_units = 0;
    int64_t close_profit_units = 0;
    std::string fill_source;
    bool is_approximate = false;
    uint64_t market_sequence = 0;
};

struct backtest_closed_trade {
    std::string close_trade_id;
    std::string open_trade_id;
    std::string instrument_id;
    std::string position_side;   // long / short
    std::string open_time;       // YYYY-MM-DD HH:MM:SS
    std::string close_time;
    double open_price = 0.0;
    double close_price = 0.0;
    int volume = 0;
    double gross_pnl = 0.0;
    double commission = 0.0;
    double net_pnl = 0.0;
    double holding_minutes = 0.0;
    std::string exit_reason;
};

struct backtest_daily_equity {
    std::string trading_day;
    uint64_t bars = 0;
    uint64_t signals = 0;
    uint64_t orders = 0;
    uint64_t fills = 0;
    uint64_t canceled = 0;
    int64_t realized_pnl_units = 0;
    int64_t commission_units = 0;
    int64_t balance_units = 0;
    int64_t margin_units = 0;
    int64_t unrealized_pnl_units = 0;
    double equity = 0.0;
    double peak_equity = 0.0;
    double drawdown = 0.0;
    double drawdown_pct = 0.0;
    int close_position = 0;
    std::string status{"ok"};    // ok / failed
    std::string error;
};

struct backtest_run_meta {
    uint64_t run_id = 0;
    std::string created_at;
    std::string status{"completed"};    // completed / incomplete / failed
    std::string strategy_id;
    std::string strategy_params;
    std::string exchange_id;
    std::string instrument_id;
    std::string timeframe;
    std::string start_date;
    std::string end_date;
    std::string data_file;
    int64_t initial_balance_units = 0;
    int64_t money_units_per_currency = 1;
    int64_t tick_value_per_lot = 1;
    int volume_multiple = 1;
    double price_tick = 0.0;
    double margin_rate = 0.0;
    double fee_per_lot_open = 0.0;
    double fee_per_lot_close = 0.0;
    double slippage_ticks = 0.0;
    std::string matching_mode{"next_bar_open"};
    std::string close_rule{"today_first"};
    std::string cost_allocation{"fifo"};
    int failed_days = 0;
    std::vector<std::string> warnings;
};

struct backtest_summary {
    // 区间
    uint64_t trading_days = 0;
    uint64_t total_bars = 0;
    // 资金
    double initial_balance = 0.0;
    double final_balance = 0.0;
    double net_pnl = 0.0;
    double return_pct = 0.0;
    double final_margin = 0.0;
    // 成本
    double total_commission = 0.0;
    double total_slippage = 0.0;
    // 交易
    uint64_t signals = 0;
    uint64_t orders = 0;
    uint64_t fills = 0;
    uint64_t canceled = 0;
    double fill_rate = 0.0;
    // 平仓
    uint64_t closed_trades = 0;
    uint64_t wins = 0;
    uint64_t losses = 0;
    uint64_t breakeven = 0;
    double win_rate = 0.0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double payoff_ratio = 0.0;
    double profit_factor = 0.0;
    double expectancy = 0.0;
    double max_single_win = 0.0;
    double max_single_loss = 0.0;
    uint64_t max_consecutive_wins = 0;
    uint64_t max_consecutive_losses = 0;
    double avg_holding_minutes = 0.0;
    double trades_per_day = 0.0;
    // 风险
    double max_drawdown = 0.0;
    double max_drawdown_pct = 0.0;
    std::string max_drawdown_peak_day;
    std::string max_drawdown_trough_day;
    std::string max_drawdown_recovery_day;
    double max_daily_loss = 0.0;
    double daily_volatility = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    double calmar = 0.0;
    double annualized_return_pct = 0.0;
    // 校验
    uint64_t flat_violations = 0;
    std::vector<std::string> warnings;
};

// 按 FIFO 把逐笔成交配对成平仓交易；配对异常写入 warnings，不伪造数据。
std::vector<backtest_closed_trade> build_closed_trades(
        const std::vector<backtest_trade_record>& trades,
        int64_t tick_value_per_lot, double price_tick, int64_t money_units_per_currency,
        std::vector<std::string>& warnings);

// 计算汇总指标，并回填逐日权益曲线（peak/drawdown）。
backtest_summary summarize_backtest(
        const backtest_run_meta& meta,
        std::vector<backtest_daily_equity>& daily,
        const std::vector<backtest_trade_record>& trades,
        const std::vector<backtest_closed_trade>& closed);

// 报告目录：<root>/<exchange>_<instrument>/<strategy>/<run_id>_<timestamp>/
std::filesystem::path backtest_report_directory(
        const std::filesystem::path& root,
        const std::string& exchange_id, const std::string& instrument_id,
        const std::string& strategy_id, uint64_t run_id, const std::string& timestamp);

// 写出 report.json / summary.csv / trades.csv / closed_trades.csv / equity.csv / report.md。
std::filesystem::path write_backtest_report(
        const std::filesystem::path& directory,
        const backtest_run_meta& meta,
        const backtest_summary& summary,
        const std::vector<backtest_trade_record>& trades,
        const std::vector<backtest_closed_trade>& closed,
        const std::vector<backtest_daily_equity>& daily);

// 供 CLI 打印的一行汇总。
std::string format_backtest_summary_text(
        const backtest_run_meta& meta, const backtest_summary& summary);

} // namespace seastar::xtrader
