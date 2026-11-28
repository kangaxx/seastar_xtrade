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
#include <seastar/xtrader/backtest_report.hh>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace seastar::xtrader {
namespace {

// 统计与报告写出工具。
// Source: user request, 2026-11-28.
constexpr double TRADING_DAYS_PER_YEAR = 252.0;
constexpr double PNL_EPSILON = 1e-9;

std::string side_text(domain::side value) {
    return value == domain::side::buy ? "buy" : "sell";
}

std::string offset_text(domain::offset_flag value) {
    switch (value) {
    case domain::offset_flag::open: return "open";
    case domain::offset_flag::close: return "close";
    case domain::offset_flag::close_today: return "close_today";
    case domain::offset_flag::close_yesterday: return "close_yesterday";
    }
    return "unknown";
}

std::string hedge_text(domain::hedge_flag value) {
    switch (value) {
    case domain::hedge_flag::speculation: return "speculation";
    case domain::hedge_flag::hedge: return "hedge";
    case domain::hedge_flag::arbitrage: return "arbitrage";
    }
    return "unknown";
}

bool all_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [] (char ch) { return ch >= '0' && ch <= '9'; });
}

std::string pretty_day(const std::string& day) {
    if (day == "start") {
        return "期初";
    }
    if (day.size() == 8 && all_digits(day)) {
        return day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2);
    }
    return day;
}

std::string pretty_time(const std::string& calendar_date, const std::string& time) {
    return pretty_day(calendar_date) + " " + time;
}

double units_to_currency(int64_t units, int64_t per_currency) {
    if (per_currency <= 0) {
        throw std::invalid_argument("money_units_per_currency must be positive");
    }
    return static_cast<double>(units) / static_cast<double>(per_currency);
}

std::string fixed_number(double value, int precision) {
    if (!std::isfinite(value)) {
        return "0";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string money_text(double value) {
    return fixed_number(value, 2);
}

std::string percent_text(double value) {
    return fixed_number(value, 4);
}

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch);
                result += escaped.str();
            } else {
                result += static_cast<char>(ch);
            }
            break;
        }
    }
    return result;
}

std::string json_number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream out;
    out << std::setprecision(12) << value;
    auto text = out.str();
    if (text == "-0") {
        text = "0";
    }
    return text;
}

std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string result = "\"";
    for (char ch : value) {
        if (ch == '"') {
            result += "\"\"";
        } else {
            result += ch;
        }
    }
    result += '"';
    return result;
}

void write_csv_row(std::ostream& out, const std::vector<std::string>& fields) {
    for (size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << csv_escape(fields[index]);
    }
    out << '\n';
}

void json_key(std::ostream& out, int indent, const char* key) {
    out << std::string(static_cast<size_t>(indent), ' ') << '"' << key << "\": ";
}

void json_string_field(std::ostream& out, int indent, const char* key,
                       const std::string& value, bool last = false) {
    json_key(out, indent, key);
    out << '"' << json_escape(value) << '"' << (last ? "\n" : ",\n");
}

void json_int_field(std::ostream& out, int indent, const char* key,
                    int64_t value, bool last = false) {
    json_key(out, indent, key);
    out << value << (last ? "\n" : ",\n");
}

void json_uint_field(std::ostream& out, int indent, const char* key,
                     uint64_t value, bool last = false) {
    json_key(out, indent, key);
    out << value << (last ? "\n" : ",\n");
}

void json_double_field(std::ostream& out, int indent, const char* key,
                       double value, bool last = false) {
    json_key(out, indent, key);
    out << json_number(value) << (last ? "\n" : ",\n");
}

void json_bool_field(std::ostream& out, int indent, const char* key,
                     bool value, bool last = false) {
    json_key(out, indent, key);
    out << (value ? "true" : "false") << (last ? "\n" : ",\n");
}

void json_string_array(std::ostream& out, int indent, const char* key,
                       const std::vector<std::string>& values, bool last = false) {
    json_key(out, indent, key);
    out << '[';
    if (values.empty()) {
        out << ']' << (last ? "\n" : ",\n");
        return;
    }
    out << '\n';
    for (size_t index = 0; index < values.size(); ++index) {
        out << std::string(static_cast<size_t>(indent + 2), ' ')
            << '"' << json_escape(values[index]) << '"'
            << (index + 1 == values.size() ? "\n" : ",\n");
    }
    out << std::string(static_cast<size_t>(indent), ' ') << ']'
        << (last ? "\n" : ",\n");
}

struct open_lot {
    std::string trade_id;
    std::string open_time;
    int64_t open_nanos = 0;
    double price = 0.0;
    int remaining = 0;
    double commission_per_volume = 0.0;
};

} // namespace

