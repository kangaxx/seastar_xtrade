// RM 海龟日内策略（Turtle Intraday, RM only）。
//
// 业务约束（用户需求，2026-11-27）：
//   1) 只接受 RM（菜粕）期货，其它品种、其它交易所一律忽略；
//   2) 只处理日盘 09:00-15:00 的 bar，夜盘（21:00-23:00/23:30）不参与；
//   3) 日内收盘倒数 5 分钟（14:55 起）不允许开新仓；
//   4) 最后 2 分钟（14:58 起）必须平仓，收盘不留隔夜仓。
//
// 时间口径：bar 为右标签（09:01 表示 09:00-09:01），因此
//   - 允许开仓：bar.timestamp < 14:55:00
//   - 禁开仓、可出场：14:55:00 <= timestamp < 14:58:00
//   - 强制平仓：timestamp >= 14:58:00（14:58 / 14:59 / 15:00 三根 bar）
//
// 执行口径（实盘 CTP / 回测通用，2026-11-28）：
//   1) 策略只依赖 strategy_context，不调用任何回测专有接口；
//   2) 持仓只由成交回报（on_trade_update）驱动，初始持仓由 query_position 获取；
//   3) 委托状态由 on_order_update 维护，未成交委托按 bar 超时撤单；
//   4) 下单使用带滑点缓冲的可成交限价（买加/卖减 price_offset_ticks 跳）；
//   5) 实盘 tick 行情通过 on_market_data 聚合为 1 分钟 bar，回测直接接收 bar；
//      适配器通过 strategy_context::tick_market_data 声明数据粒度。
//
// Source: user request, 2026-11-27 / 2026-11-28.
#pragma once

#include <seastar/xtrader/strategy_base.hh>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace seastar::xtrader {

// RM 海龟日内专属参数（与框架 strategy_config 分离）
struct rm_turtle_intraday_params {
    // 品种与交易所白名单
    sstring instrument_prefix = "RM";
    std::vector<sstring> allowed_exchanges{"CZCE", "XZCE"};

    // 日盘时段（分钟数，09:00 = 540，15:00 = 900）
    int day_open_minute = 9 * 60;
    int day_close_minute = 15 * 60;

    // 收盘前的时间约束
    int no_entry_before_close_minutes = 5;
    int flatten_before_close_minutes = 2;

    // 日内海龟参数
    int entry_period = 20;
    int exit_period = 10;
    int atr_period = 20;
    double atr_stop_multiple = 2.0;
    int volume = 1;
    bool allow_short = true;

    // 执行参数：可成交限价的滑点缓冲与挂单超时（实盘/回测共用）
    int price_offset_ticks = 2;
    int order_timeout_bars = 1;

    // 校验参数合法性，非法配置直接抛 std::invalid_argument（启动期失败优于运行期静默）
    void validate() const;
};

class rm_turtle_intraday final : public strategy_base {
public:
    rm_turtle_intraday(strategy_config config, rm_turtle_intraday_params params);

    future<> on_init(strategy_context& ctx) override;
    future<> on_market_data(strategy_context& ctx, const domain::market_data& md) override;
    future<> on_bar(strategy_context& ctx, const domain::bar_data& bar) override;
    future<> on_order_update(strategy_context& ctx, const domain::order& order) override;
    future<> on_trade_update(strategy_context& ctx, const domain::trade_report& trade) override;
    future<> on_stop(strategy_context& ctx, const sstring& reason) override;
    std::string get_info() const override;

    // 是否接受该合约：前缀匹配 + 余下为数字，且交易所命中白名单
    [[nodiscard]] bool accepts_instrument(const sstring& instrument_id,
                                          const sstring& exchange_id) const;

    // 观测用（测试与日志）
    [[nodiscard]] int position_volume() const noexcept { return _long_volume - _short_volume; }
    [[nodiscard]] size_t day_bars() const noexcept { return _day_bars; }
    [[nodiscard]] uint64_t rejected_bars() const noexcept { return _rejected_bars; }
    [[nodiscard]] const sstring& current_day() const noexcept { return _current_day; }
    [[nodiscard]] bool has_working_order() const noexcept { return _working.has_value(); }

private:
    enum class session_phase {
        entry_window,     // 允许开仓
        exit_only,        // 收盘前 5 分钟：只平不开
        flatten_only,     // 最后 2 分钟：强制平仓
    };

