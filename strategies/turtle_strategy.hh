// 海龟策略（Turtle Trading System 的简化实现）。
// 独立策略文件：仅依赖策略框架基类 strategy_base，不依赖任何回测引擎代码。
// Source: user request, 2026-11-27.
#pragma once

#include <seastar/xtrader/strategy_base.hh>

#include <deque>

namespace seastar::xtrader {

// 海龟策略专属参数（与框架的 strategy_config 分离，便于独立调参）
struct turtle_strategy_params {
    int entry_period = 20;           // 唐奇安入场通道周期（System 1 默认 20）
    int exit_period = 10;            // 唐奇安出场通道周期（System 1 默认 10）
    int atr_period = 20;             // ATR（N 值）计算周期
    double atr_stop_multiple = 2.0;  // 止损 = 入场价 ± N * 该倍数
    int default_volume = 1;          // 固定开仓手数（简化仓位管理）
    bool allow_short = true;         // 是否允许做空
};

// 海龟策略：唐奇安通道突破入场 + 短期通道/ATR 止损出场
class turtle_strategy final : public strategy_base {
public:
    turtle_strategy(strategy_config config, turtle_strategy_params params);

    future<> on_init(strategy_context& ctx) override;
    future<> on_market_data(strategy_context& ctx, const domain::market_data& md) override;
    future<> on_bar(strategy_context& ctx, const domain::bar_data& bar) override;
    future<> on_stop(strategy_context& ctx, const sstring& reason) override;

private:
    // 指标计算（基于已收盘的历史 bar，不含当前 bar）
    double entry_high() const noexcept;   // 入场通道最高价
    double entry_low() const noexcept;    // 入场通道最低价
    double exit_high() const noexcept;    // 出场通道最高价
    double exit_low() const noexcept;     // 出场通道最低价
    double atr_value() const noexcept;    // ATR（N 值）简单移动平均
    void push_bar(const domain::bar_data& bar);

    // 下单动作
    future<> open_long(strategy_context& ctx, double price);
    future<> open_short(strategy_context& ctx, double price);
    future<> close_long(strategy_context& ctx, double price);
    future<> close_short(strategy_context& ctx, double price);
    future<> submit(strategy_context& ctx, domain::side dir,
                    domain::offset_flag offset, double price);

    turtle_strategy_params _params;
    sstring _instrument_id;

    // 历史 bar 队列（保留最近 max(entry,exit,atr) 根，用于通道与 ATR 计算）
    std::deque<double> _highs;
    std::deque<double> _lows;
    std::deque<double> _closes;
    std::deque<double> _true_ranges;

    // 本地持仓状态（乐观更新，回测中 limit 单按指定价即时成交）
    int _long_volume = 0;
    int _short_volume = 0;
    double _long_entry_price = 0.0;
    double _short_entry_price = 0.0;
};

} // namespace seastar::xtrader
