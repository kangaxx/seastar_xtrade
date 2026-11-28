#include "rm_turtle_intraday.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>
#include <seastar/xtrader/strategy_registry.hh>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>

namespace seastar::xtrader {

namespace {

bool is_ascii_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

// 单根 bar 的真实波幅（True Range）
double true_range(double high, double low, double prev_close) {
    if (prev_close <= 0.0) {
        return high - low;  // 每日首根 bar 无前收，用振幅近似
    }
    return std::max({high - low,
                     std::fabs(high - prev_close),
                     std::fabs(low - prev_close)});
}

double window_max(const std::deque<double>& values, int n) {
    double result = 0.0;
    bool first = true;
    for (auto it = values.rbegin(); it != values.rend() && n > 0; ++it, --n) {
        result = first ? *it : std::max(result, *it);
        first = false;
    }
    return result;
}

double window_min(const std::deque<double>& values, int n) {
    double result = 0.0;
    bool first = true;
    for (auto it = values.rbegin(); it != values.rend() && n > 0; ++it, --n) {
        result = first ? *it : std::min(result, *it);
        first = false;
    }
    return result;
}

sstring upper_copy(const sstring& value) {
    sstring result = value;
    for (auto& ch : result) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return result;
}

} // namespace

void rm_turtle_intraday_params::validate() const {
    if (instrument_prefix.empty()) {
        throw std::invalid_argument("rm_turtle_intraday: instrument_prefix must not be empty");
    }
    if (allowed_exchanges.empty()) {
        throw std::invalid_argument("rm_turtle_intraday: allowed_exchanges must not be empty");
    }
    if (day_open_minute < 0 || day_close_minute > 24 * 60 || day_open_minute >= day_close_minute) {
        throw std::invalid_argument("rm_turtle_intraday: invalid day session bounds");
    }
    if (no_entry_before_close_minutes <= 0 || flatten_before_close_minutes <= 0) {
        throw std::invalid_argument("rm_turtle_intraday: close-window minutes must be positive");
    }
    if (flatten_before_close_minutes >= no_entry_before_close_minutes) {
        throw std::invalid_argument(
            "rm_turtle_intraday: flatten window must start after the no-entry window");
    }
    if (no_entry_before_close_minutes >= day_close_minute - day_open_minute) {
        throw std::invalid_argument("rm_turtle_intraday: no-entry window covers the whole session");
    }
    if (entry_period <= 0 || exit_period <= 0 || atr_period <= 0) {
        throw std::invalid_argument("rm_turtle_intraday: indicator periods must be positive");
    }
    if (!(atr_stop_multiple > 0.0)) {
        throw std::invalid_argument("rm_turtle_intraday: atr_stop_multiple must be positive");
    }
    if (volume <= 0) {
        throw std::invalid_argument("rm_turtle_intraday: volume must be positive");
    }
    if (price_offset_ticks < 0) {
        throw std::invalid_argument("rm_turtle_intraday: price_offset_ticks must not be negative");
    }
    if (order_timeout_bars <= 0) {
        throw std::invalid_argument("rm_turtle_intraday: order_timeout_bars must be positive");
    }
}

rm_turtle_intraday::rm_turtle_intraday(strategy_config config, rm_turtle_intraday_params params)
    : strategy_base(std::move(config)), _params(std::move(params)) {
    _params.validate();
    for (const auto& instrument : _config.subscribed_instruments) {
        if (!accepts_instrument(instrument, _params.allowed_exchanges.front())) {
            throw std::invalid_argument("rm_turtle_intraday: subscribed instrument is not an RM contract");
        }
    }
}

bool rm_turtle_intraday::accepts_instrument(const sstring& instrument_id,
                                            const sstring& exchange_id) const {
    const auto prefix_size = _params.instrument_prefix.size();
    if (instrument_id.size() <= prefix_size) {
        return false;
    }
    for (size_t i = 0; i < prefix_size; ++i) {
        if (upper_copy(instrument_id.substr(i, 1)) != _params.instrument_prefix.substr(i, 1)) {
            return false;
        }
    }
    for (size_t i = prefix_size; i < instrument_id.size(); ++i) {
        if (!is_ascii_digit(instrument_id[i])) {
            return false;
        }
    }
    const auto exchange = upper_copy(exchange_id);
    return std::find(_params.allowed_exchanges.begin(), _params.allowed_exchanges.end(), exchange)
        != _params.allowed_exchanges.end();
}

void rm_turtle_intraday::seed_position(const strategy_context& ctx) {
    if (!ctx.query_position) {
        return;
    }
    const auto position = ctx.query_position();
    _long_volume = position.long_today + position.long_yesterday;
    _short_volume = position.short_today + position.short_yesterday;
    if (_long_volume != 0 || _short_volume != 0) {
        // 启动时已有持仓（重启/隔夜）：优先平掉，不在指标未就绪时继续持有。
        _flat_required = true;
    }
}

future<> rm_turtle_intraday::on_init(strategy_context& ctx) {
    if (!ctx.spec.instrument_id.empty()) {
        if (!accepts_instrument(ctx.spec.instrument_id, ctx.spec.exchange_id)) {
            throw std::invalid_argument(
                "rm_turtle_intraday: only RM futures on CZCE/XZCE are supported");
        }
        _instrument_id = ctx.spec.instrument_id;
    }
    seed_position(ctx);

    // 数据粒度由适配器声明：实盘 CTP 推 tick，回测推 bar。
    if (ctx.tick_market_data) {
        if (!ctx.subscribe_market_data) {
            throw std::invalid_argument(
                "rm_turtle_intraday: tick market data is declared but no subscription is available");
        }
        co_await ctx.subscribe_market_data();
    } else {
        if (!ctx.subscribe_bar_data) {
            throw std::invalid_argument(
                "rm_turtle_intraday: bar market data is declared but no subscription is available");
        }
        co_await ctx.subscribe_bar_data();
    }
    co_return;
}

// ===== 行情：实盘 tick 聚合 / 回测 bar =====

bool rm_turtle_intraday::parse_tick_time(const sstring& timestamp, int& hour, int& minute,
                                         int& second) {
    std::string text(timestamp);
    const auto space = text.find_last_of(' ');
    if (space != std::string::npos) {
        text = text.substr(space + 1);
    }
    if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
        return false;
    }
    for (size_t index : {0u, 1u, 3u, 4u, 6u, 7u}) {
        if (index >= text.size() || !is_ascii_digit(text[index])) {
            return false;
        }
    }
    hour = (text[0] - '0') * 10 + (text[1] - '0');
    minute = (text[3] - '0') * 10 + (text[4] - '0');
    second = (text[6] - '0') * 10 + (text[7] - '0');
    return hour < 24 && minute < 60 && second < 60;
}

