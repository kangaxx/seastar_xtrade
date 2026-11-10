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
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/ctp_query_tracker.hh>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace {

using namespace seastar::xtrader;
using clock = ctp_query_tracker::clock;
using namespace std::chrono_literals;

// B4: synthetic callbacks cover query tracking without loading or contacting CTP.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
ctp_query_spec query(ctp_query_kind kind, int request_id, size_t minimum_rows = 0, size_t maximum_rows = 8) {
    return {
        .kind = kind,
        .request_id = request_id,
        .minimum_rows = minimum_rows,
        .maximum_rows = maximum_rows,
        .deadline = clock::time_point{} + 10s,
    };
}

ctp_query_response response(uint64_t generation, ctp_query_kind kind, int request_id,
                            bool has_row, bool is_last, int error_id = 0) {
    return {
        .session_generation = generation,
        .kind = kind,
        .request_id = request_id,
        .has_row = has_row,
        .is_last = is_last,
        .error_id = error_id,
        .error_message = error_id == 0 ? "" : "synthetic CTP error",
    };
}

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_queries_require_every_complete_bounded_stream) {
    ctp_query_tracker tracker({ctp_query_kind::instruments, ctp_query_kind::account,
                               ctp_query_kind::positions, ctp_query_kind::orders,
                               ctp_query_kind::trades}, 8, 8);
    tracker.start_session(1);
    const auto now = clock::time_point{};
    tracker.begin(query(ctp_query_kind::instruments, 1, 1), now);
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::instruments, 1, true, false), now)
                == ctp_query_observation::row_accepted);
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::instruments, 1, true, true), now)
                == ctp_query_observation::completed);
    BOOST_REQUIRE(tracker.latest(ctp_query_kind::instruments));
    BOOST_CHECK_EQUAL(tracker.latest(ctp_query_kind::instruments)->rows, 2);

    tracker.begin(query(ctp_query_kind::account, 2, 1, 1), now);
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::account, 2, true, true), now)
                == ctp_query_observation::completed);
    BOOST_CHECK(!tracker.required_queries_completed());

    for (const auto [kind, request_id] : {
             std::pair{ctp_query_kind::positions, 3}, std::pair{ctp_query_kind::orders, 4},
             std::pair{ctp_query_kind::trades, 5}}) {
        tracker.begin(query(kind, request_id), now);
        BOOST_CHECK(tracker.observe(response(1, kind, request_id, false, true), now)
                    == ctp_query_observation::completed);
    }
    BOOST_CHECK(tracker.is_consistent());
    BOOST_CHECK(tracker.required_queries_completed());
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::orders, 4, false, true), now)
                == ctp_query_observation::duplicate_terminal);

    tracker.begin(query(ctp_query_kind::account, 6, 1, 1), now);
    BOOST_CHECK(!tracker.required_queries_completed());
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::account, 6, false, true), now)
                == ctp_query_observation::failed);
    BOOST_CHECK(!tracker.required_queries_completed());
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_queries_retry_errors_and_timeouts_without_reusing_ids) {
    ctp_query_tracker tracker({ctp_query_kind::account}, 4, 4);
    tracker.start_session(7);
    const auto now = clock::time_point{};
    tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now);
    BOOST_CHECK(tracker.observe(response(7, ctp_query_kind::account, 1, false, true, 42), now)
                == ctp_query_observation::failed);
    BOOST_REQUIRE(tracker.latest(ctp_query_kind::account));
    BOOST_CHECK_EQUAL(tracker.latest(ctp_query_kind::account)->error_id, 42);
    BOOST_CHECK_THROW(tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now),
                      std::invalid_argument);

    tracker.begin(query(ctp_query_kind::account, 2, 1, 1), now);
    BOOST_CHECK(!tracker.expire(now + 9s));
    BOOST_CHECK(tracker.expire(now + 10s));
    BOOST_CHECK(tracker.latest(ctp_query_kind::account)->state == ctp_query_state::timed_out);
    auto retry = query(ctp_query_kind::account, 3, 1, 1);
    retry.deadline += 10s;
    tracker.begin(retry, now + 10s);
    BOOST_CHECK(tracker.observe(response(7, ctp_query_kind::account, 2, true, true), now + 11s)
                == ctp_query_observation::late_response);
    BOOST_CHECK(tracker.observe(response(7, ctp_query_kind::account, 3, true, true), now + 11s)
                == ctp_query_observation::completed);
    BOOST_CHECK(tracker.required_queries_completed());

    tracker.start_session(8);
    BOOST_CHECK(!tracker.required_queries_completed());
    BOOST_CHECK(tracker.observe(response(7, ctp_query_kind::account, 3, true, true), now)
                == ctp_query_observation::stale_session);
    BOOST_CHECK(tracker.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_queries_fail_closed_on_invalid_or_extra_callbacks) {
    const auto now = clock::time_point{};
    ctp_query_tracker tracker({ctp_query_kind::account}, 3, 2);
    tracker.start_session(1);
    tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now);
    BOOST_CHECK_THROW(tracker.begin(query(ctp_query_kind::account, 2, 1, 1), now),
                      std::logic_error);
    BOOST_CHECK(tracker.observe(response(1, ctp_query_kind::account, 1, true, true), now)
                == ctp_query_observation::completed);
    BOOST_CHECK_THROW(tracker.observe(response(1, ctp_query_kind::account, 1, true, true), now),
                      std::runtime_error);
    BOOST_CHECK(!tracker.is_consistent());
    BOOST_CHECK(!tracker.required_queries_completed());
    BOOST_CHECK_THROW(tracker.begin(query(ctp_query_kind::account, 2, 1, 1), now),
                      std::logic_error);

    tracker.start_session(2);
    tracker.begin(query(ctp_query_kind::account, 1, 0, 1), now);
    BOOST_CHECK(tracker.observe(response(2, ctp_query_kind::account, 1, true, false), now)
                == ctp_query_observation::row_accepted);
    BOOST_CHECK_THROW(tracker.observe(response(2, ctp_query_kind::account, 1, true, true), now),
                      std::runtime_error);
    BOOST_CHECK(!tracker.is_consistent());
}

