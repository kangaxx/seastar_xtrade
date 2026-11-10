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
#include <seastar/xtrader/market_data_pipeline.hh>

#include "ledger_validation.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace seastar::xtrader {

market_data_pipeline::market_data_pipeline(sstring time_domain, std::vector<market_key> markets)
    : _time_domain(std::move(time_domain)) {
    if (!ledger_detail::has_identifier(_time_domain) || markets.empty()) {
        throw std::invalid_argument("market pipeline needs a time domain and configured markets");
    }
    for (auto& key : markets) {
        if (!ledger_detail::has_identifier(key.first) || !ledger_detail::has_identifier(key.second)
                || !_markets.emplace(std::move(key), market_state{}).second) {
            throw std::invalid_argument("market pipeline has invalid or duplicate markets");
        }
    }
}

void market_data_pipeline::validate(const market_tick_input& input) const {
    if (_finished) {
        throw std::logic_error("market pipeline has finished");
    }
    const auto& tick = input.tick;
    if (!_markets.contains({input.exchange_id, tick.instrument_id})
            || input.time_domain != _time_domain || !ledger_detail::is_valid_trading_day(tick.trading_day)
            || input.sequence == 0 || input.sequence <= _last_sequence
            || input.session_begin_ms < 0 || input.timestamp_ms < input.session_begin_ms
            || input.timestamp_ms >= input.session_end_ms
            || input.timestamp_ms > std::numeric_limits<int64_t>::max() - 60000) {
        throw std::invalid_argument("invalid market identity, source sequence, or session timestamp");
    }
    const auto valid_price = [] (double price) {
        return std::isfinite(price) && price > 0 && price < std::numeric_limits<double>::max();
    };
    if (!valid_price(tick.last_price) || !valid_price(tick.bid_price[0])
            || !valid_price(tick.ask_price[0]) || tick.bid_price[0] > tick.ask_price[0]
            || tick.bid_volume[0] < 0 || tick.ask_volume[0] < 0 || tick.volume < 0
            || !std::isfinite(tick.open_interest) || tick.open_interest < 0
            || tick.open_interest == std::numeric_limits<double>::max()) {
        throw std::invalid_argument("missing or invalid last price, best quote, volume, or open interest");
    }
}

std::optional<int> market_data_pipeline::volume_delta(const market_tick_input& current,
                                                      const market_state& state, bool gap) {
    if (state.previous) {
        const auto& previous = *state.previous;
        const bool same_day = current.tick.trading_day == previous.tick.trading_day;
        if (current.timestamp_ms < previous.timestamp_ms
                || current.tick.trading_day < previous.tick.trading_day
                || (same_day && current.tick.volume < previous.tick.volume)) {
            throw std::invalid_argument("market timestamp/day regressed or intraday volume reset");
        }
        const bool same_session = current.session_begin_ms == previous.session_begin_ms
            && current.session_end_ms == previous.session_end_ms;
        if ((!same_session || !same_day) && current.session_begin_ms < previous.session_end_ms) {
            throw std::invalid_argument("changed session/day overlaps the previous session");
        }
        if (same_day && !gap && state.has_volume_baseline) {
            return current.tick.volume - previous.tick.volume;
        }
    }
    return std::nullopt;
}

normalized_market_tick market_data_pipeline::accept(market_tick_input input) {
    validate(input);
    auto& state = _markets.at({input.exchange_id, input.tick.instrument_id});
    const bool gap = _last_sequence != 0 && input.sequence - _last_sequence != 1;
    const auto delta = volume_delta(input, state, gap);
    normalized_market_tick result{
        .source = std::move(input), .incremental_volume = delta, .has_sequence_gap = gap,
    };
    auto& current = result.source;
    // A feed-wide gap may hide updates for any configured instrument.
    if (gap) {
        for (auto& [key, market] : _markets) {
            market.has_volume_baseline = false;
            if (market.bar) {
                market.bar->is_complete = false;
            }
        }
    }
    const auto minute_begin = current.timestamp_ms / 60000 * 60000;
    const auto begin = std::max(minute_begin, current.session_begin_ms);
    const auto end = std::min(minute_begin + 60000, current.session_end_ms);
    auto staged = state.bar;
    if (staged && (staged->begin_ms != begin || staged->trading_day != current.tick.trading_day)) {
        result.completed_bar = staged;
        staged.reset();
    }
    // Deltas that bridge a minute boundary cannot be attributed to either minute.
    const bool continuous_bucket = state.previous
        && state.previous->timestamp_ms >= begin && state.previous->timestamp_ms < end;
    if (!staged) {
        staged = recorded_minute_bar{
            .exchange_id = current.exchange_id,
            .instrument_id = current.tick.instrument_id,
            .trading_day = current.tick.trading_day,
            .time_domain = current.time_domain,
            .begin_ms = begin,
            .end_ms = end,
            .open = current.tick.last_price,
            .high = current.tick.last_price,
            .low = current.tick.last_price,
            .close = current.tick.last_price,
            .is_complete = current.timestamp_ms == begin && result.incremental_volume == 0 && !gap,
        };
    }
    if (!result.incremental_volume || (!continuous_bucket && result.incremental_volume != 0)) {
        staged->is_complete = false;
        if (result.completed_bar) {
            result.completed_bar->is_complete = false;
        }
    }
    staged->high = std::max(staged->high, current.tick.last_price);
    staged->low = std::min(staged->low, current.tick.last_price);
    staged->close = current.tick.last_price;
    staged->open_interest = current.tick.open_interest;
    if (continuous_bucket && result.incremental_volume) {
        staged->observed_volume += *result.incremental_volume;
    }
    state.previous = current;
    state.bar = std::move(staged);
    state.has_volume_baseline = true;
    _last_sequence = current.sequence;
    return result;
}

std::vector<recorded_minute_bar> market_data_pipeline::finish() {
    if (_finished) {
        throw std::logic_error("market pipeline has already finished");
    }
    std::vector<recorded_minute_bar> bars;
    for (const auto& [key, state] : _markets) {
        if (state.bar) {
            bars.push_back(*state.bar);
            bars.back().is_complete = false;
        }
    }
    _finished = true;
    return bars;
}

} // namespace seastar::xtrader