sstring rm_turtle_intraday::format_minute_label(int total_seconds) {
    const int hour = total_seconds / 3600;
    const int minute = (total_seconds % 3600) / 60;
    const int second = total_seconds % 60;
    char buffer[9];
    buffer[0] = static_cast<char>('0' + (hour / 10) % 10);
    buffer[1] = static_cast<char>('0' + hour % 10);
    buffer[2] = ':';
    buffer[3] = static_cast<char>('0' + minute / 10);
    buffer[4] = static_cast<char>('0' + minute % 10);
    buffer[5] = ':';
    buffer[6] = static_cast<char>('0' + second / 10);
    buffer[7] = static_cast<char>('0' + second % 10);
    buffer[8] = '\0';
    return sstring(buffer);
}

void rm_turtle_intraday::reset_tick_bar() {
    _tick = tick_bar{};
}

future<> rm_turtle_intraday::flush_tick_bar(strategy_context& ctx) {
    if (!_tick.active) {
        co_return;
    }
    domain::bar_data bar;
    bar.instrument_id = _instrument_id;
    bar.exchange_id = ctx.spec.exchange_id;
    bar.calendar_date = _tick.calendar_date;
    bar.timestamp = _tick.label;
    bar.timeframe = "1m";
    bar.open_price = _tick.open;
    bar.high_price = _tick.high;
    bar.low_price = _tick.low;
    bar.close_price = _tick.close;
    bar.volume = _tick.volume;
    bar.open_interest = _tick.open_interest;
    reset_tick_bar();
    co_await process_bar(ctx, bar);
}