std::vector<backtest_closed_trade> build_closed_trades(
        const std::vector<backtest_trade_record>& trades,
        int64_t tick_value_per_lot, double price_tick, int64_t money_units_per_currency,
        std::vector<std::string>& warnings) {
    if (tick_value_per_lot <= 0 || !std::isfinite(price_tick) || price_tick <= 0.0) {
        throw std::invalid_argument("closed trade pairing requires positive tick value and price tick");
    }
    std::deque<open_lot> long_lots;
    std::deque<open_lot> short_lots;
    std::vector<backtest_closed_trade> closed;

    for (const auto& trade : trades) {
        if (trade.volume <= 0) {
            continue;
        }
        const double commission_per_volume =
            units_to_currency(trade.commission_units, money_units_per_currency)
            / static_cast<double>(trade.volume);
        if (trade.offset == domain::offset_flag::open) {
            auto& lots = trade.direction == domain::side::buy ? long_lots : short_lots;
            lots.push_back({
                .trade_id = trade.trade_id,
                .open_time = pretty_time(trade.calendar_date, trade.trade_time),
                .open_nanos = trade.timestamp_nanos,
                .price = trade.price,
                .remaining = trade.volume,
                .commission_per_volume = commission_per_volume,
            });
            continue;
        }

        // 卖出平多、买入平空；按 FIFO 消耗最早的开仓批次。
        const bool closing_long = trade.direction == domain::side::sell;
        auto& lots = closing_long ? long_lots : short_lots;
        int remaining = trade.volume;
        while (remaining > 0) {
            if (lots.empty()) {
                warnings.push_back("平仓成交 " + trade.trade_id + "（" + trade.trading_day
                    + "）找不到对应开仓批次，配对已停止");
                break;
            }
            auto& lot = lots.front();
            const int portion = std::min(remaining, lot.remaining);
            const double price_difference = closing_long
                ? trade.price - lot.price : lot.price - trade.price;
            const double gross_units = price_difference / price_tick
                * static_cast<double>(tick_value_per_lot) * static_cast<double>(portion);
            const double gross = units_to_currency(
                static_cast<int64_t>(std::llround(gross_units)), money_units_per_currency);
            const double commission = (lot.commission_per_volume + commission_per_volume)
                * static_cast<double>(portion);

            backtest_closed_trade row;
            row.close_trade_id = trade.trade_id;
            row.open_trade_id = lot.trade_id;
            row.instrument_id = trade.instrument_id;
            row.position_side = closing_long ? "long" : "short";
            row.open_time = lot.open_time;
            row.close_time = pretty_time(trade.calendar_date, trade.trade_time);
            // 策略层暂未提供原因标签，平仓原因用委托的开平标志表示。
            row.open_price = lot.price;
            row.close_price = trade.price;
            row.volume = portion;
            row.gross_pnl = gross;
            row.commission = commission;
            row.net_pnl = gross - commission;
            row.holding_minutes = trade.timestamp_nanos >= lot.open_nanos
                ? static_cast<double>(trade.timestamp_nanos - lot.open_nanos) / 60'000'000'000.0
                : 0.0;
            row.exit_reason = offset_text(trade.offset);
            closed.push_back(std::move(row));

            remaining -= portion;
            lot.remaining -= portion;
            if (lot.remaining == 0) {
                lots.pop_front();
            }
        }
    }

    int leftover = 0;
    for (const auto& lot : long_lots) {
        leftover += lot.remaining;
    }
    for (const auto& lot : short_lots) {
        leftover += lot.remaining;
    }
    if (leftover > 0) {
        warnings.push_back("回测结束时仍有 " + std::to_string(leftover) + " 手未配对开仓");
    }
    return closed;
}

