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
#include <seastar/xtrader/offline_bar_strategy_replay.hh>

#include "ledger_validation.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/coroutine/as_future.hh>

#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace seastar::xtrader {
namespace {

// B3: keep strategy callbacks serialized and never invent market or account data.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
class context_scope {
public:
    context_scope(offline_bar_replay_context*& slot, offline_bar_replay_context& context) : _slot(slot) {
        if (_slot) {
            throw std::logic_error("offline strategy callbacks cannot be reentered");
        }
        _slot = &context;
    }
    ~context_scope() { _slot = nullptr; }

private:
    offline_bar_replay_context*& _slot;
};

class notification_guard {
public:
    explicit notification_guard(bool& consistent) : _consistent(consistent) { _consistent = false; }
    void commit() noexcept { _consistent = true; }

private:
    bool& _consistent;
};

bool valid_time_label(const offline_calendar_time& label) {
    if (!ledger_detail::is_valid_trading_day(label.calendar_date)
        || label.time.size() < 8 || label.time.size() > 18
        || label.time[2] != ':' || label.time[5] != ':') {
        return false;
    }
    for (size_t index = 0; index < label.time.size(); ++index) {
        if (index == 2 || index == 5) {
            continue;
        }
        if (index == 8) {
            if (label.time[index] != '.' || label.time.size() == 9) {
                return false;
            }
        } else if (label.time[index] < '0' || label.time[index] > '9') {
            return false;
        }
    }
    const int hour = (label.time[0] - '0') * 10 + label.time[1] - '0';
    const int minute = (label.time[3] - '0') * 10 + label.time[4] - '0';
    const int second = (label.time[6] - '0') * 10 + label.time[7] - '0';
    return hour < 24 && minute < 60 && second < 60;
}

sstring order_id(const simulated_order_id& id) {
    return sstring("sim-order-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence));
}

sstring trade_id(const simulated_trade_id& id) {
    return sstring("sim-trade-" + std::to_string(id.run_id) + "-" + std::to_string(id.sequence));
}

simulated_order_id parse_order_id(const sstring& value, uint64_t expected_run_id) {
    const std::string_view text(value.data(), value.size());
    constexpr std::string_view PREFIX = "sim-order-";
    if (!text.starts_with(PREFIX)) {
        throw std::invalid_argument("invalid offline order identifier");
    }
    const auto separator = text.find('-', PREFIX.size());
    if (separator == std::string_view::npos) {
        throw std::invalid_argument("invalid offline order identifier");
    }
    simulated_order_id result;
    const auto run = std::from_chars(text.data() + PREFIX.size(), text.data() + separator, result.run_id);
    const auto sequence = std::from_chars(text.data() + separator + 1, text.data() + text.size(), result.sequence);
    if (run.ec != std::errc{} || run.ptr != text.data() + separator
        || sequence.ec != std::errc{} || sequence.ptr != text.data() + text.size()
        || result.run_id != expected_run_id || result.sequence == 0) {
        throw std::invalid_argument("offline order identifier does not belong to this replay");
    }
    return result;
}

domain::order_status to_domain_status(simulated_order_status status) {
    switch (status) {
    case simulated_order_status::accepted: return domain::order_status::accepted;
    case simulated_order_status::partially_filled: return domain::order_status::partially_filled;
    case simulated_order_status::filled: return domain::order_status::filled;
    case simulated_order_status::canceled: return domain::order_status::canceled;
    }
    throw std::logic_error("unknown offline order status");
}

bool consistent_tick_value(const domain::instrument_spec& spec, const offline_futures_terms& terms,
                           int64_t money_units_per_currency) {
    constexpr int64_t MAX_EXACT_INTEGER = (int64_t{1} << 53) - 1;
    if (terms.tick_value_per_lot <= 0 || terms.tick_value_per_lot > MAX_EXACT_INTEGER) {
        return false;
    }
    const auto expected = static_cast<double>(terms.tick_value_per_lot);
    const auto calculated = spec.price_tick * static_cast<double>(spec.volume_multiple)
                          * static_cast<double>(money_units_per_currency);
    return std::isfinite(calculated)
        && (calculated == expected
            || calculated == std::nextafter(expected, -std::numeric_limits<double>::infinity())
            || calculated == std::nextafter(expected, std::numeric_limits<double>::infinity()));
}

} // namespace

std::shared_ptr<offline_bar_strategy_replay> offline_bar_strategy_replay::create(
        offline_bar_strategy_config config, std::shared_ptr<strategy_base> strategy,
        const std::vector<offline_bar_record>& records) {
    return std::shared_ptr<offline_bar_strategy_replay>(
        new offline_bar_strategy_replay(std::move(config), std::move(strategy), records));
}

offline_bar_strategy_replay::offline_bar_strategy_replay(offline_bar_strategy_config config,
        std::shared_ptr<strategy_base> strategy, const std::vector<offline_bar_record>& records)
    : _config(std::move(config)), _strategy(std::move(strategy)) {
    if (!_strategy || _strategy->state() != strategy_state::created || _config.replay.contracts.size() != 1
        || _strategy->strategy_id() != _config.replay.strategy_id
        || _config.maximum_price_ticks <= 0 || _config.money_units_per_currency <= 0
        || _config.money_units_per_currency > (int64_t{1} << 53)
        || _config.max_notifications_per_boundary == 0 || !_config.format_timestamp) {
        throw std::invalid_argument("offline strategy replay requires explicit single-contract configuration");
    }
    _instrument = _config.replay.contracts.front().instrument;
    const auto& spec = _config.instrument_spec;
    const auto& subscriptions = _strategy->config().subscribed_instruments;
    if (spec.instrument_id != _instrument.instrument_id || spec.exchange_id != _instrument.exchange_id
        || spec.price_tick != _config.replay.contracts.front().price_tick || spec.volume_multiple <= 0
        || !consistent_tick_value(spec, _config.replay.contracts.front(), _config.money_units_per_currency)
        || subscriptions.size() != 1 || subscriptions.front() != _instrument.instrument_id) {
        throw std::invalid_argument("strategy, contract, multiplier and money unit must agree");
    }
    prepare_time_labels(records);
    _replay = offline_bar_replay::create(_config.replay, records);
    configure_manager();
}

void offline_bar_strategy_replay::prepare_time_labels(const std::vector<offline_bar_record>& records) {
    std::map<int64_t, offline_calendar_time> unique_times;
    for (const auto& event : build_offline_bar_timeline(records)) {
        auto it = unique_times.find(event.timestamp_nanos);
        if (it == unique_times.end()) {
            auto label = _config.format_timestamp(event.timestamp_nanos);
            if (!valid_time_label(label)) {
                throw std::invalid_argument("offline timestamp formatter returned an invalid date or time");
            }
            it = unique_times.emplace(event.timestamp_nanos, std::move(label)).first;
        }
        _time_labels.emplace(event.sequence, it->second);
    }
}

void offline_bar_strategy_replay::configure_manager() {
    _manager.set_subscription_registry(&_subscriptions);
    _manager.set_context_configurer([this] (strategy_context& context) {
        context.trading_day = _config.replay.account.trading_day;
        context.spec = _config.instrument_spec;
    });
    _manager.set_submit_order_handler([this] (const domain::order_request& request) {
        return futurize_invoke([this, &request] { return submit_order(request); });
    });
    _manager.set_cancel_order_handler([this] (const sstring& id) {
        return futurize_invoke([this, &id] { return cancel_order(id); });
    });
    _manager.set_position_query_handler([this] { return query_position(); });
    _manager.set_account_query_handler([this] () -> const domain::trading_account* {
        static_cast<void>(active_context());
        throw std::logic_error("offline account view requires a mark-to-market model");
    });
}

offline_bar_replay_context& offline_bar_strategy_replay::active_context() const {
    if (!_active_context || !_notifications_consistent) {
        throw std::logic_error("offline strategy replay is not accepting operations");
    }
    return *_active_context;
}

domain::order_status offline_bar_strategy_replay::submit_order(const domain::order_request& request) {
    auto& context = active_context();
    if (_shutdown_phase || request.strategy_id != _strategy->strategy_id()
        || request.instrument_id != _instrument.instrument_id || request.hedge != *_instrument.hedge
        || request.type != domain::order_type::limit || !request.is_valid()) {
        throw std::invalid_argument("offline strategy order does not match the configured contract");
    }
    require_notification_space(1);
    const auto price = context.normalize_price(_instrument, request.price);
    if (price.price_ticks > _config.maximum_price_ticks) {
        throw std::invalid_argument("offline strategy limit exceeds the configured settlement bound");
    }
    const auto order = context.submit({
        .strategy_id = request.strategy_id,
        .instrument = _instrument,
        .direction = request.direction,
        .offset = request.offset,
        .close_rule = request.offset == domain::offset_flag::close ? _config.close_rule : std::nullopt,
        .volume = request.volume,
        .limit_price_ticks = price.price_ticks,
        .maximum_price_ticks = _config.maximum_price_ticks,
    });
    notification_guard guard(_notifications_consistent);
    enqueue(make_order_update(order));
    if (price.adjusted) {
        ++_normalized_order_prices;
    }
    guard.commit();
    return domain::order_status::accepted;
}

domain::order_status offline_bar_strategy_replay::cancel_order(const sstring& id) {
    auto& context = active_context();
    if (_shutdown_phase) {
        throw std::logic_error("offline strategy cancellation is unavailable during shutdown");
    }
    require_notification_space(1);
    const auto order = context.cancel(parse_order_id(id, _config.replay.run_id));
    notification_guard guard(_notifications_consistent);
    enqueue(make_order_update(order));
    guard.commit();
    return domain::order_status::canceled;
}

domain::position_view offline_bar_strategy_replay::query_position() const {
    const auto position = active_context().snapshot(_instrument).position.total;
    return {
        .long_today = position.long_today,
        .long_yesterday = position.long_yesterday,
        .short_today = position.short_today,
        .short_yesterday = position.short_yesterday,
    };
}

domain::order offline_bar_strategy_replay::make_order_update(const simulated_order& order) const {
    if (!order.request.direction || !order.request.offset || !order.request.type) {
        throw std::runtime_error("offline order notification is missing required fields");
    }
    domain::order result;
    result.strategy_id = order.request.strategy_id;
    result.order_ref = order_id(order.id);
    result.order_sys_id = result.order_ref;
    result.broker_id = _config.replay.account.broker_id;
    result.investor_id = _config.replay.account.account_id;
    result.exchange_id = order.request.exchange_id;
    result.instrument_id = order.request.instrument_id;
    result.direction = *order.request.direction;
    result.offset = *order.request.offset;
    result.type = *order.request.type;
    result.limit_price = order.request.limit_price;
    result.volume_original = order.request.volume;
    result.volume_traded = order.filled_volume;
    result.volume_remaining = order.remaining_volume();
    result.submit_status = domain::order_submit_status::accepted;
    result.status = to_domain_status(order.status);
    result.trading_day = _config.replay.account.trading_day;
    result.status_msg = "offline simulated order";
    result.hedge = order.request.hedge;
    return result;
}

domain::trade_report offline_bar_strategy_replay::make_trade_update(
        const simulated_trade& trade, uint64_t event_sequence) const {
    if (!trade.is_approximate || trade.source != simulated_fill_source::bar_open_approximation
        || !trade.commission_units || !trade.margin_change_units || !trade.close_profit_units) {
        throw std::runtime_error("offline trade is missing its settled accounting values");
    }
    domain::trade_report result;
    result.trade_id = trade_id(trade.id);
    result.order_sys_id = order_id(trade.order_id);
    result.order_ref = result.order_sys_id;
    result.instrument_id = trade.instrument_id;
    result.exchange_id = trade.exchange_id;
    result.broker_id = _config.replay.account.broker_id;
    result.investor_id = _config.replay.account.account_id;
    result.direction = trade.direction;
    result.offset = trade.offset;
    result.price = trade.price;
    result.volume = trade.volume;
    result.trade_time = _time_labels.at(event_sequence).time;
    result.trading_day = _config.replay.account.trading_day;
    result.commission = static_cast<double>(*trade.commission_units)
                      / static_cast<double>(_config.money_units_per_currency);
    result.fill_origin = domain::trade_fill_origin::simulated_bar_open;
    result.is_approximate = true;
    result.strategy_id = trade.strategy_id;
    result.hedge = trade.hedge;
    return result;
}

void offline_bar_strategy_replay::require_notification_space(size_t count) const {
    if (!_notifications_consistent || count > _config.max_notifications_per_boundary
        || _pending.size() > _config.max_notifications_per_boundary - count) {
        throw std::runtime_error("offline strategy notification capacity exceeded");
    }
}

void offline_bar_strategy_replay::enqueue(notification value) {
    _pending.push_back(std::move(value));
}

future<> offline_bar_strategy_replay::flush_notifications() {
    size_t delivered = 0;
    while (!_pending.empty()) {
        if (!_notifications_consistent || delivered == _config.max_notifications_per_boundary) {
            throw std::runtime_error("offline strategy notifications cannot be fully delivered");
        }
        auto event = std::move(_pending.front());
        _pending.pop_front();
        ++delivered;
        if (auto* order = std::get_if<domain::order>(&event)) {
            co_await _manager.dispatch_order_update(_strategy->strategy_id(), std::move(*order));
        } else {
            co_await _manager.dispatch_trade_update(
                _strategy->strategy_id(), std::move(std::get<domain::trade_report>(event)));
        }
    }
    if (!_notifications_consistent) {
        throw std::runtime_error("an offline order notification was not recorded");
    }
    co_return;
}

future<> offline_bar_strategy_replay::start_strategy(offline_bar_replay_context& context) {
    context_scope scope(_active_context, context);
    co_await _manager.register_strategy(_strategy);
    co_await _manager.init_all();
    co_await _manager.start_all();
    if (_strategy->state() != strategy_state::running
        || !_subscriptions.has_subscriber(_strategy->strategy_id(), _instrument.instrument_id, event_kind::bar)) {
        throw std::runtime_error("offline strategy did not start or subscribe to its bars");
    }
    co_await flush_notifications();
    co_return;
}

future<> offline_bar_strategy_replay::dispatch_open(offline_bar_replay_context& context,
        const offline_bar_open_notice& notice, const simulated_match_result& result) {
    context_scope scope(_active_context, context);
    if (result.trades.size() != result.order_updates.size()
        || result.market_sequence != notice.sequence
        || result.trades.size() > _config.max_notifications_per_boundary / 2) {
        throw std::runtime_error("offline fill notifications are inconsistent or exceed capacity");
    }
    require_notification_space(result.trades.size() * 2);
    for (size_t index = 0; index < result.trades.size(); ++index) {
        if (result.trades[index].order_id != result.order_updates[index].id
            || result.trades[index].strategy_id != _strategy->strategy_id()
            || result.trades[index].exchange_id != _instrument.exchange_id
            || result.trades[index].instrument_id != _instrument.instrument_id
            || result.trades[index].hedge != *_instrument.hedge
            || result.trades[index].market_sequence != notice.sequence) {
            throw std::runtime_error("offline fill and order notification do not match");
        }
        enqueue(make_order_update(result.order_updates[index]));
        enqueue(make_trade_update(result.trades[index], notice.sequence));
    }
    co_await flush_notifications();
    co_return;
}

future<> offline_bar_strategy_replay::dispatch_bar(offline_bar_replay_context& context,
                                                   const offline_completed_bar_notice& notice) {
    context_scope scope(_active_context, context);
    if (!_subscriptions.has_subscriber(_strategy->strategy_id(), _instrument.instrument_id, event_kind::bar)) {
        throw std::runtime_error("offline strategy is no longer subscribed to its bars");
    }
    const auto& time = _time_labels.at(notice.sequence);
    co_await _manager.dispatch_bar(_strategy->strategy_id(), domain::bar_data{
        .instrument_id = notice.instrument_id,
        .exchange_id = notice.exchange_id,
        .calendar_date = time.calendar_date,
        .timestamp = time.time,
        .timeframe = notice.timeframe,
        .open_price = notice.bar.open_price,
        .high_price = notice.bar.high_price,
        .low_price = notice.bar.low_price,
        .close_price = notice.bar.close_price,
        .volume = notice.bar.volume,
        .open_interest = notice.bar.open_interest,
    });
    co_await flush_notifications();
    co_return;
}

future<> offline_bar_strategy_replay::finish_strategy(offline_bar_replay_context& context) {
    context_scope scope(_active_context, context);
    co_await flush_notifications();
    _shutdown_phase = true;
    co_await _manager.stop_all();
    co_return;
}

future<> offline_bar_strategy_replay::run() {
    auto keep_alive = shared_from_this();
    if (_run_started) {
        throw std::logic_error("offline strategy replay can run only once");
    }
    _run_started = true;
    offline_bar_replay_callbacks callbacks;
    callbacks.on_start = [this] (auto& context) { return start_strategy(context); };
    callbacks.on_bar_open = [this] (auto& context, const auto& notice, const auto& result) {
        return dispatch_open(context, notice, result);
    };
    callbacks.on_bar_completed = [this] (auto& context, const auto& notice) {
        return dispatch_bar(context, notice);
    };
    callbacks.on_finish = [this] (auto& context) { return finish_strategy(context); };
    auto outcome = co_await coroutine::as_future(_replay->run(std::move(callbacks)));
    if (outcome.failed()) {
        const auto failure = outcome.get_exception();
        _shutdown_phase = true;
        auto cleanup = co_await coroutine::as_future(_manager.stop_all());
        if (cleanup.failed()) {
            try {
                cleanup.get();
            } catch (const std::exception& error) {
                std::cerr << "[ERROR] offline strategy cleanup failed: " << error.what() << std::endl;
            }
        } else {
            cleanup.get();
        }
        std::rethrow_exception(failure);
    }
    outcome.get();
    co_return;
}

void offline_bar_strategy_replay::request_stop() noexcept {
    _replay->request_stop();
}

} // namespace seastar::xtrader