future<> rm_turtle_intraday::on_market_data(strategy_context& ctx, const domain::market_data& md) {
    if (!accepts_instrument(md.instrument_id, ctx.spec.exchange_id)) {
        ++_rejected_bars;
        co_return;
    }
    if (!(md.last_price > 0.0)) {
        co_return;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_tick_time(md.update_time, hour, minute, second)) {
        co_return;
    }
    if (_instrument_id.empty()) {
        _instrument_id = md.instrument_id;
    }

    // 右标签 1 分钟 bar：普通 tick 归入下一分钟，整分 tick 归入当前分钟。
    const int total_seconds = hour * 3600 + minute * 60 + second;
    const bool on_minute = second == 0 && md.update_millisec == 0;
    int bucket = on_minute ? total_seconds : (total_seconds / 60 + 1) * 60;
    const bool first_tick = !_tick.active;
    if (first_tick && on_minute) {
        // 开盘第一笔正好落在整分时，归入第一根完整 1 分钟 bar。
        bucket = total_seconds + 60;
    }

    const auto start_bucket = [&] {
        _tick.active = true;
        _tick.bucket_second = bucket;
        _tick.label = format_minute_label(bucket);
        _tick.calendar_date = md.trading_day.empty() ? ctx.trading_day : md.trading_day;
        _tick.open = md.last_price;
        _tick.high = md.last_price;
        _tick.low = md.last_price;
        _tick.close = md.last_price;
        _tick.volume = std::max(0, md.last_volume);
        _tick.open_interest = md.open_interest;
    };

    if (!_tick.active) {
        start_bucket();
    } else if (bucket != _tick.bucket_second) {
        co_await flush_tick_bar(ctx);
        start_bucket();
    } else {
        _tick.high = std::max(_tick.high, md.last_price);
        _tick.low = std::min(_tick.low, md.last_price);
        _tick.close = md.last_price;
        _tick.volume += std::max(0, md.last_volume);
        _tick.open_interest = md.open_interest;
    }

    // 整分 tick 已把当前分钟 bar 补完整，立即结算，避免最后一根 bar 等不到下一笔行情。
    if (on_minute && !(first_tick && _tick.bucket_second == total_seconds + 60)) {
        co_await flush_tick_bar(ctx);
    }
    co_return;
}

future<> rm_turtle_intraday::on_bar(strategy_context& ctx, const domain::bar_data& bar) {
    return process_bar(ctx, bar);
}

// ===== 统一 bar 处理：实盘与回测共用 =====

int rm_turtle_intraday::parse_minute_of_day(const sstring& timestamp) {
    std::string text(timestamp);
    const auto space = text.find_last_of(' ');
    if (space != std::string::npos) {
        text = text.substr(space + 1);
    }
    if (text.size() < 5 || text[2] != ':' || !is_ascii_digit(text[0]) || !is_ascii_digit(text[1])
            || !is_ascii_digit(text[3]) || !is_ascii_digit(text[4])) {
        return -1;
    }
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    if (hour > 23 || minute > 59) {
        return -1;
    }
    return hour * 60 + minute;
}

