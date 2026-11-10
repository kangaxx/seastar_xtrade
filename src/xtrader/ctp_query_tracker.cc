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
#include <seastar/xtrader/ctp_query_tracker.hh>

#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B4: unsupported query kinds and incomplete streams never imply readiness.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
bool valid_kind(ctp_query_kind kind) {
    switch (kind) {
    case ctp_query_kind::instruments:
    case ctp_query_kind::account:
    case ctp_query_kind::positions:
    case ctp_query_kind::orders:
    case ctp_query_kind::trades:
        return true;
    }
    return false;
}

class consistency_guard {
public:
    explicit consistency_guard(bool& consistent) : _consistent(consistent) {}
    ~consistency_guard() {
        if (!_committed) {
            _consistent = false;
        }
    }
    void commit() noexcept { _committed = true; }

private:
    bool& _consistent;
    bool _committed = false;
};

} // namespace

ctp_query_tracker::ctp_query_tracker(std::vector<ctp_query_kind> required, size_t max_requests_per_session,
                                     size_t max_rows_per_query)
    : _max_requests(max_requests_per_session), _max_rows(max_rows_per_query) {
    if (required.empty() || _max_requests == 0 || _max_rows == 0) {
        throw std::invalid_argument("explicit required queries and bounded query limits are required");
    }
    for (const auto kind : required) {
        if (!valid_kind(kind) || !_required.insert(kind).second) {
            throw std::invalid_argument("required CTP queries must be valid and unique");
        }
    }
    if (_required.size() > _max_requests) {
        throw std::invalid_argument("request limit cannot cover the required CTP queries");
    }
}

void ctp_query_tracker::start_session(uint64_t generation) {
    if (generation == 0 || generation <= _generation) {
        throw std::invalid_argument("CTP session generation must increase");
    }
    _generation = generation;
    _active.reset();
    _retired.clear();
    _latest.clear();
    _last_time.reset();
    _consistent = true;
}

void ctp_query_tracker::begin(ctp_query_spec spec, clock::time_point now) {
    if (!_consistent || _generation == 0) {
        throw std::logic_error("CTP query tracker requires a consistent session");
    }
    observe_time(now);
    if (_active && _active->state == ctp_query_state::waiting) {
        throw std::logic_error("only one CTP query may be in flight");
    }
    if (!valid_kind(spec.kind) || spec.request_id <= 0 || spec.minimum_rows > spec.maximum_rows
        || spec.maximum_rows > _max_rows || spec.deadline <= now
        || _retired.contains(spec.request_id) || _retired.size() >= _max_requests) {
        throw std::invalid_argument("invalid, reused or unbounded CTP query");
    }
    consistency_guard guard(_consistent);
    _latest.erase(spec.kind);
    _active = ctp_query_summary{.spec = std::move(spec)};
    guard.commit();
}

[[noreturn]] void ctp_query_tracker::fail_protocol(const char* message) {
    _consistent = false;
    throw std::runtime_error(message);
}

void ctp_query_tracker::observe_time(clock::time_point now) {
    if (_last_time && now < *_last_time) {
        fail_protocol("CTP query observation time moved backwards");
    }
    _last_time = now;
}

void ctp_query_tracker::retire_active(bool empty_success_terminal) {
    if (!_active || _active->state == ctp_query_state::waiting) {
        fail_protocol("cannot retire an unfinished CTP query");
    }
    consistency_guard guard(_consistent);
    const bool inserted = _retired.emplace(_active->spec.request_id,
                                            retired_query{*_active, empty_success_terminal}).second;
    if (!inserted) {
        fail_protocol("duplicate retired CTP request identifier");
    }
    _latest.insert_or_assign(_active->spec.kind, _active.value());
    guard.commit();
}

ctp_query_observation ctp_query_tracker::observe_retired(const ctp_query_response& response) {
    const auto it = _retired.find(response.request_id);
    if (it == _retired.end() || it->second.summary.spec.kind != response.kind) {
        fail_protocol("unmatched CTP query response");
    }
    if (it->second.summary.state != ctp_query_state::completed) {
        return ctp_query_observation::late_response;
    }
    if (it->second.empty_success_terminal && !response.has_row && response.is_last && response.error_id == 0) {
        return ctp_query_observation::duplicate_terminal;
    }
    fail_protocol("additional data after a completed CTP query");
}

ctp_query_observation ctp_query_tracker::observe(const ctp_query_response& response, clock::time_point now) {
    if (!_consistent || _generation == 0) {
        throw std::logic_error("CTP query tracker requires a consistent session");
    }
    observe_time(now);
    if (response.session_generation == 0) {
        fail_protocol("invalid CTP query session generation");
    }
    if (response.session_generation < _generation) {
        return ctp_query_observation::stale_session;
    }
    if (response.session_generation != _generation || response.request_id <= 0 || !valid_kind(response.kind)) {
        fail_protocol("invalid CTP query session or response identity");
    }
    if (!_active || response.request_id != _active->spec.request_id || _active->state != ctp_query_state::waiting) {
        return observe_retired(response);
    }
    if (response.kind != _active->spec.kind) {
        fail_protocol("CTP query response kind does not match its request");
    }
    if (now >= _active->spec.deadline) {
        expire(now);
        return ctp_query_observation::late_response;
    }

    consistency_guard guard(_consistent);
    if (response.error_id != 0) {
        _active->state = ctp_query_state::failed;
        _active->error_id = response.error_id;
        _active->error_message = response.error_message;
        retire_active(false);
        guard.commit();
        return ctp_query_observation::failed;
    }
    if (!response.has_row && !response.is_last) {
        fail_protocol("CTP query response has neither data nor a terminal marker");
    }
    if (response.has_row) {
        if (_active->rows >= _active->spec.maximum_rows) {
            fail_protocol("CTP query returned more rows than permitted");
        }
        ++_active->rows;
    }
    if (!response.is_last) {
        guard.commit();
        return ctp_query_observation::row_accepted;
    }
    if (_active->rows < _active->spec.minimum_rows) {
        _active->state = ctp_query_state::failed;
        _active->error_message = "CTP query returned fewer rows than required";
        retire_active(false);
        guard.commit();
        return ctp_query_observation::failed;
    }
    _active->state = ctp_query_state::completed;
    retire_active(!response.has_row);
    guard.commit();
    return ctp_query_observation::completed;
}

bool ctp_query_tracker::expire(clock::time_point now) {
    if (!_consistent || _generation == 0) {
        throw std::logic_error("CTP query tracker requires a consistent session");
    }
    observe_time(now);
    if (!_active || _active->state != ctp_query_state::waiting || now < _active->spec.deadline) {
        return false;
    }
    consistency_guard guard(_consistent);
    _active->state = ctp_query_state::timed_out;
    _active->error_message = "CTP query timed out";
    retire_active(false);
    guard.commit();
    return true;
}

bool ctp_query_tracker::required_queries_completed() const noexcept {
    if (!_consistent || _generation == 0 || (_active && _active->state == ctp_query_state::waiting)) {
        return false;
    }
    for (const auto kind : _required) {
        const auto result = _latest.find(kind);
        if (result == _latest.end() || result->second.state != ctp_query_state::completed) {
            return false;
        }
    }
    return true;
}

std::optional<ctp_query_summary> ctp_query_tracker::latest(ctp_query_kind kind) const {
    if (!valid_kind(kind)) {
        throw std::invalid_argument("invalid CTP query kind");
    }
    const auto result = _latest.find(kind);
    return result == _latest.end() ? std::nullopt : std::optional<ctp_query_summary>(result->second);
}

} // namespace seastar::xtrader
