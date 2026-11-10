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

#include <seastar/xtrader/domain_types.hh>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace seastar::xtrader {

// B5: explicit source time/session boundaries, never inferred from wall-clock text.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
struct market_tick_input {
    uint64_t sequence = 0;
    sstring exchange_id;
    sstring time_domain;
    int64_t timestamp_ms = 0;
    int64_t session_begin_ms = 0;
    int64_t session_end_ms = 0;
    domain::market_data tick;
};

struct recorded_minute_bar {
    sstring exchange_id;
    sstring instrument_id;
    sstring trading_day;
    sstring time_domain;
    int64_t begin_ms = 0;
    int64_t end_ms = 0;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    int64_t observed_volume = 0;
    double open_interest = 0;
    bool is_complete = false;
};

struct normalized_market_tick {
    market_tick_input source;
    // Authoritative observed delta; source.tick.last_volume is not trusted.
    std::optional<int> incremental_volume;
    bool has_sequence_gap = false;
    std::optional<recorded_minute_bar> completed_bar;
};

// Serialized owner-shard component. Configured markets bound memory usage.
// Caller supplies a validated exchange calendar and one stable ordered feed.
class market_data_pipeline {
public:
    using market_key = std::pair<sstring, sstring>;

    market_data_pipeline(sstring time_domain, std::vector<market_key> markets);
    normalized_market_tick accept(market_tick_input input);
    // End-of-stream is not proof that the final minute was fully observed.
    std::vector<recorded_minute_bar> finish();

private:
    struct market_state {
        std::optional<market_tick_input> previous;
        std::optional<recorded_minute_bar> bar;
        bool has_volume_baseline = false;
    };

    void validate(const market_tick_input& input) const;
    static std::optional<int> volume_delta(const market_tick_input& current,
                                          const market_state& state, bool gap);
    sstring _time_domain;
    std::map<market_key, market_state> _markets;
    uint64_t _last_sequence = 0;
    bool _finished = false;
};

} // namespace seastar::xtrader