rm_turtle_intraday::session_phase rm_turtle_intraday::classify(int minute_of_day) const noexcept {
    const int flat_from = _params.day_close_minute - _params.flatten_before_close_minutes;
    const int entry_cutoff = _params.day_close_minute - _params.no_entry_before_close_minutes;
    if (minute_of_day >= flat_from) {
        return session_phase::flatten_only;
    }
    if (minute_of_day >= entry_cutoff) {
        return session_phase::exit_only;
    }
    return session_phase::entry_window;
}

void rm_turtle_intraday::reset_indicators() {
    _highs.clear();
    _lows.clear();
    _closes.clear();
    _true_ranges.clear();
    _day_bars = 0;
}

future<> rm_turtle_intraday::process_bar(strategy_context& ctx, const domain::bar_data& bar) {
    // 1) 品种/交易所过滤：非 RM 一律忽略，不打断回放
    if (!accepts_instrument(bar.instrument_id, bar.exchange_id)) {
        ++_rejected_bars;
        co_return;
    }
    if (_instrument_id.empty()) {
        _instrument_id = bar.instrument_id;
    }

    // 2) 时段过滤：只处理日盘；夜盘与盘外 bar 直接丢弃
    const int minute_of_day = parse_minute_of_day(bar.timestamp);
    if (minute_of_day < 0 || minute_of_day < _params.day_open_minute
            || minute_of_day > _params.day_close_minute) {
        co_return;
    }

    // 3) 跨日重置：指标重置，但持仓以实际成交为准保留，异常持仓优先平掉。
    if (bar.calendar_date != _current_day) {
        reset_indicators();
        _current_day = bar.calendar_date;
        if (_long_volume != 0 || _short_volume != 0) {
            _flat_required = true;
        }
    }
    ++_day_bars;

    // 4) 未完成委托按 bar 计时，超时撤单；等待撤单/成交期间不再发新单。
    if (_working && !_working->terminal) {
        ++_working->bars_alive;
    }
    co_await maybe_cancel_stale(ctx);
    const bool busy = _working.has_value();
    const auto phase = classify(minute_of_day);

    // 5) 异常持仓/隔夜持仓：先平仓，不做任何开仓判断。
    if (_flat_required) {
        if (_long_volume == 0 && _short_volume == 0) {
            _flat_required = false;
        } else {
            push_bar(bar);
            if (!busy) {
                if (_long_volume > 0) {
                    co_await close_long(ctx, bar.close_price);
                } else {
                    co_await close_short(ctx, bar.close_price);
                }
            }
            co_return;
        }
    }

    // 6) 强制平仓优先于一切：最后 2 分钟无条件清仓
    if (phase == session_phase::flatten_only) {
        push_bar(bar);
        if (!busy) {
            if (_long_volume > 0) {
                co_await close_long(ctx, bar.close_price);
            } else if (_short_volume > 0) {
                co_await close_short(ctx, bar.close_price);
            }
        }
        co_return;
    }

    const bool entry_ready = static_cast<int>(_highs.size()) >= _params.entry_period;
    const bool exit_ready = static_cast<int>(_highs.size()) >= _params.exit_period;
    const bool atr_ready = static_cast<int>(_true_ranges.size()) >= _params.atr_period;

    enum class action { none, open_long, open_short, close_long, close_short };
    action act = action::none;
    const double price = bar.close_price;

    // 7) 出场：ATR 止损或反向短通道突破；有活动委托时等待回报，不再叠加新单。
    if (!busy && exit_ready && atr_ready) {
        if (_long_volume > 0) {
            const bool hit_stop = _long_entry_price > 0.0
                && bar.low_price <= _long_entry_price - _params.atr_stop_multiple * atr_value();
            const bool exit_signal = bar.close_price < exit_low();
            if (hit_stop || exit_signal) {
                act = action::close_long;
            }
        } else if (_short_volume > 0) {
            const bool hit_stop = _short_entry_price > 0.0
                && bar.high_price >= _short_entry_price + _params.atr_stop_multiple * atr_value();
            const bool exit_signal = bar.close_price > exit_high();
            if (hit_stop || exit_signal) {
                act = action::close_short;
            }
        }
    }

    // 8) 入场：仅在允许开仓窗口内、无活动委托、实际空仓且突破通道才开
    if (!busy && act == action::none && phase == session_phase::entry_window
            && entry_ready && atr_ready && _long_volume == 0 && _short_volume == 0) {
        if (bar.close_price > entry_high()) {
            act = action::open_long;
        } else if (_params.allow_short && bar.close_price < entry_low()) {
            act = action::open_short;
        }
    }

    // 9) 信号基于不含当前 bar 的通道，计算完再纳入历史
    push_bar(bar);

    switch (act) {
    case action::open_long:   co_await open_long(ctx, price); break;
    case action::open_short:  co_await open_short(ctx, price); break;
    case action::close_long:  co_await close_long(ctx, price); break;
    case action::close_short: co_await close_short(ctx, price); break;
    case action::none:        break;
    }
    co_return;
}

