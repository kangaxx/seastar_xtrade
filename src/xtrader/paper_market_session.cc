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
#include <seastar/xtrader/paper_market_session.hh>
#include <seastar/core/do_with.hh>

#include <set>
#include <stdexcept>

namespace seastar::xtrader {

namespace {

std::vector<market_data_pipeline::market_key> markets(const paper_market_config& config) {
    std::set<market_data_pipeline::market_key> keys;
    for (const auto& terms : config.contracts) {
        keys.emplace(terms.instrument.exchange_id, terms.instrument.instrument_id);
    }
    return {keys.begin(), keys.end()};
}

} // namespace

paper_market_session::paper_market_session(paper_market_config config, tick_sink record_tick,
                                           bar_sink record_final_bars)
    : _config(std::move(config)), _pipeline(_config.time_domain, markets(_config)),
      _execution(_config.account, _config.money_unit, _config.strategy_id, _config.contracts,
                 _config.allocation, _config.run_id, simulated_market_mode::tick_quote, std::nullopt),
      _record_tick(std::move(record_tick)), _record_final_bars(std::move(record_final_bars)) {
    if (_config.account.environment != "paper" || _config.maximum_quote_age.count() <= 0
            || !_record_tick || !_record_final_bars) {
        throw std::invalid_argument("paper session requires paper namespace, quote age and recording sinks");
    }
    _execution.initialize_flat(_config.initial_balance);
}

future<paper_market_result> paper_market_session::on_tick(market_tick_input tick) {
    if (_stopping || _failed || _busy) {
        return make_exception_future<paper_market_result>(
            std::logic_error("paper session stopped, faulted, or busy; await the previous tick"));
    }
    const auto received = clock::now();
    _busy = true;
    return with_gate(_gate, [this, received, tick = std::move(tick)] () mutable {
        return futurize_invoke([this, received, tick = std::move(tick)] () mutable {
            return process_tick(std::move(tick), received);
        }).then_wrapped([this] (future<paper_market_result> result) {
            _busy = false;
            if (result.failed()) {
                _failed = true;
            }
            return result;
        });
    });
}

future<paper_market_result> paper_market_session::process_tick(market_tick_input tick, clock::time_point received) {
    if (tick.tick.trading_day != _config.account.trading_day) {
        throw std::invalid_argument("paper account cannot roll trading day without settlement");
    }
    return do_with(_pipeline.accept(std::move(tick)), [this, received] (normalized_market_tick& normalized) {
        if (normalized.has_sequence_gap) {
            throw std::runtime_error("market source gap requires reconciliation; matching stopped");
        }
        const auto& input = normalized.source;
        const auto bid = _execution.normalize_price(input.exchange_id, input.tick.instrument_id,
                                                    input.tick.bid_price[0], _config.price_normalization);
        const auto ask = _execution.normalize_price(input.exchange_id, input.tick.instrument_id,
                                                    input.tick.ask_price[0], _config.price_normalization);
        const offline_simulated_quote quote{
            .sequence = input.sequence, .exchange_id = input.exchange_id,
            .instrument_id = input.tick.instrument_id,
            .bid_ticks = bid.price_ticks, .bid_volume = input.tick.bid_volume[0],
            .ask_ticks = ask.price_ticks, .ask_volume = input.tick.ask_volume[0],
        };
        return futurize_invoke(_record_tick, std::as_const(normalized)).then(
            [this, received, quote, &normalized] {
                if (_failed || _stopping || clock::now() - received > _config.maximum_quote_age) {
                    throw std::runtime_error("paper quote invalidated while recording; no match applied");
                }
                auto matches = _execution.on_quote(quote);
                _last_quote[{quote.exchange_id, quote.instrument_id}] = received;
                return paper_market_result{std::move(normalized), std::move(matches)};
            });
    });
}

simulated_order paper_market_session::submit(const offline_simulated_order_request& request) {
    const auto quote = _last_quote.find({request.instrument.exchange_id, request.instrument.instrument_id});
    if (_failed || _stopping || _busy || quote == _last_quote.end()
            || clock::now() - quote->second > _config.maximum_quote_age) {
        throw std::logic_error("paper order blocked: stopped, faulted, busy or stale/missing quote");
    }
    return _execution.submit(request);
}

simulated_order paper_market_session::cancel(const simulated_order_id& id, const sstring& strategy_id) {
    if (_stopping || _busy) {
        throw std::logic_error("paper cancellation blocked during processing or stop");
    }
    // Local cancellation remains available after feed/recording failure.
    return _execution.cancel(id, strategy_id);
}

offline_account_snapshot paper_market_session::snapshot(const position_instrument& instrument) const {
    return _execution.snapshot(instrument);
}

void paper_market_session::disconnect() {
    _failed = true;
    _last_quote.clear();
}

future<> paper_market_session::stop() {
    if (_stopping) {
        return make_exception_future<>(std::logic_error("paper session stop already requested"));
    }
    _stopping = true;
    return _gate.close().then([this] {
        return do_with(_pipeline.finish(), [this] (const std::vector<recorded_minute_bar>& bars) {
            return futurize_invoke(_record_final_bars, bars);
        });
    }).handle_exception([this] (std::exception_ptr error) {
        _failed = true;
        return make_exception_future<>(error);
    });
}

} // namespace seastar::xtrader