backtest_summary summarize_backtest(
        const backtest_run_meta& meta,
        std::vector<backtest_daily_equity>& daily,
        const std::vector<backtest_trade_record>& trades,
        const std::vector<backtest_closed_trade>& closed) {
    backtest_summary summary;
    summary.initial_balance = units_to_currency(
        meta.initial_balance_units, meta.money_units_per_currency);
    summary.total_slippage = 0.0;

    for (const auto& row : daily) {
        summary.total_bars += row.bars;
        summary.signals += row.signals;
        summary.orders += row.orders;
        summary.fills += row.fills;
        summary.canceled += row.canceled;
        summary.total_commission += units_to_currency(
            row.commission_units, meta.money_units_per_currency);
        if (row.status == "ok") {
            ++summary.trading_days;
        }
        if (row.close_position != 0 && row.status == "ok") {
            ++summary.flat_violations;
        }
    }
    summary.final_balance = daily.empty()
        ? summary.initial_balance
        : units_to_currency(daily.back().balance_units, meta.money_units_per_currency);
    summary.net_pnl = summary.final_balance - summary.initial_balance;
    summary.return_pct = summary.initial_balance > 0.0
        ? summary.net_pnl / summary.initial_balance * 100.0 : 0.0;
    summary.final_margin = daily.empty() ? 0.0
        : units_to_currency(daily.back().margin_units, meta.money_units_per_currency);

    // 逐日权益曲线与最大回撤。
    double peak = summary.initial_balance;
    std::string peak_day = "start";
    double peak_at_trough = 0.0;
    size_t trough_index = 0;
    bool has_drawdown = false;
    for (size_t index = 0; index < daily.size(); ++index) {
        auto& row = daily[index];
        const double equity = units_to_currency(
            row.balance_units + row.unrealized_pnl_units, meta.money_units_per_currency);
        row.equity = equity;
        if (equity > peak) {
            peak = equity;
            peak_day = row.trading_day;
        }
        row.peak_equity = peak;
        row.drawdown = peak - equity;
        row.drawdown_pct = peak > 0.0 ? row.drawdown / peak * 100.0 : 0.0;
        if (row.drawdown > summary.max_drawdown + PNL_EPSILON) {
            summary.max_drawdown = row.drawdown;
            summary.max_drawdown_pct = row.drawdown_pct;
            summary.max_drawdown_peak_day = peak_day;
            summary.max_drawdown_trough_day = row.trading_day;
            peak_at_trough = peak;
            trough_index = index;
            has_drawdown = true;
        }
    }
    if (has_drawdown) {
        for (size_t index = trough_index + 1; index < daily.size(); ++index) {
            if (daily[index].equity + PNL_EPSILON >= peak_at_trough) {
                summary.max_drawdown_recovery_day = daily[index].trading_day;
                break;
            }
        }
    }

    // 日收益序列统计。
    std::vector<double> returns;
    returns.reserve(daily.size());
    double previous = summary.initial_balance;
    for (const auto& row : daily) {
        if (previous > 0.0) {
            returns.push_back((row.equity - previous) / previous);
        }
        previous = row.equity;
    }
    double mean_return = 0.0;
    if (!returns.empty()) {
        for (double value : returns) {
            mean_return += value;
        }
        mean_return /= static_cast<double>(returns.size());
    }
    double variance = 0.0;
    double downside = 0.0;
    if (!returns.empty()) {
        for (double value : returns) {
            const double centered = value - mean_return;
            variance += centered * centered;
            if (value < 0.0) {
                downside += value * value;
            }
        }
        const double sample_count = returns.size() > 1
            ? static_cast<double>(returns.size() - 1) : 1.0;
        variance /= sample_count;
        downside /= static_cast<double>(returns.size());
    }
    const double deviation = std::sqrt(std::max(variance, 0.0));
    const double downside_deviation = std::sqrt(std::max(downside, 0.0));
    summary.daily_volatility = deviation * std::sqrt(TRADING_DAYS_PER_YEAR) * 100.0;
    summary.sharpe = deviation > 0.0
        ? mean_return / deviation * std::sqrt(TRADING_DAYS_PER_YEAR) : 0.0;
    summary.sortino = downside_deviation > 0.0
        ? mean_return / downside_deviation * std::sqrt(TRADING_DAYS_PER_YEAR) : 0.0;
    if (summary.trading_days > 0 && summary.initial_balance > 0.0 && summary.final_balance > 0.0) {
        const double growth = summary.final_balance / summary.initial_balance;
        const double annualized = std::pow(
            growth, TRADING_DAYS_PER_YEAR / static_cast<double>(summary.trading_days)) - 1.0;
        summary.annualized_return_pct = annualized * 100.0;
    }
    summary.calmar = summary.max_drawdown_pct > 0.0
        ? summary.annualized_return_pct / summary.max_drawdown_pct : 0.0;

    // 平仓交易统计。
    double closed_net = 0.0;
    double holding_total = 0.0;
    uint64_t win_streak = 0;
    uint64_t loss_streak = 0;
    summary.max_daily_loss = 0.0;
    for (const auto& row : daily) {
        if (row.status != "ok") {
            continue;
        }
        const double realized = units_to_currency(
            row.realized_pnl_units, meta.money_units_per_currency);
        summary.max_daily_loss = std::min(summary.max_daily_loss, realized);
    }
    for (const auto& trade : closed) {
        closed_net += trade.net_pnl;
        holding_total += trade.holding_minutes;
        ++summary.closed_trades;
        if (trade.net_pnl > PNL_EPSILON) {
            ++summary.wins;
            summary.gross_profit += trade.net_pnl;
            summary.max_single_win = std::max(summary.max_single_win, trade.net_pnl);
            ++win_streak;
            loss_streak = 0;
        } else if (trade.net_pnl < -PNL_EPSILON) {
            ++summary.losses;
            summary.gross_loss += -trade.net_pnl;
            summary.max_single_loss = std::min(summary.max_single_loss, trade.net_pnl);
            ++loss_streak;
            win_streak = 0;
        } else {
            ++summary.breakeven;
            win_streak = 0;
            loss_streak = 0;
        }
        summary.max_consecutive_wins = std::max(summary.max_consecutive_wins, win_streak);
        summary.max_consecutive_losses = std::max(summary.max_consecutive_losses, loss_streak);
    }
    if (summary.closed_trades > 0) {
        summary.win_rate = static_cast<double>(summary.wins)
            / static_cast<double>(summary.closed_trades) * 100.0;
        summary.avg_win = summary.wins > 0
            ? summary.gross_profit / static_cast<double>(summary.wins) : 0.0;
        summary.avg_loss = summary.losses > 0
            ? summary.gross_loss / static_cast<double>(summary.losses) : 0.0;
        summary.payoff_ratio = summary.avg_loss > 0.0
            ? summary.avg_win / summary.avg_loss : 0.0;
        summary.profit_factor = summary.gross_loss > 0.0
            ? summary.gross_profit / summary.gross_loss : 0.0;
        summary.expectancy = closed_net / static_cast<double>(summary.closed_trades);
        summary.avg_holding_minutes = holding_total / static_cast<double>(summary.closed_trades);
    }
    if (summary.trading_days > 0) {
        summary.trades_per_day = static_cast<double>(summary.closed_trades)
            / static_cast<double>(summary.trading_days);
    }
    summary.fill_rate = summary.orders > 0
        ? static_cast<double>(summary.fills) / static_cast<double>(summary.orders) * 100.0 : 0.0;

    // 结果自检：不完整或对不上时必须显式告警。
    double trade_close_profit = 0.0;
    double trade_commission = 0.0;
    for (const auto& trade : trades) {
        trade_close_profit += units_to_currency(
            trade.close_profit_units, meta.money_units_per_currency);
        trade_commission += units_to_currency(
            trade.commission_units, meta.money_units_per_currency);
    }
    if (trades.size() != summary.fills) {
        summary.warnings.push_back("成交笔数不一致：逐笔 " + std::to_string(trades.size())
            + " 笔，统计 " + std::to_string(summary.fills) + " 笔");
    }
    if (std::fabs((trade_close_profit - trade_commission) - summary.net_pnl) > 0.5) {
        summary.warnings.push_back("资金勾稽不一致：逐笔平仓盈亏减手续费="
            + money_text(trade_close_profit - trade_commission)
            + "，账户净盈亏=" + money_text(summary.net_pnl));
    }
    if (std::fabs(trade_commission - summary.total_commission) > 0.5) {
        summary.warnings.push_back("手续费勾稽不一致：逐笔 " + money_text(trade_commission)
            + "，逐日 " + money_text(summary.total_commission));
    }
    if (summary.flat_violations > 0) {
        summary.warnings.push_back("存在 " + std::to_string(summary.flat_violations)
            + " 个交易日收盘仍有持仓");
    }
    if (meta.failed_days > 0) {
        summary.warnings.push_back("存在 " + std::to_string(meta.failed_days)
            + " 个交易日回放失败，结果不完整");
    }
    return summary;
}