// ===== 委托与成交回报 =====

bool rm_turtle_intraday::terminal_status(domain::order_status status) {
    return status == domain::order_status::filled
        || status == domain::order_status::canceled
        || status == domain::order_status::rejected;
}

bool rm_turtle_intraday::order_matches_working(const domain::order& order) const {
    // 运行时会按 strategy_id 路由；这里只做非空校验，兼容未回填策略归属的 CTP 回报。
    if (!_working || (!order.strategy_id.empty() && order.strategy_id != strategy_id())) {
        return false;
    }
    if (!order.instrument_id.empty() && order.instrument_id != _instrument_id) {
        return false;
    }
    if (!order.order_sys_id.empty() && !_working->order_sys_id.empty()) {
        return order.order_sys_id == _working->order_sys_id;
    }
    if (!order.order_ref.empty() && !_working->order_ref.empty()) {
        return order.order_ref == _working->order_ref;
    }
    return order.direction == _working->direction && order.offset == _working->offset;
}

bool rm_turtle_intraday::trade_matches_working(const domain::trade_report& trade) const {
    if (!_working || (!trade.strategy_id.empty() && trade.strategy_id != strategy_id())) {
        return false;
    }
    if (!trade.instrument_id.empty() && trade.instrument_id != _instrument_id) {
        return false;
    }
    if (!trade.order_sys_id.empty() && !_working->order_sys_id.empty()) {
        return trade.order_sys_id == _working->order_sys_id;
    }
    if (!trade.order_ref.empty() && !_working->order_ref.empty()) {
        return trade.order_ref == _working->order_ref;
    }
    return trade.direction == _working->direction && trade.offset == _working->offset;
}

void rm_turtle_intraday::settle_working_if_done() {
    if (!_working || !_working->terminal) {
        return;
    }
    // 委托已终态；等成交流水补齐已成交数量后再释放，避免回报乱序导致重复下单。
    if (_working->order_filled == 0 || _working->trade_filled >= _working->order_filled) {
        _working.reset();
    }
}

void rm_turtle_intraday::apply_order_report(const domain::order& order) {
    if (!order_matches_working(order)) {
        return;
    }
    if (_working->order_sys_id.empty() && !order.order_sys_id.empty()) {
        _working->order_sys_id = order.order_sys_id;
    }
    if (_working->order_ref.empty() && !order.order_ref.empty()) {
        _working->order_ref = order.order_ref;
    }
    _working->order_filled = std::max(_working->order_filled, order.volume_traded);
    _working->status = order.status;
    if (terminal_status(order.status)) {
        _working->terminal = true;
        settle_working_if_done();
    }
}

