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
#pragma once

#include <seastar/core/sstring.hh>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace seastar::xtrader {

// B3: split bar opens and completions without exposing future OHLC at the open.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class offline_bar_timestamp {
    open,
    close,
};

struct offline_bar_record {
    sstring exchange_id;
    sstring instrument_id;
    sstring timeframe;
    sstring trading_day;
    sstring time_domain;
    int64_t timestamp_nanos = 0;
    int64_t duration_nanos = 0;
    std::optional<offline_bar_timestamp> timestamp_kind;
    double open_price = 0.0;
    double high_price = 0.0;
    double low_price = 0.0;
    double close_price = 0.0;
    int volume = -1;
    double open_interest = -1.0;
};

struct offline_bar_open_data {
    double open_price = 0.0;
};

struct offline_bar_completed_data {
    double open_price = 0.0;
    double high_price = 0.0;
    double low_price = 0.0;
    double close_price = 0.0;
    int volume = 0;
    double open_interest = 0.0;
};

struct offline_bar_timeline_event {
    uint64_t sequence = 0;
    int64_t timestamp_nanos = 0;
    sstring exchange_id;
    sstring instrument_id;
    sstring timeframe;
    sstring trading_day;
    size_t source_index = 0;
    std::variant<offline_bar_open_data, offline_bar_completed_data> data;
};

// Timestamps must already use the declared, caller-verified time domain;
// trading-day labels are supplied by the caller, not derived from timestamps.
// At an equal timestamp, completed bars precede every new bar open.
[[nodiscard]] std::vector<offline_bar_timeline_event>
build_offline_bar_timeline(const std::vector<offline_bar_record>& records);

} // namespace seastar::xtrader
