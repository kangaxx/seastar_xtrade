#include "turtle_strategy.hh"

#include <seastar/xtrader/strategy_lifecycle.hh>

#include <algorithm>
#include <cmath>
#include <utility>

namespace seastar::xtrader {

namespace {

// 单根 bar 的真实波幅（True Range）
double true_range(double high, double low, double prev_close) {
    if (prev_close <= 0.0) {
        return high - low;  // 首根 bar 无前收，用振幅近似
    }
    return std::max({high - low,
                     std::fabs(high - prev_close),
                     std::fabs(low - prev_close)});
}

// 从队列尾部取最近 n 根 bar 的最高价
double window_max(const std::deque<double>& values, int n) {
    double result = 0.0;
    bool first = true;
    for (auto it = values.rbegin(); it != values.rend() && n > 0; ++it, --n) {
        result = first ? *it : std::max(result, *it);
        first = false;
    }
    return result;
}

// 从队列尾部取最近 n 根 bar 的最低价
double window_min(const std::deque<double>& values, int n) {
    double result = 0.0;
    bool first = true;
    for (auto it = values.rbegin(); it != values.rend() && n > 0; ++it, --n) {
        result = first ? *it : std::min(result, *it);
        first = false;
    }
    return result;
}

} // namespace

turtle_strategy::turtle_strategy(strategy_config config, turtle_strategy_params params)
    : strategy_base(std::move(config)), _params(std::move(params)) {
}

future<> turtle_strategy::on_init(strategy_context& ctx) {
    _instrument_id = ctx.spec.instrument_id;
    // 海龟策略为日线级别趋势跟踪，仅订阅 bar 数据
    if (ctx.subscribe_bar_data) {
        return ctx.subscribe_bar_data();
    }
    return make_ready_future<>();
}

future<> turtle_strategy::on_market_data(strategy_context&, const domain::market_data&) {
    // 海龟策略不处理逐笔行情，收到 tick 直接忽略
    return make_ready_future<>();
}

future<> turtle_strategy::on_bar(strategy_context& ctx, const domain::bar_data& bar) {
    if (_instrument_id.empty()) {
        _instrument_id = bar.instrument_id;
    }

    const bool entry_ready = static_cast<int>(_highs.size()) >= _params.entry_period;
    const bool exit_ready  = static_cast<int>(_highs.size()) >= _params.exit_period;
    const bool atr_ready   = static_cast<int>(_true_ranges.size()) >= _params.atr_period;

    enum class action { none, open_long, open_short, close_long, close_short };
    action act = action::none;
    const double price = bar.close_price;

    // region 出场与止损：先平仓，避免同根 bar 反转时同时持有双向仓位
    if (exit_ready && atr_ready) {
        if (_long_volume > 0) {
            const bool hit_stop = _long_entry_price > 0.0 &&
                bar.low_price <= _long_entry_price - _params.atr_stop_multiple * atr_value();
            const bool exit_signal = bar.close_price < exit_low();
            if (hit_stop || exit_signal) {
                act = action::close_long;
            }
        } else if (_short_volume > 0) {
            const bool hit_stop = _short_entry_price > 0.0 &&
                bar.high_price >= _short_entry_price + _params.atr_stop_multiple * atr_value();
            const bool exit_signal = bar.close_price > exit_high();
            if (hit_stop || exit_signal) {
                act = action::close_short;
            }
        }
    }
    // endregion

    // region 入场：无仓位时，收盘价突破唐奇安通道则开仓
    if (act == action::none && entry_ready && atr_ready && _long_volume == 0 && _short_volume == 0) {
        if (bar.close_price > entry_high()) {
            act = action::open_long;
        } else if (_params.allow_short && bar.close_price < entry_low()) {
            act = action::open_short;
        }
    }
    // endregion

    // region 将当前 bar 纳入历史（信号基于不含当前 bar 的通道，此处再更新）
    push_bar(bar);
    // endregion

    switch (act) {
    case action::open_long:   return open_long(ctx, price);
    case action::open_short:  return open_short(ctx, price);
    case action::close_long:  return close_long(ctx, price);
    case action::close_short: return close_short(ctx, price);
    case action::none:        break;
    }
    return make_ready_future<>();
}

future<> turtle_strategy::on_stop(strategy_context&, const sstring&) {
    // 停止时无额外资源需要释放，持仓由外部账户体系管理
    return make_ready_future<>();
}

double turtle_strategy::entry_high() const noexcept {
    return window_max(_highs, _params.entry_period);
}

double turtle_strategy::entry_low() const noexcept {
    return window_min(_lows, _params.entry_period);
}

double turtle_strategy::exit_high() const noexcept {
    return window_max(_highs, _params.exit_period);
}

double turtle_strategy::exit_low() const noexcept {
    return window_min(_lows, _params.exit_period);
}

double turtle_strategy::atr_value() const noexcept {
    if (_true_ranges.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double tr : _true_ranges) {
        sum += tr;
    }
    return sum / static_cast<double>(_true_ranges.size());
}

void turtle_strategy::push_bar(const domain::bar_data& bar) {
    const double prev_close = _closes.empty() ? 0.0 : _closes.back();
    _true_ranges.push_back(true_range(bar.high_price, bar.low_price, prev_close));

    _highs.push_back(bar.high_price);
    _lows.push_back(bar.low_price);
    _closes.push_back(bar.close_price);

    // 修剪历史，只保留计算所需的最多根数，避免随回测无限增长
    const size_t max_period = static_cast<size_t>(std::max({
        _params.entry_period, _params.exit_period, _params.atr_period}));
    while (_highs.size() > max_period) _highs.pop_front();
    while (_lows.size() > max_period) _lows.pop_front();
    while (_closes.size() > max_period) _closes.pop_front();
    while (_true_ranges.size() > max_period) _true_ranges.pop_front();
}

future<> turtle_strategy::open_long(strategy_context& ctx, double price) {
    _long_volume += _params.default_volume;
    _long_entry_price = price;
    return submit(ctx, domain::side::buy, domain::offset_flag::open, price);
}

future<> turtle_strategy::open_short(strategy_context& ctx, double price) {
    _short_volume += _params.default_volume;
    _short_entry_price = price;
    return submit(ctx, domain::side::sell, domain::offset_flag::open, price);
}

future<> turtle_strategy::close_long(strategy_context& ctx, double price) {
    _long_volume = 0;
    _long_entry_price = 0.0;
    return submit(ctx, domain::side::sell, domain::offset_flag::close, price);
}

future<> turtle_strategy::close_short(strategy_context& ctx, double price) {
    _short_volume = 0;
    _short_entry_price = 0.0;
    return submit(ctx, domain::side::buy, domain::offset_flag::close, price);
}

future<> turtle_strategy::submit(strategy_context& ctx, domain::side dir,
                                 domain::offset_flag offset, double price) {
    domain::order_request request{
        .strategy_id = strategy_id(),
        .instrument_id = _instrument_id,
        .direction = dir,
        .offset = offset,
        .hedge = domain::hedge_flag::speculation,
        .volume = _params.default_volume,
        .price = price,
        .type = domain::order_type::limit,
    };
    stats().signal_count++;
    return ctx.submit_order(request).then([](domain::order_status) {});
}

} // namespace seastar::xtrader