void rm_turtle_intraday::apply_trade_report(const domain::trade_report& trade) {
    const int volume = trade.volume;
    if (volume <= 0) {
        return;
    }
    if (trade.offset == domain::offset_flag::open) {
        if (trade.direction == domain::side::buy) {
            const double previous_notional = _long_entry_price * static_cast<double>(_long_volume);
            _long_volume += volume;
            _long_entry_price = (_long_volume > 0)
                ? (previous_notional + trade.price * volume) / _long_volume : trade.price;
        } else {
            const double previous_notional = _short_entry_price * static_cast<double>(_short_volume);
            _short_volume += volume;
            _short_entry_price = (_short_volume > 0)
                ? (previous_notional + trade.price * volume) / _short_volume : trade.price;
        }
    } else if (trade.direction == domain::side::sell) {
        _long_volume = std::max(0, _long_volume - volume);
        if (_long_volume == 0) {
            _long_entry_price = 0.0;
        }
    } else {
        _short_volume = std::max(0, _short_volume - volume);
        if (_short_volume == 0) {
            _short_entry_price = 0.0;
        }
    }
    if (_long_volume == 0 && _short_volume == 0) {
        _flat_required = false;
    }
}

future<> rm_turtle_intraday::on_order_update(strategy_context&, const domain::order& order) {
    apply_order_report(order);
    return make_ready_future<>();
}

future<> rm_turtle_intraday::on_trade_update(strategy_context&, const domain::trade_report& trade) {
    if (!accepts_instrument(trade.instrument_id, trade.exchange_id)) {
        return make_ready_future<>();
    }
    if (_working && trade_matches_working(trade)) {
        _working->trade_filled += trade.volume;
    }
    apply_trade_report(trade);
    settle_working_if_done();
    return make_ready_future<>();
}

// ===== 下单、撤单与执行口径 =====

double rm_turtle_intraday::marketable_price(const strategy_context& ctx, domain::side direction,
                                            double reference_price) const {
    const double tick = ctx.spec.price_tick > 0.0 ? ctx.spec.price_tick : 1.0;
    const long long base_ticks = std::llround(reference_price / tick);
    const long long adjusted = base_ticks + (direction == domain::side::buy
        ? _params.price_offset_ticks : -_params.price_offset_ticks);
    return static_cast<double>(std::max(1LL, adjusted)) * tick;
}

future<> rm_turtle_intraday::submit_marketable(strategy_context& ctx, domain::side direction,
                                               domain::offset_flag offset, double reference_price) {
    if (_working) {
        co_return;
    }
    working_order working;
    working.direction = direction;
    working.offset = offset;
    working.target_volume = _params.volume;
    _working = working;

    domain::order_request request{
        .strategy_id = strategy_id(),
        .instrument_id = _instrument_id,
        .direction = direction,
        .offset = offset,
        .hedge = domain::hedge_flag::speculation,
        .volume = _params.volume,
        .price = marketable_price(ctx, direction, reference_price),
        .type = domain::order_type::limit,
    };
    stats().signal_count++;

    domain::order_status status = domain::order_status::unknown;
    try {
        status = co_await ctx.submit_order(request);
    } catch (...) {
        _working.reset();
        throw;
    }
    if (!_working) {
        co_return;
    }
    if (status == domain::order_status::rejected) {
        _working.reset();
        co_return;
    }
    _working->status = status;
    co_return;
}

future<> rm_turtle_intraday::maybe_cancel_stale(strategy_context& ctx) {
    if (!_working || _working->terminal || _working->cancel_requested) {
        co_return;
    }
    if (_working->bars_alive < _params.order_timeout_bars) {
        co_return;
    }
    if (_working->order_sys_id.empty()) {
        // 尚未收到 OnRtnOrder（没有 OrderSysID），实盘无法安全撤单，继续等待回报。
        co_return;
    }
    _working->cancel_requested = true;
    try {
        co_await ctx.cancel_order(_working->order_sys_id);
    } catch (const std::exception& error) {
        std::cerr << "[WARN] rm_turtle_intraday cancel_order failed: " << error.what() << std::endl;
        if (_working) {
            _working->cancel_requested = false;
        }
    }
    co_return;
}

