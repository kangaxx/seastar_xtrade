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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace seastar::xtrader {

// B4: correlate offline CTP query callbacks before any gateway integration.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class ctp_query_kind {
    instruments,
    account,
    positions,
    orders,
    trades,
};

enum class ctp_query_state {
    waiting,
    completed,
    failed,
    timed_out,
};

enum class ctp_query_observation {
    row_accepted,
    completed,
    failed,
    stale_session,
    late_response,
    duplicate_terminal,
};

struct ctp_query_spec {
    ctp_query_kind kind = ctp_query_kind::account;
    int request_id = 0;
    size_t minimum_rows = 0;
    size_t maximum_rows = 0;
    std::chrono::steady_clock::time_point deadline;
};

struct ctp_query_response {
    uint64_t session_generation = 0;
    ctp_query_kind kind = ctp_query_kind::account;
    int request_id = 0;
    bool has_row = false;
    bool is_last = false;
    int error_id = 0;
    sstring error_message;
};

struct ctp_query_summary {
    ctp_query_spec spec;
    ctp_query_state state = ctp_query_state::waiting;
    size_t rows = 0;
    int error_id = 0;
    sstring error_message;
};

// Owner-shard only. The caller supplies monotonically increasing observation
// times. Completion counts responses, not committed snapshot data; this
// object cannot authenticate a session or authorize trading.
class ctp_query_tracker {
public:
    using clock = std::chrono::steady_clock;

    ctp_query_tracker(std::vector<ctp_query_kind> required, size_t max_requests_per_session,
                      size_t max_rows_per_query);
    void start_session(uint64_t generation);
    void begin(ctp_query_spec spec, clock::time_point now);
    ctp_query_observation observe(const ctp_query_response& response, clock::time_point now);
    bool expire(clock::time_point now);

    [[nodiscard]] bool is_consistent() const noexcept { return _consistent; }
    [[nodiscard]] bool required_queries_completed() const noexcept;
    [[nodiscard]] const std::optional<ctp_query_summary>& active() const noexcept { return _active; }
    [[nodiscard]] std::optional<ctp_query_summary> latest(ctp_query_kind kind) const;
    [[nodiscard]] uint64_t session_generation() const noexcept { return _generation; }

private:
    struct retired_query {
        ctp_query_summary summary;
        bool empty_success_terminal = false;
    };

    [[noreturn]] void fail_protocol(const char* message);
    void observe_time(clock::time_point now);
    void retire_active(bool empty_success_terminal);
    ctp_query_observation observe_retired(const ctp_query_response& response);

    std::set<ctp_query_kind> _required;
    size_t _max_requests;
    size_t _max_rows;
    uint64_t _generation = 0;
    std::optional<ctp_query_summary> _active;
    std::map<int, retired_query> _retired;
    std::map<ctp_query_kind, ctp_query_summary> _latest;
    std::optional<clock::time_point> _last_time;
    bool _consistent = true;
};

} // namespace seastar::xtrader