std::filesystem::path backtest_report_directory(
        const std::filesystem::path& root,
        const std::string& exchange_id, const std::string& instrument_id,
        const std::string& strategy_id, uint64_t run_id, const std::string& timestamp) {
    return root
        / (exchange_id + "_" + instrument_id)
        / strategy_id
        / (std::to_string(run_id) + "_" + timestamp);
}

std::filesystem::path write_backtest_report(
        const std::filesystem::path& directory,
        const backtest_run_meta& meta,
        const backtest_summary& summary,
        const std::vector<backtest_trade_record>& trades,
        const std::vector<backtest_closed_trade>& closed,
        const std::vector<backtest_daily_equity>& daily) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        throw std::runtime_error("cannot create backtest report directory: " + ec.message());
    }
    const auto per_currency = meta.money_units_per_currency;

    // report.json
    {
        std::ofstream out(directory / "report.json", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open report.json");
        }
        out << "{\n";
        out << "  \"meta\": {\n";
        json_uint_field(out, 4, "run_id", meta.run_id);
        json_string_field(out, 4, "created_at", meta.created_at);
        json_string_field(out, 4, "status", meta.status);
        json_string_field(out, 4, "strategy_id", meta.strategy_id);
        json_string_field(out, 4, "strategy_params", meta.strategy_params);
        json_string_field(out, 4, "exchange_id", meta.exchange_id);
        json_string_field(out, 4, "instrument_id", meta.instrument_id);
        json_string_field(out, 4, "timeframe", meta.timeframe);
        json_string_field(out, 4, "start_date", meta.start_date);
        json_string_field(out, 4, "end_date", meta.end_date);
        json_string_field(out, 4, "data_file", meta.data_file);
        json_int_field(out, 4, "initial_balance_units", meta.initial_balance_units);
        json_int_field(out, 4, "money_units_per_currency", meta.money_units_per_currency);
        json_int_field(out, 4, "tick_value_per_lot", meta.tick_value_per_lot);
        json_int_field(out, 4, "volume_multiple", meta.volume_multiple);
        json_double_field(out, 4, "price_tick", meta.price_tick);
        json_double_field(out, 4, "margin_rate", meta.margin_rate);
        json_double_field(out, 4, "fee_per_lot_open", meta.fee_per_lot_open);
        json_double_field(out, 4, "fee_per_lot_close", meta.fee_per_lot_close);
        json_double_field(out, 4, "slippage_ticks", meta.slippage_ticks);
        json_string_field(out, 4, "matching_mode", meta.matching_mode);
        json_string_field(out, 4, "close_rule", meta.close_rule);
        json_string_field(out, 4, "cost_allocation", meta.cost_allocation);
        json_int_field(out, 4, "failed_days", meta.failed_days);
        json_string_array(out, 4, "warnings", meta.warnings, true);
        out << "  },\n";

        out << "  \"summary\": {\n";
        json_uint_field(out, 4, "trading_days", summary.trading_days);
        json_uint_field(out, 4, "total_bars", summary.total_bars);
        json_double_field(out, 4, "initial_balance", summary.initial_balance);
        json_double_field(out, 4, "final_balance", summary.final_balance);
        json_double_field(out, 4, "net_pnl", summary.net_pnl);
        json_double_field(out, 4, "return_pct", summary.return_pct);
        json_double_field(out, 4, "final_margin", summary.final_margin);
        json_double_field(out, 4, "total_commission", summary.total_commission);
        json_double_field(out, 4, "total_slippage", summary.total_slippage);
        json_uint_field(out, 4, "signals", summary.signals);
        json_uint_field(out, 4, "orders", summary.orders);
        json_uint_field(out, 4, "fills", summary.fills);
        json_uint_field(out, 4, "canceled", summary.canceled);
        json_double_field(out, 4, "fill_rate", summary.fill_rate);
        json_uint_field(out, 4, "closed_trades", summary.closed_trades);
        json_uint_field(out, 4, "wins", summary.wins);
        json_uint_field(out, 4, "losses", summary.losses);
        json_uint_field(out, 4, "breakeven", summary.breakeven);
        json_double_field(out, 4, "win_rate", summary.win_rate);
        json_double_field(out, 4, "gross_profit", summary.gross_profit);
        json_double_field(out, 4, "gross_loss", summary.gross_loss);
        json_double_field(out, 4, "avg_win", summary.avg_win);
        json_double_field(out, 4, "avg_loss", summary.avg_loss);
        json_double_field(out, 4, "payoff_ratio", summary.payoff_ratio);
        json_double_field(out, 4, "profit_factor", summary.profit_factor);
        json_double_field(out, 4, "expectancy", summary.expectancy);
        json_double_field(out, 4, "max_single_win", summary.max_single_win);
        json_double_field(out, 4, "max_single_loss", summary.max_single_loss);
        json_uint_field(out, 4, "max_consecutive_wins", summary.max_consecutive_wins);
        json_uint_field(out, 4, "max_consecutive_losses", summary.max_consecutive_losses);
        json_double_field(out, 4, "avg_holding_minutes", summary.avg_holding_minutes);
        json_double_field(out, 4, "trades_per_day", summary.trades_per_day);
        json_double_field(out, 4, "max_drawdown", summary.max_drawdown);
        json_double_field(out, 4, "max_drawdown_pct", summary.max_drawdown_pct);
        json_string_field(out, 4, "max_drawdown_peak_day", summary.max_drawdown_peak_day);
        json_string_field(out, 4, "max_drawdown_trough_day", summary.max_drawdown_trough_day);
        json_string_field(out, 4, "max_drawdown_recovery_day", summary.max_drawdown_recovery_day);
        json_double_field(out, 4, "max_daily_loss", summary.max_daily_loss);
        json_double_field(out, 4, "daily_volatility", summary.daily_volatility);
        json_double_field(out, 4, "sharpe", summary.sharpe);
        json_double_field(out, 4, "sortino", summary.sortino);
        json_double_field(out, 4, "calmar", summary.calmar);
        json_double_field(out, 4, "annualized_return_pct", summary.annualized_return_pct);
        json_uint_field(out, 4, "flat_violations", summary.flat_violations);
        json_string_array(out, 4, "warnings", summary.warnings, true);
        out << "  },\n";

        out << "  \"daily_equity\": [";
        out << (daily.empty() ? "],\n" : "\n");
        for (size_t index = 0; index < daily.size(); ++index) {
            const auto& row = daily[index];
            out << "    {\n";
            json_string_field(out, 6, "trading_day", row.trading_day);
            json_uint_field(out, 6, "bars", row.bars);
            json_uint_field(out, 6, "signals", row.signals);
            json_uint_field(out, 6, "orders", row.orders);
            json_uint_field(out, 6, "fills", row.fills);
            json_uint_field(out, 6, "canceled", row.canceled);
            json_double_field(out, 6, "realized_pnl",
                units_to_currency(row.realized_pnl_units, per_currency));
            json_double_field(out, 6, "commission",
                units_to_currency(row.commission_units, per_currency));
            json_double_field(out, 6, "balance",
                units_to_currency(row.balance_units, per_currency));
            json_double_field(out, 6, "margin",
                units_to_currency(row.margin_units, per_currency));
            json_double_field(out, 6, "unrealized_pnl",
                units_to_currency(row.unrealized_pnl_units, per_currency));
            json_double_field(out, 6, "equity", row.equity);
            json_double_field(out, 6, "peak_equity", row.peak_equity);
            json_double_field(out, 6, "drawdown", row.drawdown);
            json_double_field(out, 6, "drawdown_pct", row.drawdown_pct);
            json_int_field(out, 6, "close_position", row.close_position);
            json_string_field(out, 6, "status", row.status);
            json_string_field(out, 6, "error", row.error, true);
            out << "    }" << (index + 1 == daily.size() ? "\n" : ",\n");
        }
        if (!daily.empty()) {
            out << "  ],\n";
        }

        out << "  \"trades\": [";
        out << (trades.empty() ? "],\n" : "\n");
        for (size_t index = 0; index < trades.size(); ++index) {
            const auto& trade = trades[index];
            out << "    {\n";
            json_string_field(out, 6, "trade_id", trade.trade_id);
            json_string_field(out, 6, "order_sys_id", trade.order_sys_id);
            json_string_field(out, 6, "strategy_id", trade.strategy_id);
            json_string_field(out, 6, "exchange_id", trade.exchange_id);
            json_string_field(out, 6, "instrument_id", trade.instrument_id);
            json_string_field(out, 6, "trading_day", trade.trading_day);
            json_string_field(out, 6, "trade_time", trade.trade_time);
            json_string_field(out, 6, "direction", side_text(trade.direction));
            json_string_field(out, 6, "offset", offset_text(trade.offset));
            json_string_field(out, 6, "hedge", hedge_text(trade.hedge));
            json_double_field(out, 6, "price", trade.price);
            json_int_field(out, 6, "volume", trade.volume);
            json_double_field(out, 6, "commission",
                units_to_currency(trade.commission_units, per_currency));
            json_double_field(out, 6, "margin_change",
                units_to_currency(trade.margin_change_units, per_currency));
            json_double_field(out, 6, "close_profit",
                units_to_currency(trade.close_profit_units, per_currency));
            json_string_field(out, 6, "fill_source", trade.fill_source);
            json_bool_field(out, 6, "is_approximate", trade.is_approximate);
            json_uint_field(out, 6, "market_sequence", trade.market_sequence, true);
            out << "    }" << (index + 1 == trades.size() ? "\n" : ",\n");
        }
        if (!trades.empty()) {
            out << "  ],\n";
        }

        out << "  \"closed_trades\": [";
        out << (closed.empty() ? "]\n" : "\n");
        for (size_t index = 0; index < closed.size(); ++index) {
            const auto& trade = closed[index];
            out << "    {\n";
            json_string_field(out, 6, "close_trade_id", trade.close_trade_id);
            json_string_field(out, 6, "open_trade_id", trade.open_trade_id);
            json_string_field(out, 6, "instrument_id", trade.instrument_id);
            json_string_field(out, 6, "position_side", trade.position_side);
            json_string_field(out, 6, "open_time", trade.open_time);
            json_string_field(out, 6, "close_time", trade.close_time);
            json_double_field(out, 6, "open_price", trade.open_price);
            json_double_field(out, 6, "close_price", trade.close_price);
            json_int_field(out, 6, "volume", trade.volume);
            json_double_field(out, 6, "gross_pnl", trade.gross_pnl);
            json_double_field(out, 6, "commission", trade.commission);
            json_double_field(out, 6, "net_pnl", trade.net_pnl);
            json_double_field(out, 6, "holding_minutes", trade.holding_minutes);
            json_string_field(out, 6, "exit_reason", trade.exit_reason, true);
            out << "    }" << (index + 1 == closed.size() ? "\n" : ",\n");
        }
        if (!closed.empty()) {
            out << "  ]\n";
        }
        out << "}\n";
    }

    // summary.csv
    {
        std::ofstream out(directory / "summary.csv", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open summary.csv");
        }
        write_csv_row(out, {"metric", "value", "unit"});
        auto row = [&] (const std::string& metric, const std::string& value, const std::string& unit) {
            write_csv_row(out, {metric, value, unit});
        };
        row("status", meta.status, "");
        row("trading_days", std::to_string(summary.trading_days), "days");
        row("total_bars", std::to_string(summary.total_bars), "bars");
        row("initial_balance", money_text(summary.initial_balance), "CNY");
        row("final_balance", money_text(summary.final_balance), "CNY");
        row("net_pnl", money_text(summary.net_pnl), "CNY");
        row("return_pct", percent_text(summary.return_pct), "%");
        row("final_margin", money_text(summary.final_margin), "CNY");
        row("total_commission", money_text(summary.total_commission), "CNY");
        row("total_slippage", money_text(summary.total_slippage), "CNY");
        row("signals", std::to_string(summary.signals), "");
        row("orders", std::to_string(summary.orders), "");
        row("fills", std::to_string(summary.fills), "");
        row("canceled", std::to_string(summary.canceled), "");
        row("closed_trades", std::to_string(summary.closed_trades), "");
        row("wins", std::to_string(summary.wins), "");
        row("losses", std::to_string(summary.losses), "");
        row("breakeven", std::to_string(summary.breakeven), "");
        row("win_rate", percent_text(summary.win_rate), "%");
        row("gross_profit", money_text(summary.gross_profit), "CNY");
        row("gross_loss", money_text(summary.gross_loss), "CNY");
        row("fill_rate", percent_text(summary.fill_rate), "%");
        row("avg_win", money_text(summary.avg_win), "CNY");
        row("avg_loss", money_text(summary.avg_loss), "CNY");
        row("payoff_ratio", fixed_number(summary.payoff_ratio, 4), "");
        row("profit_factor", fixed_number(summary.profit_factor, 4), "");
        row("expectancy", money_text(summary.expectancy), "CNY");
        row("max_single_win", money_text(summary.max_single_win), "CNY");
        row("max_single_loss", money_text(summary.max_single_loss), "CNY");
        row("max_consecutive_wins", std::to_string(summary.max_consecutive_wins), "");
        row("max_consecutive_losses", std::to_string(summary.max_consecutive_losses), "");
        row("avg_holding_minutes", fixed_number(summary.avg_holding_minutes, 2), "minutes");
        row("trades_per_day", fixed_number(summary.trades_per_day, 2), "");
        row("max_drawdown", money_text(summary.max_drawdown), "CNY");
        row("max_drawdown_pct", percent_text(summary.max_drawdown_pct), "%");
        row("max_daily_loss", money_text(summary.max_daily_loss), "CNY");
        row("daily_volatility", percent_text(summary.daily_volatility), "%");
        row("sharpe", fixed_number(summary.sharpe, 4), "");
        row("sortino", fixed_number(summary.sortino, 4), "");
        row("calmar", fixed_number(summary.calmar, 4), "");
        row("annualized_return_pct", percent_text(summary.annualized_return_pct), "%");
        row("flat_violations", std::to_string(summary.flat_violations), "");
        for (const auto& warning : summary.warnings) {
            row("warning", warning, "");
        }
        for (const auto& warning : meta.warnings) {
            row("failure", warning, "");
        }
    }

    // trades.csv
    {
        std::ofstream out(directory / "trades.csv", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open trades.csv");
        }
        write_csv_row(out, {"trade_id", "order_sys_id", "strategy_id", "exchange_id",
            "instrument_id", "trading_day", "calendar_date", "trade_time", "direction",
            "offset", "hedge", "price", "volume", "commission", "margin_change",
            "close_profit", "fill_source", "is_approximate", "market_sequence"});
        for (const auto& trade : trades) {
            write_csv_row(out, {
                trade.trade_id, trade.order_sys_id, trade.strategy_id, trade.exchange_id,
                trade.instrument_id, trade.trading_day, trade.calendar_date, trade.trade_time,
                side_text(trade.direction), offset_text(trade.offset), hedge_text(trade.hedge),
                fixed_number(trade.price, 4), std::to_string(trade.volume),
                money_text(units_to_currency(trade.commission_units, per_currency)),
                money_text(units_to_currency(trade.margin_change_units, per_currency)),
                money_text(units_to_currency(trade.close_profit_units, per_currency)),
                trade.fill_source, trade.is_approximate ? "true" : "false",
                std::to_string(trade.market_sequence),
            });
        }
    }

    // closed_trades.csv
    {
        std::ofstream out(directory / "closed_trades.csv", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open closed_trades.csv");
        }
        write_csv_row(out, {"close_trade_id", "open_trade_id", "instrument_id", "position_side",
            "open_time", "close_time", "open_price", "close_price", "volume", "gross_pnl",
            "commission", "net_pnl", "holding_minutes", "exit_reason"});
        for (const auto& trade : closed) {
            write_csv_row(out, {
                trade.close_trade_id, trade.open_trade_id, trade.instrument_id,
                trade.position_side, trade.open_time, trade.close_time,
                fixed_number(trade.open_price, 4), fixed_number(trade.close_price, 4),
                std::to_string(trade.volume), money_text(trade.gross_pnl),
                money_text(trade.commission), money_text(trade.net_pnl),
                fixed_number(trade.holding_minutes, 2), trade.exit_reason,
            });
        }
    }

    // equity.csv
    {
        std::ofstream out(directory / "equity.csv", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open equity.csv");
        }
        write_csv_row(out, {"trading_day", "bars", "signals", "orders", "fills", "canceled",
            "realized_pnl", "commission", "balance", "margin", "unrealized_pnl", "equity",
            "peak_equity", "drawdown", "drawdown_pct", "close_position", "status"});
        for (const auto& row : daily) {
            write_csv_row(out, {
                row.trading_day, std::to_string(row.bars), std::to_string(row.signals),
                std::to_string(row.orders), std::to_string(row.fills),
                std::to_string(row.canceled),
                money_text(units_to_currency(row.realized_pnl_units, per_currency)),
                money_text(units_to_currency(row.commission_units, per_currency)),
                money_text(units_to_currency(row.balance_units, per_currency)),
                money_text(units_to_currency(row.margin_units, per_currency)),
                money_text(units_to_currency(row.unrealized_pnl_units, per_currency)),
                money_text(row.equity), money_text(row.peak_equity),
                money_text(row.drawdown), percent_text(row.drawdown_pct),
                std::to_string(row.close_position), row.status,
            });
        }
    }

    // report.md
    {
        std::ofstream out(directory / "report.md", std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open report.md");
        }
        out << "# 回测报告 · " << meta.strategy_id << "\n\n";
        out << "- 运行编号：" << meta.run_id << "（" << meta.created_at << "）\n";
        out << "- 状态：" << meta.status << "\n";
        out << "- 合约：" << meta.exchange_id << "/" << meta.instrument_id
            << " | 周期 " << meta.timeframe << "\n";
        out << "- 区间：" << pretty_day(meta.start_date) << " ~ " << pretty_day(meta.end_date)
            << " | " << summary.trading_days << " 个交易日 | " << summary.total_bars << " 根 bar\n";
        out << "- 数据文件：" << meta.data_file << "\n";
        out << "- 初始资金：" << money_text(summary.initial_balance)
            << " | 期末权益：" << money_text(summary.final_balance)
            << " | 净盈亏：" << money_text(summary.net_pnl)
            << "（" << percent_text(summary.return_pct) << "%）\n";
        out << "- 手续费：" << money_text(summary.total_commission)
            << " | 滑点：" << money_text(summary.total_slippage)
            << " | 成交：" << summary.fills << " 笔 | 平仓：" << summary.closed_trades
            << " 次 | 胜率：" << percent_text(summary.win_rate) << "%\n";
        out << "- 盈亏比：" << fixed_number(summary.payoff_ratio, 2)
            << " | 盈利因子：" << fixed_number(summary.profit_factor, 2)
            << " | 单笔期望：" << money_text(summary.expectancy) << "\n";
        out << "- 最大回撤：" << money_text(summary.max_drawdown)
            << "（" << percent_text(summary.max_drawdown_pct) << "%）";
        if (summary.max_drawdown > 0.0) {
            out << " | 峰值日：" << pretty_day(summary.max_drawdown_peak_day)
                << " | 谷值日：" << pretty_day(summary.max_drawdown_trough_day)
                << " | 修复日："
                << (summary.max_drawdown_recovery_day.empty()
                    ? "未修复" : pretty_day(summary.max_drawdown_recovery_day));
        } else {
            out << " | 区间内无回撤";
        }
        out << "\n";
        out << "- 夏普：" << fixed_number(summary.sharpe, 2)
            << " | 索提诺：" << fixed_number(summary.sortino, 2)
            << " | 年化收益：" << percent_text(summary.annualized_return_pct) << "%\n\n";

        out << "## 逐日权益\n\n";
        out << "| 交易日 | 成交 | 已实现盈亏 | 手续费 | 期末权益 | 回撤 | 状态 |\n";
        out << "|---|---:|---:|---:|---:|---:|---|\n";
        for (const auto& row : daily) {
            out << "| " << pretty_day(row.trading_day)
                << " | " << row.fills
                << " | " << money_text(units_to_currency(row.realized_pnl_units, per_currency))
                << " | " << money_text(units_to_currency(row.commission_units, per_currency))
                << " | " << money_text(row.equity)
                << " | " << percent_text(row.drawdown_pct) << "%"
                << " | " << row.status << " |\n";
        }

        out << "\n## 校验\n\n";
        bool has_warning = false;
        for (const auto& warning : meta.warnings) {
            out << "- 失败日：" << warning << "\n";
            has_warning = true;
        }
        for (const auto& warning : summary.warnings) {
            out << "- " << warning << "\n";
            has_warning = true;
        }
        if (!has_warning) {
            out << "- 成交、手续费与资金勾稽一致，收盘持仓校验通过。\n";
        }
        out << "\n明细见同目录 `trades.csv`、`closed_trades.csv`、`equity.csv`、`summary.csv`、`report.json`。\n";
    }

    return directory;
}

std::string format_backtest_summary_text(
        const backtest_run_meta& meta, const backtest_summary& summary) {
    std::ostringstream out;
    out << "exchange=" << meta.exchange_id << " instrument=" << meta.instrument_id
        << " timeframe=" << meta.timeframe << "\n"
        << "days=" << summary.trading_days
        << " bars=" << summary.total_bars
        << " orders=" << summary.orders
        << " fills=" << summary.fills
        << " canceled=" << summary.canceled << "\n"
        << "closed_trades=" << summary.closed_trades
        << " net_pnl=" << money_text(summary.net_pnl)
        << " commission=" << money_text(summary.total_commission)
        << " win_rate=" << percent_text(summary.win_rate) << "%"
        << " profit_factor=" << fixed_number(summary.profit_factor, 2)
        << " max_drawdown=" << money_text(summary.max_drawdown)
        << "(" << percent_text(summary.max_drawdown_pct) << "%)"
        << " sharpe=" << fixed_number(summary.sharpe, 2) << "\n"
        << "signals=" << summary.signals
        << " failed_days=" << meta.failed_days;
    return out.str();
}

} // namespace seastar::xtrader