future<> rm_turtle_intraday::open_long(strategy_context& ctx, double price) {
    return submit_marketable(ctx, domain::side::buy, domain::offset_flag::open, price);
}

future<> rm_turtle_intraday::open_short(strategy_context& ctx, double price) {
    return submit_marketable(ctx, domain::side::sell, domain::offset_flag::open, price);
}

future<> rm_turtle_intraday::close_long(strategy_context& ctx, double price) {
    return submit_marketable(ctx, domain::side::sell, domain::offset_flag::close, price);
}

future<> rm_turtle_intraday::close_short(strategy_context& ctx, double price) {
    return submit_marketable(ctx, domain::side::buy, domain::offset_flag::close, price);
}

future<> rm_turtle_intraday::on_stop(strategy_context& ctx, const sstring&) {
    // 停止时尽力撤销未完成委托；回测收尾阶段适配器可能拒绝撤单，失败不阻断停机。
    if (_working && !_working->terminal && !_working->order_sys_id.empty()) {
        try {
            co_await ctx.cancel_order(_working->order_sys_id);
        } catch (const std::exception&) {
        }
    }
    co_return;
}

// ===== 指标 =====

double rm_turtle_intraday::entry_high() const noexcept {
    return window_max(_highs, _params.entry_period);
}

double rm_turtle_intraday::entry_low() const noexcept {
    return window_min(_lows, _params.entry_period);
}

double rm_turtle_intraday::exit_high() const noexcept {
    return window_max(_highs, _params.exit_period);
}

double rm_turtle_intraday::exit_low() const noexcept {
    return window_min(_lows, _params.exit_period);
}

double rm_turtle_intraday::atr_value() const noexcept {
    if (_true_ranges.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double tr : _true_ranges) {
        sum += tr;
    }
    return sum / static_cast<double>(_true_ranges.size());
}

void rm_turtle_intraday::push_bar(const domain::bar_data& bar) {
    const double prev_close = _closes.empty() ? 0.0 : _closes.back();
    _true_ranges.push_back(true_range(bar.high_price, bar.low_price, prev_close));
    _highs.push_back(bar.high_price);
    _lows.push_back(bar.low_price);
    _closes.push_back(bar.close_price);

    // 日内指标只需最近 max(period) 根，避免随回测无限增长
    const size_t max_period = static_cast<size_t>(std::max(
        {_params.entry_period, _params.exit_period, _params.atr_period}));
    while (_highs.size() > max_period) _highs.pop_front();
    while (_lows.size() > max_period) _lows.pop_front();
    while (_closes.size() > max_period) _closes.pop_front();
    while (_true_ranges.size() > max_period) _true_ranges.pop_front();
}

std::string rm_turtle_intraday::get_info() const {
    return R"({"name":"rm_turtle_intraday","backtest_support":true,"is_intraday":true})";
}

} // namespace seastar::xtrader

namespace {

// 工厂：构造带默认配置的策略实例。
std::shared_ptr<seastar::xtrader::strategy_base> create_rm_turtle_intraday() {
    seastar::xtrader::strategy_config config{
        .strategy_id = "rm_turtle_intraday",
        .strategy_name = "RM 海龟日内策略",
        .strategy_type = "intraday",
        .subscribed_instruments = {"rm9999"},
    };
    return std::make_shared<seastar::xtrader::rm_turtle_intraday>(
        std::move(config), seastar::xtrader::rm_turtle_intraday_params{});
}

// 自动注册：静态对象构造时把工厂注册进全局注册表。
struct rm_turtle_intraday_registrar {
    rm_turtle_intraday_registrar() {
        seastar::xtrader::strategy_registry::instance().register_strategy(
            "rm_turtle_intraday", &create_rm_turtle_intraday);
    }
};
static rm_turtle_intraday_registrar s_rm_turtle_intraday_registrar;

} // namespace
