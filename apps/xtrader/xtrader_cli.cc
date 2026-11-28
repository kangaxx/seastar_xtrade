/*
 * X-Trader CLI - Main entry point for X-Trader Seastar trading engine
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/xtrader/backtest_driver.hh>
#include <seastar/xtrader/backtest_report.hh>
#include <seastar/xtrader/data_importer.hh>
#include <seastar/xtrader/historical_data_manager.hh>
#include <seastar/xtrader/offline_bar_strategy_replay.hh>
#include <seastar/xtrader/offline_futures_economics.hh>
#include <seastar/xtrader/redis_sync_client.hh>
#include <seastar/xtrader/strategy_registry.hh>
#include <seastar/xtrader/version.hh>
#include <xtrader/minute_to_daily.h>

#include "data_source_menu.hh"
#include "backtest_menu.hh"
#include "market_capture_menu.hh"
#include "xtrader_log.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using seastar::xtrader::cli::ui_text;

namespace xlog = seastar::xtrader::log;

std::string to_lower(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [] (unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

std::filesystem::path config_root() {
    const char* root = std::getenv("XTRADER_CLI_CONFIG_ROOT");
    return root && *root ? std::filesystem::path(root) : std::filesystem::path("/etc/xtrader");
}

struct config_inventory {
    std::vector<std::filesystem::path> envs;
    bool is_complete = true;
};

config_inventory scan_env_dirs(const std::filesystem::path& root) {
    config_inventory inventory;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot inspect configuration root: ", "[ERROR] 无法检查配置根目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
        }
        return inventory;
    }
    if (!std::filesystem::is_directory(root, ec)) {
        std::cerr << ui_text("[ERROR] Configuration root is not an accessible directory", "[ERROR] 配置根目录不可访问或不是目录");
        if (ec) {
            std::cerr << ": " << ec.message();
        }
        std::cerr << std::endl;
        inventory.is_complete = false;
        return inventory;
    }

    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
        std::cerr << ui_text("[ERROR] Cannot list configuration root: ", "[ERROR] 无法列出配置根目录：") << ec.message() << std::endl;
        inventory.is_complete = false;
        return inventory;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const auto& entry = *it;
        const bool is_dir = entry.is_directory(ec);
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot inspect configuration directory: ", "[ERROR] 无法检查配置目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
            ec.clear();
        } else if (is_dir) {
            const auto ini_path = entry.path() / "xtrader.ini";
            if (std::filesystem::is_regular_file(ini_path, ec)) {
                inventory.envs.push_back(entry.path());
            } else if (ec && ec != std::errc::no_such_file_or_directory) {
                std::cerr << ui_text("[ERROR] Cannot inspect account configuration: ", "[ERROR] 无法检查账号配置：") << ec.message() << std::endl;
                inventory.is_complete = false;
            }
            ec.clear();
        }
        it.increment(ec);
        if (ec) {
            std::cerr << ui_text("[ERROR] Cannot continue listing configuration root: ", "[ERROR] 无法继续列出配置根目录：") << ec.message() << std::endl;
            inventory.is_complete = false;
            break;
        }
    }
    std::sort(inventory.envs.begin(), inventory.envs.end());
    return inventory;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string declared_environment(const std::filesystem::path& ini) {
    std::ifstream file(ini);
    if (!file) {
        return "UNREADABLE";
    }

    bool has_declaration = false;
    std::string environment = "UNKNOWN";
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != "account_env") {
            continue;
        }
        if (has_declaration) {
            return "UNKNOWN";
        }
        has_declaration = true;
        const auto value = to_lower(trim(line.substr(separator + 1)));
        if (value == "real") {
            environment = "REAL";
        } else if (value == "simu") {
            environment = "SIMU";
        }
    }
    return file.bad() ? "UNREADABLE" : environment;
}

void print_env_summary(const std::filesystem::path& env_dir, size_t index) {
    const auto environment = declared_environment(env_dir / "xtrader.ini");
    if (environment == "UNREADABLE") {
        std::cerr << ui_text("[ERROR] Cannot read account configuration #", "[ERROR] 无法读取账号配置 #") << index << std::endl;
    }
    std::cout << "  [" << environment << ui_text("] configuration #", "] 配置 #") << index
              << ui_text(" (unverified; account details hidden)", "（未验证；账号详情已隐藏）") << std::endl;
}

// ============================================================
// show_config_info（重构版）
// ============================================================
void show_config_info(const seastar::xtrader::market_data_paths& paths,
                      const std::filesystem::path& selected_data_root) {
    std::cout << ui_text("\n========== Configuration Info ==========", "\n========== 配置信息 ==========") << std::endl;
    std::cout << ui_text("Version:    ", "版本：") << seastar::xtrader::get_full_version() << std::endl;
    std::cout << ui_text("Build:      ", "构建模式：") << seastar::xtrader::build_type << std::endl;
    std::cout << ui_text("Git Commit: ", "Git 提交：") << seastar::xtrader::git_commit << std::endl;
    std::cout << ui_text("Git Branch: ", "Git 分支：") << seastar::xtrader::git_branch << std::endl;
    std::cout << ui_text("Build Time: ", "构建时间：") << seastar::xtrader::get_runtime_build_info().timestamp << std::endl;
    std::cout << "========================================" << std::endl;

    const auto root = config_root();
    const auto inventory = scan_env_dirs(root);

    std::cout << ui_text("\n--- Account Environments ---", "\n--- 账号环境 ---") << std::endl;
    std::cout << ui_text("Config Root: ", "配置目录：") << root.string() << std::endl;

    if (!inventory.is_complete) {
        std::cout << ui_text("[ERROR] Configuration inventory is incomplete", "[ERROR] 配置清单不完整") << std::endl;
    } else if (inventory.envs.empty()) {
        std::cout << ui_text("(no environments found)", "（未找到账号环境）") << std::endl;
    }
    for (size_t index = 0; index < inventory.envs.size(); ++index) {
        print_env_summary(inventory.envs[index], index + 1);
    }
    std::cout << ui_text("Found: ", "找到：") << inventory.envs.size() << ui_text(" environment(s)", " 个环境") << std::endl;
    std::cout << ui_text("Account configuration creation is disabled until secure credential management is available.",
                         "此处不允许创建明文账号配置，请使用独立的安全凭据管理入口。")
              << std::endl;

    // 数据目录
    std::cout << ui_text("\n--- Data Directories ---", "\n--- 数据目录 ---") << std::endl;
    std::cout << ui_text("Default Data Root: ", "默认数据目录：") << paths.source_root().string() << std::endl;
    std::cout << ui_text("Selected Data Root: ", "当前数据目录：") << selected_data_root.string() << std::endl;
    std::cout << "========================================" << std::endl;
}

void run_heartbeat_test() {
    std::cout << ui_text("\n[Heartbeat Test]", "\n[心跳测试]") << std::endl;
    auto start = std::chrono::steady_clock::now();
    seastar::xtrader::redis_sync_client redis;
    if (!redis.connect("127.0.0.1", 6379)) {
        std::cout << ui_text("[FAIL] Cannot connect to Redis", "[FAIL] 无法连接 Redis") << std::endl;
        return;
    }
    auto latency = std::chrono::steady_clock::now() - start;
    std::cout << ui_text("[OK] Connected to Redis in ", "[OK] 已连接 Redis，耗时 ")
              << std::chrono::duration<double, std::milli>(latency).count()
              << " ms" << std::endl;

    // Test SET/GET
    bool set_ok = redis.set("xtrader:heartbeat:test", "ok");
    if (set_ok) {
        std::string val = redis.get("xtrader:heartbeat:test");
        std::cout << ui_text("[OK] SET/GET test: ", "[OK] SET/GET 测试：") << val << std::endl;
    } else {
        std::cout << ui_text("[FAIL] SET failed", "[FAIL] SET 失败") << std::endl;
    }

    redis.disconnect();
}

// 将 epoch 纳秒时间戳反推回 calendar_date(YYYYMMDD) + time(HH:MM:SS)。
// 与 backtest_driver 的 calendar_timestamp_nanos 互逆。
// Source: user request, 2026-11-28.
seastar::xtrader::offline_calendar_time decode_timestamp(int64_t nanos) {
    constexpr int64_t nanos_per_second = 1'000'000'000LL;
    constexpr int64_t nanos_per_day = 86'400LL * nanos_per_second;
    const auto days = nanos / nanos_per_day;
    const auto rem = nanos % nanos_per_day;
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
    const int64_t total_seconds = rem / nanos_per_second;
    const int hour = static_cast<int>(total_seconds / 3600);
    const int minute = static_cast<int>((total_seconds % 3600) / 60);
    const int second = static_cast<int>(total_seconds % 60);

    std::ostringstream date_ss;
    date_ss << std::setw(4) << std::setfill('0') << static_cast<int>(ymd.year())
            << std::setw(2) << static_cast<unsigned>(ymd.month())
            << std::setw(2) << static_cast<unsigned>(ymd.day());
    std::ostringstream time_ss;
    time_ss << std::setw(2) << std::setfill('0') << hour << ':'
            << std::setw(2) << minute << ':' << std::setw(2) << second;
    return {seastar::sstring(date_ss.str()), seastar::sstring(time_ss.str())};
}

// 构造菜粕（RM）等商品期货的离线合约条款：1 元/吨最小变动，10 吨/手，10 元/跳。
// Source: user request, 2026-11-28.
seastar::xtrader::offline_futures_terms make_backtest_terms(
        const seastar::sstring& instrument_id, const seastar::sstring& exchange_id) {
    using namespace seastar::xtrader;
    const offline_rate zero{0, 1, offline_rounding::down};
    offline_futures_terms terms;
    terms.instrument = position_instrument{
        .exchange_id = exchange_id,
        .instrument_id = instrument_id,
        .hedge = domain::hedge_flag::speculation,
    };
    terms.money_unit = "CNY";
    terms.tick_value_per_lot = 10;
    terms.price_tick = 1.0;
    terms.long_margin_rate = offline_rate{1, 10, offline_rounding::up};
    terms.short_margin_rate = offline_rate{1, 10, offline_rounding::up};
    terms.open_fee = offline_fee_rule{0, zero};
    terms.close_fee = offline_fee_rule{0, zero};
    terms.close_today_fee = offline_fee_rule{0, zero};
    terms.close_yesterday_fee = offline_fee_rule{0, zero};
    return terms;
}

// 报告用的本地时间文本与撮合编号文本。
// Source: user request, 2026-11-28.
std::string local_timestamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, format);
    return out.str();
}

std::string simulated_order_id_text(const seastar::xtrader::simulated_order_id& id) {
    return "sim-order-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence);
}

std::string simulated_trade_id_text(const seastar::xtrader::simulated_trade_id& id) {
    return "sim-trade-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence);
}

// 从 "YYYY-MM-DD HH:MM:SS" 提取 "YYYYMMDD"；不合法返回空串。
std::string calendar_date_of(const std::string& datetime) {
    if (datetime.size() < 10 || datetime[4] != '-' || datetime[7] != '-') {
        return {};
    }
    std::string date = datetime.substr(0, 10);
    date.erase(7, 1);
    date.erase(4, 1);
    return date;
}

// 收集数据集的所有交易日（YYYYMMDD，去重、升序）。
std::vector<std::string> collect_trading_days(
        const seastar::xtrader::historical_data_manager& manager,
        const seastar::xtrader::dataset_manifest& dataset) {
    std::set<std::string> days;
    if (dataset.layout_version == 2) {
        for (const auto& file : dataset.files) {
            const auto paths = seastar::xtrader::market_data_paths::for_data_file(file, manager.data_root());
            days.insert(paths.parse_data_file(file).calendar_date);
        }
    } else {
        std::ifstream input(dataset.active_file_path);
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto comma = line.find(',');
            const auto datetime = comma == std::string::npos ? line : line.substr(0, comma);
            const auto date = calendar_date_of(datetime);
            if (!date.empty()) {
                days.insert(date);
            }
        }
    }
    return {days.begin(), days.end()};
}

// 真实回测执行：加载策略动态模块 -> 加载历史 bar -> 离线策略回放。
// Source: user request, 2026-11-28.
std::string run_backtest(const seastar::xtrader::cli::backtest_selection& selection) {
    using namespace seastar::xtrader;
    using seastar::sstring;

    // 1) 探针：确认策略存在，取订阅合约与策略信息快照。
    sstring instrument_id;
    std::string strategy_info;
    {
        auto probe = strategy_registry::instance().create(selection.strategy_id);
        if (probe) {
            strategy_info = probe->get_info();
            const auto& subscriptions = probe->config().subscribed_instruments;
            if (!subscriptions.empty()) {
                instrument_id = subscriptions.front();
            }
        }
    }
    if (instrument_id.empty()) {
        return "strategy not found or has no subscribed instrument: " + selection.strategy_id;
    }

    // 2) 扫描数据集，定位该合约并收集交易日列表。
    historical_data_manager manager(selection.data_root);
    std::string warning;
    const auto datasets = manager.scan_datasets(&warning);
    if (!warning.empty()) {
        return "data scan failed: " + warning;
    }
    const dataset_manifest* selected = nullptr;
    for (const auto& dataset : datasets) {
        if (to_lower(dataset.symbol) == to_lower(std::string(instrument_id))) {
            if (selected != nullptr) {
                return "ambiguous datasets for " + std::string(instrument_id);
            }
            selected = &dataset;
        }
    }
    if (selected == nullptr) {
        return "historical dataset not found for " + std::string(instrument_id);
    }
    auto days = collect_trading_days(manager, *selected);
    // 日期范围过滤（环境变量，YYYYMMDD，闭区间），用于限制回测范围、避免全量逐日。
    if (const char* value = std::getenv("XTRADER_BACKTEST_START"); value && *value) {
        days.erase(std::remove_if(days.begin(), days.end(), [&value] (const std::string& day) {
            return day < std::string(value);
        }), days.end());
    }
    if (const char* value = std::getenv("XTRADER_BACKTEST_END"); value && *value) {
        days.erase(std::remove_if(days.begin(), days.end(), [&value] (const std::string& day) {
            return day > std::string(value);
        }), days.end());
    }
    if (days.empty()) {
        return "no trading days found for " + std::string(instrument_id);
    }

    // 3) 回测参数与结果收集容器；逐日资金滚动，形成连续权益曲线。
    constexpr int64_t tick_value_per_lot = 10;
    constexpr int volume_multiple = 10;
    constexpr double price_tick = 1.0;
    constexpr double margin_rate = 0.1;
    constexpr int64_t money_units_per_currency = 1;
    constexpr int64_t initial_balance_units = 1'000'000;
    int64_t running_balance_units = initial_balance_units;

    size_t traded_days = 0;
    size_t failed_days = 0;
    sstring exchange_id;
    sstring timeframe;
    std::vector<seastar::xtrader::backtest_trade_record> trade_records;
    std::vector<seastar::xtrader::backtest_daily_equity> daily_equity;
    std::vector<std::string> failure_messages;

    // 4) 逐交易日回放：每天只加载当日 bar，模拟行情逐渐到达，避免全量加载与前视。
    for (const auto& day : days) {
        backtest_config data_config;
        data_config.data_root = selection.data_root.string();
        data_config.start_date = sstring(day);
        data_config.end_date = sstring(day);
        backtest_driver driver(data_config);
        try {
            driver.start().get();
            driver.load_data(instrument_id).get();
        } catch (const std::exception& error) {
            // 单日加载失败不丢弃整次回测，记录失败日后继续。
            ++failed_days;
            failure_messages.push_back(day + ": data load failed: " + error.what());
            seastar::xtrader::backtest_daily_equity row;
            row.trading_day = day;
            row.balance_units = running_balance_units;
            row.status = "failed";
            row.error = std::string("data load failed: ") + error.what();
            daily_equity.push_back(std::move(row));
            continue;
        }

        backtest_bar_export_options export_options;
        export_options.timestamp_kind = offline_bar_timestamp::open;
        export_options.duration_nanos = 60'000'000'000LL;
        export_options.time_domain = "synthetic-naive-local";
        export_options.resolve_trading_day = [] (const sstring&, const sstring&,
                                                 const sstring& date, const sstring&) {
            return date;
        };
        std::vector<offline_bar_record> bars;
        try {
            bars = driver.export_offline_bars(export_options);
        } catch (const std::exception& error) {
            ++failed_days;
            failure_messages.push_back(day + ": bar export failed: " + error.what());
            seastar::xtrader::backtest_daily_equity row;
            row.trading_day = day;
            row.balance_units = running_balance_units;
            row.status = "failed";
            row.error = std::string("bar export failed: ") + error.what();
            daily_equity.push_back(std::move(row));
            continue;
        }
        if (bars.empty()) {
            continue;
        }

        exchange_id = bars.front().exchange_id;
        timeframe = bars.front().timeframe;
        const sstring trading_day = bars.front().trading_day;

        offline_bar_strategy_config replay_config;
        replay_config.replay.account = position_account_scope{
            "offline", "sim-broker", "sim-account", trading_day};
        replay_config.replay.money_unit = "CNY";
        replay_config.replay.strategy_id = sstring(selection.strategy_id);
        replay_config.replay.contracts = {make_backtest_terms(instrument_id, exchange_id)};
        replay_config.replay.cost_allocation = offline_cost_allocation::fifo;
        replay_config.replay.price_normalization = offline_price_normalization::exact;
        replay_config.replay.bar_liquidity = simulated_bar_liquidity{.buy_volume = 1, .sell_volume = 1};
        replay_config.replay.run_id = 1;
        // 每日以滚动余额起步，跨日盈亏连续累计。
        replay_config.replay.initial_balance = running_balance_units;
        replay_config.instrument_spec = domain::instrument_spec{
            .instrument_id = instrument_id,
            .exchange_id = exchange_id,
            .volume_multiple = volume_multiple,
            .price_tick = price_tick,
            .is_trading = true,
        };
        replay_config.maximum_price_ticks = 100'000;
        replay_config.money_units_per_currency = money_units_per_currency;
        replay_config.close_rule = close_position_rule::today_first;
        replay_config.max_notifications_per_boundary = 1000;
        replay_config.format_timestamp = decode_timestamp;

        // 每天新建策略实例：日内策略每日状态重置，且回放引擎要求策略处于 created 态。
        auto strategy = strategy_registry::instance().create(selection.strategy_id);
        if (!strategy) {
            return "strategy not found in registry: " + selection.strategy_id;
        }
        try {
            auto replay = offline_bar_strategy_replay::create(replay_config, strategy, bars);
            replay->run().get();
            const auto stats = replay->stats();
            const auto snapshot = replay->snapshot();
            ++traded_days;

            seastar::xtrader::backtest_daily_equity row;
            row.trading_day = std::string(trading_day);
            row.bars = stats.completed_bars;
            row.signals = strategy->stats().signal_count;
            row.orders = stats.submitted_orders;
            row.fills = stats.fills;
            row.canceled = stats.canceled_orders;
            row.realized_pnl_units = snapshot.funds.close_profit;
            row.commission_units = snapshot.funds.commission;
            row.balance_units = snapshot.funds.balance;
            row.margin_units = snapshot.funds.position_margin;
            // 离线模型未实现盯市估值；日内策略收盘空仓，未实现盈亏按 0 记录。
            row.unrealized_pnl_units = 0;
            row.close_position = snapshot.position.total.long_today
                + snapshot.position.total.long_yesterday
                - snapshot.position.total.short_today
                - snapshot.position.total.short_yesterday;
            daily_equity.push_back(std::move(row));
            running_balance_units = snapshot.funds.balance;

            for (const auto& fill : replay->trades()) {
                seastar::xtrader::backtest_trade_record record;
                record.trade_id = simulated_trade_id_text(fill.trade.id);
                record.order_sys_id = simulated_order_id_text(fill.trade.order_id);
                record.strategy_id = std::string(fill.trade.strategy_id);
                record.exchange_id = std::string(fill.trade.exchange_id);
                record.instrument_id = std::string(fill.trade.instrument_id);
                record.trading_day = std::string(trading_day);
                record.calendar_date = std::string(fill.time.calendar_date);
                record.trade_time = std::string(fill.time.time);
                record.timestamp_nanos = fill.timestamp_nanos;
                record.direction = fill.trade.direction;
                record.offset = fill.trade.offset;
                record.hedge = fill.trade.hedge;
                record.price = fill.trade.price;
                record.volume = fill.trade.volume;
                record.commission_units = fill.trade.commission_units.value_or(0);
                record.margin_change_units = fill.trade.margin_change_units.value_or(0);
                record.close_profit_units = fill.trade.close_profit_units.value_or(0);
                record.fill_source = fill.trade.source == simulated_fill_source::bar_open_approximation
                    ? "bar_open_approximation" : "best_quote_snapshot";
                record.is_approximate = fill.trade.is_approximate;
                record.market_sequence = fill.trade.market_sequence;
                trade_records.push_back(std::move(record));
            }
        } catch (const std::exception& error) {
            ++failed_days;
            failure_messages.push_back(day + ": replay failed: " + error.what());
            seastar::xtrader::backtest_daily_equity row;
            row.trading_day = std::string(trading_day);
            row.balance_units = running_balance_units;
            row.status = "failed";
            row.error = std::string("replay failed: ") + error.what();
            daily_equity.push_back(std::move(row));
        }
    }

    if (traded_days == 0 && failed_days == 0) {
        return "no bars to replay";
    }

    // 5) FIFO 平仓配对、汇总统计与报告落盘。
    std::vector<std::string> pairing_warnings;
    const auto closed_trades = build_closed_trades(
        trade_records, tick_value_per_lot, price_tick, money_units_per_currency, pairing_warnings);

    seastar::xtrader::backtest_run_meta meta;
    meta.run_id = static_cast<uint64_t>(std::time(nullptr));
    meta.created_at = local_timestamp("%Y-%m-%d %H:%M:%S");
    meta.strategy_id = selection.strategy_id;
    meta.strategy_params = strategy_info;
    meta.exchange_id = std::string(exchange_id);
    meta.instrument_id = std::string(instrument_id);
    meta.timeframe = std::string(timeframe);
    meta.start_date = days.front();
    meta.end_date = days.back();
    meta.data_file = selected->active_file_path.empty()
        ? selected->dataset_dir.string() : selected->active_file_path.string();
    meta.initial_balance_units = initial_balance_units;
    meta.money_units_per_currency = money_units_per_currency;
    meta.tick_value_per_lot = tick_value_per_lot;
    meta.volume_multiple = volume_multiple;
    meta.price_tick = price_tick;
    meta.margin_rate = margin_rate;
    meta.fee_per_lot_open = 0.0;
    meta.fee_per_lot_close = 0.0;
    meta.slippage_ticks = 0.0;
    meta.matching_mode = "next_bar_open";
    meta.close_rule = "today_first";
    meta.cost_allocation = "fifo";
    meta.failed_days = static_cast<int>(failed_days);
    meta.warnings = failure_messages;

    auto summary = seastar::xtrader::summarize_backtest(
        meta, daily_equity, trade_records, closed_trades);
    summary.warnings.insert(summary.warnings.end(),
        pairing_warnings.begin(), pairing_warnings.end());
    meta.status = (failed_days == 0 && summary.warnings.empty())
        ? "completed" : "incomplete";

    const char* report_env = std::getenv("XTRADER_BACKTEST_REPORT_DIR");
    const std::filesystem::path report_root = report_env && *report_env
        ? std::filesystem::path(report_env) : std::filesystem::path("exports/backtest");
    const auto report_dir = seastar::xtrader::backtest_report_directory(
        report_root, std::string(exchange_id), std::string(instrument_id),
        selection.strategy_id, meta.run_id, local_timestamp("%Y%m%d_%H%M%S"));
    std::string report_path;
    try {
        report_path = seastar::xtrader::write_backtest_report(
            report_dir, meta, summary, trade_records, closed_trades, daily_equity).string();
    } catch (const std::exception& error) {
        summary.warnings.push_back(std::string("报告写出失败: ") + error.what());
    }

    std::ostringstream out;
    if (traded_days == 0) {
        out << "no successful trading days\n";
    }
    out << seastar::xtrader::format_backtest_summary_text(meta, summary);
    if (!failure_messages.empty()) {
        out << "\nfirst_failure=" << failure_messages.front();
    }
    if (!report_path.empty()) {
        out << "\nreport=" << report_path;
    }
    return out.str();
}

void open_backtest_menu(const seastar::xtrader::market_data_paths& paths) {
    seastar::xtrader::cli::run_backtest_menu(
        paths, std::cin, std::cout, std::cerr,
        [] (const seastar::xtrader::cli::backtest_selection& selection) {
            return run_backtest(selection);
        });
}

void run_root_menu() {
    // Source selection changes browsing only, not import destinations or trading modes.
    // Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
    const seastar::xtrader::market_data_paths data_paths;
    auto data_root = data_paths.source_root();
    xlog::info("menu opened data_root=" + data_root.string());
    std::string import_source_path;
    seastar::xtrader::import_source_type import_source_type = seastar::xtrader::import_source_type::taobao_csv;
    seastar::xtrader::import_mode import_mode = seastar::xtrader::import_mode::incremental;
    std::string import_config_path;
    std::string import_timeframe;
    std::string import_destination_source;

    if (::isatty(STDIN_FILENO)) {
        seastar::xtrader::cli::open_market_config_selection();
    }

    while (true) {
        std::cout << "\n========== X-Trader CLI ==========" << std::endl;
        // B1: the general-purpose CLI cannot select a trading environment.
        // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
        std::cout << ui_text("Trading modes are unavailable in this CLI; separate entries are disabled.",
                             "本 CLI 不提供交易功能；独立交易入口仍已禁用。") << std::endl;
        std::cout << ui_text("Data source: ", "数据来源：") << data_root.filename().string() << std::endl;
        std::cout << ui_text("Data root: ", "数据目录：") << data_root.string() << std::endl;
        std::cout << ui_text("3) Import", "3) 导入行情数据") << std::endl;
        std::cout << ui_text("4) Data Brief", "4) 数据概览") << std::endl;
        std::cout << ui_text("5) Config Info", "5) 配置信息") << std::endl;
        std::cout << ui_text("6) Heartbeat Test", "6) 心跳测试") << std::endl;
        std::cout << ui_text("7) Select data source", "7) 选择数据来源") << std::endl;
        std::cout << ui_text("8) Backtest setup (Run is under construction)", "8) 回测设置（执行功能尚未完成）") << std::endl;
        std::cout << ui_text("9) Read-only CTP market capture", "9) CTP 只读行情采集") << std::endl;
        std::cout << ui_text("10) Initial CTP key", "10) 初始化 CTP 密钥和账号配置") << std::endl;
        std::cout << ui_text("11) Select CTP account / set alias", "11) 选择 CTP 账号 / 设置别名") << std::endl;
        std::cout << ui_text("12) Import legacy X-Trader INI", "12) 导入旧版 X-Trader INI") << std::endl;
        std::cout << ui_text("13) Import plaintext X-Trader INI", "13) 导入明文 X-Trader INI") << std::endl;
        std::cout << "14) Language / 语言" << std::endl;
        std::cout << ui_text("15) Minute bars -> Daily bars", "15) 分钟线转日线") << std::endl;
        std::cout << ui_text("q) Quit", "q) 退出") << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << ui_text("Select: ", "选择：");

        std::string input;
        if (!std::getline(std::cin, input)) {
            return;
        }
        input = to_lower(input);
        xlog::info("menu=" + (input.empty() ? std::string("(empty)") : input));

        if (input == "1" || input == "2") {
            xlog::warning("trading requested but unavailable in this CLI: " + input);
            std::cerr << ui_text("[ERROR] Trading is unavailable in this CLI; no operation was started.",
                                 "[ERROR] 本 CLI 不提供交易功能，未启动任何交易操作。") << std::endl;
        } else if (input == "3") {
            // Import menu
            while (true) {
                std::cout << ui_text("\n[Import Menu]", "\n[行情导入菜单]") << std::endl;
                std::cout << ui_text("  source_path: ", "  输入路径：")
                          << (import_source_path.empty() ? ui_text("(empty)", "（未设置）") : import_source_path) << std::endl;
                std::cout << ui_text("  source_type: ", "  输入格式：")
                          << (import_source_type == seastar::xtrader::import_source_type::taobao_csv ? "taobao_csv" : "ctp_clean_csv") << std::endl;
                std::cout << ui_text("  mode: ", "  导入模式：")
                          << (import_mode == seastar::xtrader::import_mode::incremental
                              ? ui_text("incremental", "增量") : ui_text("overwrite", "覆盖")) << std::endl;
                std::cout << ui_text("  config_path: ", "  配置文件：")
                          << (import_config_path.empty() ? ui_text("(default)", "（默认）") : import_config_path) << std::endl;
                std::cout << ui_text("  timeframe: ", "  数据周期：") << (import_timeframe.empty()
                    ? ui_text("(filename/config; bars default 1m)", "（由文件名/配置决定；K 线默认 1m）") : import_timeframe) << std::endl;
                std::cout << ui_text("  destination_source: ", "  目的来源：") << (import_destination_source.empty()
                    ? ui_text("(config; taobao_csv -> taobao, canonical CSV -> historic)",
                              "（由配置决定；淘宝 -> taobao，规范 CSV -> historic）") : import_destination_source) << std::endl;
                std::cout << ui_text("  1) Set source path", "  1) 设置输入路径") << std::endl;
                std::cout << ui_text("  2) Set source type", "  2) 设置输入格式") << std::endl;
                std::cout << ui_text("  3) Set import mode", "  3) 设置导入模式") << std::endl;
                std::cout << ui_text("  4) Set config path", "  4) 设置配置文件") << std::endl;
                std::cout << ui_text("  5) Run import", "  5) 执行导入") << std::endl;
                std::cout << ui_text("  6) Set timeframe (tick / 5s / 2h / 1d / 1week / ...)",
                                     "  6) 设置周期（tick / 5s / 2h / 1d / 1week / ...）") << std::endl;
                std::cout << ui_text("  7) Set destination source", "  7) 设置目的来源") << std::endl;
                std::cout << ui_text("  u) Up", "  u) 返回") << std::endl;
                std::cout << ui_text("Select: ", "选择：");

                std::getline(std::cin, input);
                input = to_lower(input);

                if (input == "1") {
                    std::cout << ui_text("Enter source path: ", "请输入输入文件或目录路径：");
                    std::getline(std::cin, import_source_path);
                    std::cout << ui_text("source_path set to ", "输入路径已设为 ") << import_source_path << std::endl;
                } else if (input == "2") {
                    std::cout << ui_text("Source types:\n  1) taobao_csv\n  2) ctp_clean_csv\nSelect: ",
                                         "输入格式：\n  1) taobao_csv（淘宝 CSV）\n  2) ctp_clean_csv（规范 CSV）\n选择：");
                    std::getline(std::cin, input);
                    if (input == "1" || input == "taobao" || input == "taobao_csv") {
                        import_source_type = seastar::xtrader::import_source_type::taobao_csv;
                        std::cout << ui_text("source_type set to taobao_csv", "输入格式已设为 taobao_csv") << std::endl;
                    } else if (input == "2" || input == "ctp" || input == "ctp_clean_csv") {
                        import_source_type = seastar::xtrader::import_source_type::ctp_clean_csv;
                        std::cout << ui_text("source_type set to ctp_clean_csv", "输入格式已设为 ctp_clean_csv") << std::endl;
                    } else {
                        std::cerr << ui_text("[ERROR] Unknown source type", "[ERROR] 无效的输入格式") << std::endl;
                    }
                } else if (input == "3") {
                    std::cout << ui_text("Import modes:\n  1) incremental\n  2) overwrite\nSelect: ",
                                         "导入模式：\n  1) incremental（增量）\n  2) overwrite（覆盖）\n选择：");
                    std::getline(std::cin, input);
                    if (input == "1" || input == "incremental" || input == "inc") {
                        import_mode = seastar::xtrader::import_mode::incremental;
                        std::cout << ui_text("import_mode set to incremental", "导入模式已设为增量") << std::endl;
                    } else if (input == "2" || input == "overwrite" || input == "full") {
                        import_mode = seastar::xtrader::import_mode::overwrite;
                        std::cout << ui_text("import_mode set to overwrite", "导入模式已设为覆盖") << std::endl;
                    } else {
                        std::cerr << ui_text("[ERROR] Unknown import mode", "[ERROR] 无效的导入模式") << std::endl;
                    }
                } else if (input == "4") {
                    std::cout << ui_text("Enter config path (empty for default): ", "请输入配置文件路径（留空使用默认）：");
                    std::getline(std::cin, import_config_path);
                    if (import_config_path.empty()) {
                        std::cout << ui_text("config_path reset to default", "配置文件已恢复默认") << std::endl;
                    } else {
                        std::cout << ui_text("config_path set to ", "配置文件已设为 ") << import_config_path << std::endl;
                    }
                } else if (input == "5") {
                    if (import_source_path.empty()) {
                        std::cerr << ui_text("[ERROR] source_path not set", "[ERROR] 尚未设置输入路径") << std::endl;
                        continue;
                    }
                    if (!std::filesystem::exists(import_source_path)) {
                        std::cerr << ui_text("[ERROR] source_path does not exist: ", "[ERROR] 输入路径不存在：") << import_source_path << std::endl;
                        continue;
                    }

                    seastar::xtrader::import_options opts;
                    opts.source_path = import_source_path;
                    opts.source_type = import_source_type;
                    opts.mode = import_mode;
                    opts.timeframe = import_timeframe;
                    opts.destination_source = import_destination_source;
                    if (!import_config_path.empty()) {
                        opts.config_path = import_config_path;
                    }

                    std::cout << ui_text("Starting import...", "开始导入...") << std::endl;
                    seastar::xtrader::data_importer importer;
                    auto result = importer.execute(opts);

                    std::cout << (result.success ? ui_text("[OK] Import succeeded", "[OK] 导入成功")
                                                 : ui_text("[ERROR] Import failed", "[ERROR] 导入失败")) << std::endl;
                    std::cout << ui_text("  Destination: ", "  目的目录：") << result.target_root.string() << std::endl;
                    std::cout << ui_text("  Files: ", "  文件统计：") << result.scanned_files << ui_text(" scanned, ", " 个已扫描，")
                              << result.success_files << ui_text(" success, ", " 个成功，")
                              << result.skipped_files << ui_text(" skipped, ", " 个跳过，")
                              << result.failed_files << ui_text(" failed", " 个失败") << std::endl;
                    std::cout << ui_text("  Rows: ", "  数据行：") << result.rows_added << ui_text(" added, ", " 行新增，")
                              << result.rows_overwritten << ui_text(" overwritten, ", " 行覆盖，")
                              << result.rows_skipped << ui_text(" skipped", " 行跳过") << std::endl;
                    if (!result.message.empty()) {
                        std::cout << ui_text("  Message: ", "  详细信息：") << result.message << std::endl;
                    }
                    for (const auto& report : result.file_reports) {
                        std::cout << "  " << report.source_file.string() << " [" << report.timeframe
                            << "]: " << report.status << " - " << report.message << std::endl;
                    }
                } else if (input == "6") {
                    std::cout << ui_text("Enter timeframe (empty for filename/config): ", "请输入周期（留空由文件名/配置决定）：");
                    std::string value;
                    std::getline(std::cin, value);
                    try {
                        import_timeframe = value.empty() ? value
                            : seastar::xtrader::market_data_paths::normalize_timeframe(value);
                    } catch (const std::invalid_argument& error) {
                        std::cerr << "[ERROR] " << error.what() << std::endl;
                    }
                } else if (input == "7") {
                    std::cout << ui_text("Enter source name (empty for config/default): ", "请输入目的来源名（留空使用配置/默认值）：");
                    std::string value;
                    std::getline(std::cin, value);
                    try {
                        if (!value.empty()) {
                            auto registered = data_paths;
                            try {
                                (void)registered.source_root(value);
                            } catch (const std::invalid_argument&) {
                                registered = seastar::xtrader::market_data_paths(data_paths.root(), {value});
                            }
                        }
                        import_destination_source = value;
                    } catch (const std::invalid_argument& error) {
                        std::cerr << "[ERROR] " << error.what() << std::endl;
                    }
                } else if (input == "u" || input == "back" || input == "up") {
                    break;
                } else {
                    std::cerr << ui_text("[ERROR] Unknown command", "[ERROR] 无效的命令") << std::endl;
                }
            }
        } else if (input == "4") {
            // Data brief
            std::cout << ui_text("\n[Data Brief]", "\n[数据概览]") << std::endl;
            std::cout << ui_text("Data root: ", "数据目录：") << data_root.string() << std::endl;
            seastar::xtrader::historical_data_manager manager(data_root);
            std::string warning;
            auto datasets = manager.scan_datasets(&warning);
            if (!warning.empty()) {
                std::cout << "[WARN] " << warning << std::endl;
            }
            std::cout << ui_text("Found ", "找到 ") << datasets.size() << ui_text(" datasets:", " 个数据集：") << std::endl;
            for (const auto& ds : datasets) {
                std::cout << "  " << ds.exchange << "/" << ds.symbol
                          << " " << ds.timeframe
                          << ui_text(" source=", " 来源=") << ds.source
                          << ui_text(" kind=", " 类型=") << (ds.kind == seastar::xtrader::market_data_kind::tick ? "tick" : ui_text("bar", "K线"));
                if (ds.layout_version == 2) {
                    std::cout << " (" << ds.files.size() << ui_text(" daily files)", " 个日文件)") << std::endl;
                } else {
                    std::cout << ui_text(" (legacy manifest)", "（旧版 manifest）") << std::endl;
                }
            }
        } else if (input == "5") {
            show_config_info(data_paths, data_root);
        } else if (input == "6") {
            run_heartbeat_test();
        } else if (input == "7") {
            seastar::xtrader::cli::select_data_source(
                data_paths, data_root, std::cin, std::cout, std::cerr);
        } else if (input == "8") {
            open_backtest_menu(data_paths);
        } else if (input == "9") {
            seastar::xtrader::cli::open_market_capture_menu();
        } else if (input == "10") {
            seastar::xtrader::cli::open_initial_ctp_key_menu();
        } else if (input == "11") {
            seastar::xtrader::cli::open_market_config_selection();
        } else if (input == "12") {
            seastar::xtrader::cli::open_legacy_import_menu();
        } else if (input == "13") {
            seastar::xtrader::cli::open_plaintext_import_menu();
        } else if (input == "14" || input == "language") {
            seastar::xtrader::cli::run_language_menu(std::cin, std::cout, std::cerr);
        } else if (input == "15") {
            // 分钟线 -> 日线：直接调用独立 SO 的 C ABI，夜盘默认归下一交易日。
            std::string m2d_input_path;
            std::string m2d_output_path;
            std::string m2d_start_date;
            std::string m2d_end_date;
            while (true) {
                std::cout << ui_text("\n[Minute -> Daily]", "\n[分钟线转日线]") << std::endl;
                std::cout << ui_text("  input: ", "  输入 CSV：")
                          << (m2d_input_path.empty() ? ui_text("(not set)", "（未设置）")
                                                     : m2d_input_path) << std::endl;
                std::cout << ui_text("  output: ", "  输出 CSV：")
                          << (m2d_output_path.empty() ? ui_text("(input + .daily.csv)",
                                                                "（输入路径 + .daily.csv）")
                                                      : m2d_output_path) << std::endl;
                std::cout << ui_text("  start: ", "  开始交易日：")
                          << (m2d_start_date.empty() ? ui_text("(all)", "（不限）")
                                                     : m2d_start_date) << std::endl;
                std::cout << ui_text("  end: ", "  结束交易日：")
                          << (m2d_end_date.empty() ? ui_text("(all)", "（不限）")
                                                   : m2d_end_date) << std::endl;
                std::cout << ui_text("  night session: next trading day",
                                     "  夜盘归属：下一交易日") << std::endl;
                std::cout << ui_text("  1) Set input path", "  1) 设置输入路径") << std::endl;
                std::cout << ui_text("  2) Set output path", "  2) 设置输出路径") << std::endl;
                std::cout << ui_text("  3) Set start date", "  3) 设置开始交易日") << std::endl;
                std::cout << ui_text("  4) Set end date", "  4) 设置结束交易日") << std::endl;
                std::cout << ui_text("  5) Run conversion", "  5) 执行转换") << std::endl;
                std::cout << ui_text("  u) Up", "  u) 返回") << std::endl;
                std::cout << ui_text("Select: ", "选择：");

                std::getline(std::cin, input);
                input = to_lower(input);
                if (input == "1") {
                    std::cout << ui_text("Enter input CSV path: ", "请输入分钟线 CSV 路径：");
                    std::getline(std::cin, m2d_input_path);
                } else if (input == "2") {
                    std::cout << ui_text("Enter output CSV path (empty for default): ",
                                         "请输入日线 CSV 路径（留空使用默认）：");
                    std::getline(std::cin, m2d_output_path);
                } else if (input == "3") {
                    std::cout << ui_text("Enter start trading day YYYY-MM-DD (empty for all): ",
                                         "请输入开始交易日 YYYY-MM-DD（留空不限）：");
                    std::getline(std::cin, m2d_start_date);
                } else if (input == "4") {
                    std::cout << ui_text("Enter end trading day YYYY-MM-DD (empty for all): ",
                                         "请输入结束交易日 YYYY-MM-DD（留空不限）：");
                    std::getline(std::cin, m2d_end_date);
                } else if (input == "5") {
                    if (m2d_input_path.empty()) {
                        std::cerr << ui_text("[ERROR] input path not set",
                                             "[ERROR] 尚未设置输入路径") << std::endl;
                        continue;
                    }
                    if (!std::filesystem::exists(m2d_input_path)) {
                        std::cerr << ui_text("[ERROR] input path does not exist: ",
                                             "[ERROR] 输入路径不存在：")
                                  << m2d_input_path << std::endl;
                        continue;
                    }
                    const std::string output_path = m2d_output_path.empty()
                        ? m2d_input_path + ".daily.csv" : m2d_output_path;
                    xt_m2d_options options{};
                    options.input_path = m2d_input_path.c_str();
                    options.output_path = output_path.c_str();
                    options.start_date = m2d_start_date.empty() ? nullptr : m2d_start_date.c_str();
                    options.end_date = m2d_end_date.empty() ? nullptr : m2d_end_date.c_str();
                    options.night_mode = XT_M2D_NIGHT_NEXT_DAY;
                    xt_m2d_stats stats{};
                    std::cout << ui_text("Converting...", "开始转换...") << std::endl;
                    xlog::info("minute_to_daily input=" + m2d_input_path + " output=" + output_path);
                    const int result = xt_m2d_convert(&options, &stats);
                    std::cout << ui_text("  Rows: ", "  数据行：") << stats.input_rows
                              << ui_text(" input, ", " 输入，") << stats.output_rows
                              << ui_text(" daily, ", " 日线，") << stats.skipped_rows
                              << ui_text(" skipped, ", " 跳过，") << stats.unassigned_night_rows
                              << ui_text(" unassigned night", " 根夜盘无法归属") << std::endl;
                    if (result == 0) {
                        std::cout << ui_text("[OK] Daily file written: ",
                                             "[OK] 日线文件已生成：")
                                  << output_path << std::endl;
                    } else {
                        std::cerr << ui_text("[ERROR] Conversion failed: ",
                                             "[ERROR] 转换失败：")
                                  << (stats.error[0] ? stats.error : "unknown error") << std::endl;
                    }
                } else if (input == "u" || input == "back" || input == "up") {
                    break;
                } else {
                    std::cerr << ui_text("[ERROR] Unknown command", "[ERROR] 无效的命令") << std::endl;
                }
            }
        } else if (input == "q" || input == "quit" || input == "exit") {
            xlog::info("exit");
            std::cout << ui_text("Goodbye!", "再见！") << std::endl;
            break;
        } else {
            std::cout << ui_text("Invalid option", "无效的选项") << std::endl;
        }
    }
}

} // namespace

int main(int ac, char** av) {
    xlog::initialize("xtrader_cli");
    try {
        (void)seastar::xtrader::cli::current_ui_language();
    } catch (const std::invalid_argument& error) {
        xlog::error(std::string("invalid XTRADER_LANGUAGE: ") + error.what());
        std::cerr << "[ERROR] " << error.what() << std::endl;
        return 2;
    }
    std::cout << "========================================" << std::endl;
    std::cout << "X-Trader Seastar Trading Engine v"
              << seastar::xtrader::get_full_version() << std::endl;
    std::cout << ui_text("Build: ", "构建：") << seastar::xtrader::build_type
              << " | Git: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "========================================" << std::endl;
    xlog::info(std::string("start version=") + seastar::xtrader::get_full_version());

    seastar::app_template app;

    return app.run(ac, av, [] {
        return seastar::async([] {
            run_root_menu();
        });
    });
}
