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
#include <seastar/xtrader/offline_bar_timeline.hh>

#include "ledger_validation.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace seastar::xtrader {
namespace {

// B3: callers must explicitly declare bar duration and timestamp semantics.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct bar_interval {
    size_t index;
    int64_t open_nanos;
    int64_t close_nanos;
};

using market_key = std::tuple<sstring, sstring>;

bar_interval validate_bar(const offline_bar_record& bar, size_t index) {
    if (!ledger_detail::has_identifier(bar.exchange_id) || !ledger_detail::has_identifier(bar.instrument_id)
        || !ledger_detail::has_identifier(bar.timeframe) || !ledger_detail::has_identifier(bar.time_domain)
        || !ledger_detail::is_valid_trading_day(bar.trading_day) || bar.duration_nanos <= 0 || !bar.timestamp_kind
        || (*bar.timestamp_kind != offline_bar_timestamp::open && *bar.timestamp_kind != offline_bar_timestamp::close)
        || !std::isfinite(bar.open_price) || !std::isfinite(bar.high_price)
        || !std::isfinite(bar.low_price) || !std::isfinite(bar.close_price)
        || bar.low_price <= 0.0 || bar.high_price < bar.low_price
        || bar.open_price < bar.low_price || bar.open_price > bar.high_price
        || bar.close_price < bar.low_price || bar.close_price > bar.high_price
        || bar.volume < 0 || !std::isfinite(bar.open_interest) || bar.open_interest < 0.0) {
        throw std::invalid_argument("invalid offline bar at index " + std::to_string(index));
    }
    if (*bar.timestamp_kind == offline_bar_timestamp::open) {
        if (bar.timestamp_nanos > std::numeric_limits<int64_t>::max() - bar.duration_nanos) {
            throw std::overflow_error("offline bar close timestamp overflow");
        }
        return {index, bar.timestamp_nanos, bar.timestamp_nanos + bar.duration_nanos};
    }
    if (bar.timestamp_nanos < std::numeric_limits<int64_t>::min() + bar.duration_nanos) {
        throw std::overflow_error("offline bar open timestamp overflow");
    }
    return {index, bar.timestamp_nanos - bar.duration_nanos, bar.timestamp_nanos};
}

void reject_overlaps(std::map<market_key, std::vector<bar_interval>>& markets) {
    for (auto& [market, intervals] : markets) {
        std::sort(intervals.begin(), intervals.end(), [] (const auto& left, const auto& right) {
            return std::tie(left.open_nanos, left.close_nanos, left.index)
                 < std::tie(right.open_nanos, right.close_nanos, right.index);
        });
        for (size_t index = 1; index < intervals.size(); ++index) {
            if (intervals[index].open_nanos < intervals[index - 1].close_nanos) {
                throw std::invalid_argument("overlapping offline bars for "
                    + std::string(std::get<0>(market)) + ":" + std::string(std::get<1>(market)));
            }
        }
    }
}

} // namespace

std::vector<offline_bar_timeline_event> build_offline_bar_timeline(const std::vector<offline_bar_record>& records) {
    if (records.empty() || records.size() > std::numeric_limits<uint64_t>::max() / 2
        || records.size() > std::vector<offline_bar_timeline_event>{}.max_size() / 2) {
        throw std::invalid_argument("a bounded nonempty set of offline bars is required");
    }
    std::map<market_key, std::vector<bar_interval>> markets;
    std::vector<bar_interval> intervals;
    intervals.reserve(records.size());
    for (size_t index = 0; index < records.size(); ++index) {
        const auto interval = validate_bar(records[index], index);
        if (records[index].time_domain != records.front().time_domain) {
            throw std::invalid_argument("offline bars require one declared time domain");
        }
        intervals.push_back(interval);
        markets[{records[index].exchange_id, records[index].instrument_id}].push_back(interval);
    }
    reject_overlaps(markets);

    std::vector<offline_bar_timeline_event> events;
    events.reserve(records.size() * 2);
    for (const auto& interval : intervals) {
        const auto& bar = records[interval.index];
        events.push_back({
            .timestamp_nanos = interval.open_nanos,
            .exchange_id = bar.exchange_id,
            .instrument_id = bar.instrument_id,
            .timeframe = bar.timeframe,
            .trading_day = bar.trading_day,
            .source_index = interval.index,
            .data = offline_bar_open_data{.open_price = bar.open_price},
        });
        events.push_back({
            .timestamp_nanos = interval.close_nanos,
            .exchange_id = bar.exchange_id,
            .instrument_id = bar.instrument_id,
            .timeframe = bar.timeframe,
            .trading_day = bar.trading_day,
            .source_index = interval.index,
            .data = offline_bar_completed_data{
                .open_price = bar.open_price,
                .high_price = bar.high_price,
                .low_price = bar.low_price,
                .close_price = bar.close_price,
                .volume = bar.volume,
                .open_interest = bar.open_interest,
            },
        });
    }
    std::sort(events.begin(), events.end(), [] (const auto& left, const auto& right) {
        if (left.timestamp_nanos != right.timestamp_nanos) {
            return left.timestamp_nanos < right.timestamp_nanos;
        }
        const bool left_close = std::holds_alternative<offline_bar_completed_data>(left.data);
        const bool right_close = std::holds_alternative<offline_bar_completed_data>(right.data);
        if (left_close != right_close) {
            return left_close;
        }
        return std::tie(left.exchange_id, left.instrument_id, left.source_index)
             < std::tie(right.exchange_id, right.instrument_id, right.source_index);
    });
    for (size_t index = 0; index < events.size(); ++index) {
        events[index].sequence = index + 1;
    }
    return events;
}

} // namespace seastar::xtrader