    // 当前活动的委托：同一时刻只允许一笔，避免实盘重复下单。
    struct working_order {
        sstring order_ref;
        sstring order_sys_id;
        domain::side direction = domain::side::buy;
        domain::offset_flag offset = domain::offset_flag::open;
        int target_volume = 0;
        int order_filled = 0;      // 委托回报中的累计成交
        int trade_filled = 0;      // 成交流水中的累计成交
        int bars_alive = 0;
        bool terminal = false;
        bool cancel_requested = false;
        domain::order_status status = domain::order_status::unknown;
    };

    // tick 聚合中的 1 分钟 bar（实盘 CTP 行情路径）。
    struct tick_bar {
        bool active = false;
        int bucket_second = -1;
        sstring label;
        sstring calendar_date;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        int volume = 0;
        double open_interest = 0.0;
    };

    // "HH:MM" 或 "HH:MM:SS" 或 "YYYY-MM-DD HH:MM:SS" 解析为当日分钟数，失败返回 -1
    [[nodiscard]] static int parse_minute_of_day(const sstring& timestamp);
    [[nodiscard]] static bool parse_tick_time(const sstring& timestamp, int& hour, int& minute,
                                              int& second);
    [[nodiscard]] static sstring format_minute_label(int total_seconds);
    [[nodiscard]] session_phase classify(int minute_of_day) const noexcept;

    // 统一 bar 处理：回测的 on_bar 与实盘的 tick 聚合都走这里。
    future<> process_bar(strategy_context& ctx, const domain::bar_data& bar);

    // 持仓与委托状态：只由成交/委托回报驱动。
    void seed_position(const strategy_context& ctx);
    void apply_trade_report(const domain::trade_report& trade);
    void apply_order_report(const domain::order& order);
    [[nodiscard]] bool trade_matches_working(const domain::trade_report& trade) const;
    [[nodiscard]] bool order_matches_working(const domain::order& order) const;
    void settle_working_if_done();
    [[nodiscard]] static bool terminal_status(domain::order_status status);

    // 下单与撤单：可成交限价 + 超时撤单。
    [[nodiscard]] double marketable_price(const strategy_context& ctx, domain::side direction,
                                          double reference_price) const;
    future<> submit_marketable(strategy_context& ctx, domain::side direction,
                               domain::offset_flag offset, double reference_price);
    future<> maybe_cancel_stale(strategy_context& ctx);

    // tick 聚合。
    void reset_tick_bar();
    future<> flush_tick_bar(strategy_context& ctx);

    void reset_indicators();
    void push_bar(const domain::bar_data& bar);

    [[nodiscard]] double entry_high() const noexcept;
    [[nodiscard]] double entry_low() const noexcept;
    [[nodiscard]] double exit_high() const noexcept;
    [[nodiscard]] double exit_low() const noexcept;
    [[nodiscard]] double atr_value() const noexcept;

    future<> open_long(strategy_context& ctx, double price);
    future<> open_short(strategy_context& ctx, double price);
    future<> close_long(strategy_context& ctx, double price);
    future<> close_short(strategy_context& ctx, double price);

    rm_turtle_intraday_params _params;
    sstring _instrument_id;
    sstring _current_day;

    // 日内指标队列（每日重置）
    std::deque<double> _highs;
    std::deque<double> _lows;
    std::deque<double> _closes;
    std::deque<double> _true_ranges;

    // 实际持仓：只由成交回报更新
    int _long_volume = 0;
    int _short_volume = 0;
    double _long_entry_price = 0.0;
    double _short_entry_price = 0.0;

    // 委托状态与异常持仓处理
    std::optional<working_order> _working;
    bool _flat_required = false;

    // 实盘 tick 聚合
    tick_bar _tick;

    size_t _day_bars = 0;
    uint64_t _rejected_bars = 0;
};

} // namespace seastar::xtrader