SEASTAR_THREAD_TEST_CASE(xtrader_ctp_queries_reject_bad_config_and_protocol_identity) {
    const auto now = clock::time_point{};
    BOOST_CHECK_THROW((ctp_query_tracker({}, 2, 2)), std::invalid_argument);
    BOOST_CHECK_THROW((ctp_query_tracker({ctp_query_kind::account, ctp_query_kind::account}, 2, 2)),
                      std::invalid_argument);
    BOOST_CHECK_THROW((ctp_query_tracker({ctp_query_kind::account, ctp_query_kind::positions}, 1, 2)),
                      std::invalid_argument);
    ctp_query_tracker tracker({ctp_query_kind::account}, 2, 2);
    BOOST_CHECK_THROW(tracker.start_session(0), std::invalid_argument);
    tracker.start_session(1);
    BOOST_CHECK_THROW(tracker.start_session(1), std::invalid_argument);
    BOOST_CHECK_THROW(tracker.begin(query(ctp_query_kind::account, 0, 1, 1), now),
                      std::invalid_argument);
    tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now);
    BOOST_CHECK_THROW(tracker.observe(response(1, ctp_query_kind::positions, 1, true, true), now),
                      std::runtime_error);
    BOOST_CHECK(!tracker.is_consistent());
    tracker.start_session(2);
    tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now);
    BOOST_CHECK_THROW(tracker.observe(response(2, ctp_query_kind::account, 1, false, false), now),
                      std::runtime_error);
    BOOST_CHECK(!tracker.is_consistent());
    tracker.start_session(3);
    tracker.begin(query(ctp_query_kind::account, 1, 1, 1), now + 1s);
    BOOST_CHECK_THROW(tracker.observe(response(3, ctp_query_kind::account, 1, true, true), now),
                      std::runtime_error);
    BOOST_CHECK(!tracker.is_consistent());
}
