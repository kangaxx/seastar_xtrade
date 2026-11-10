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
#include <seastar/xtrader/bounded_callback_queue.hh>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace {

// B4: use synthetic POD events; these tests never instantiate a CTP API.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct callback_slot {
    unsigned producer = 0;
    unsigned sequence = 0;
};

} // namespace

SEASTAR_THREAD_TEST_CASE(xtrader_callback_queue_preserves_events_and_sticks_on_loss) {
    seastar::xtrader::bounded_callback_queue<callback_slot, 4> queue;
    callback_slot output;
    BOOST_CHECK(!queue.try_pop(output));
    for (unsigned sequence = 0; sequence < 4; ++sequence) {
        BOOST_REQUIRE(queue.try_push(callback_slot{.producer = 1, .sequence = sequence}));
    }
    BOOST_CHECK(!queue.try_push(callback_slot{.producer = 1, .sequence = 4}));
    BOOST_CHECK(queue.has_lost_event());
    for (unsigned sequence = 0; sequence < 4; ++sequence) {
        BOOST_REQUIRE(queue.try_pop(output));
        BOOST_CHECK_EQUAL(output.producer, 1);
        BOOST_CHECK_EQUAL(output.sequence, sequence);
    }
    BOOST_CHECK(!queue.try_pop(output));
    BOOST_REQUIRE(queue.try_push(callback_slot{.producer = 2, .sequence = 0}));
    BOOST_REQUIRE(queue.try_pop(output));
    BOOST_CHECK_EQUAL(output.producer, 2);
    BOOST_CHECK(queue.has_lost_event());
}

SEASTAR_THREAD_TEST_CASE(xtrader_callback_queue_serializes_concurrent_producers) {
    constexpr unsigned producer_count = 4;
    constexpr unsigned events_per_producer = 256;
    seastar::xtrader::bounded_callback_queue<callback_slot, 2048> queue;
    std::atomic<bool> push_failed{false};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (unsigned producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &queue, &push_failed] {
            for (unsigned sequence = 0; sequence < events_per_producer; ++sequence) {
                if (!queue.try_push(callback_slot{.producer = producer, .sequence = sequence})) {
                    push_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    BOOST_REQUIRE(!push_failed.load(std::memory_order_relaxed));
    BOOST_REQUIRE(!queue.has_lost_event());

    std::array<unsigned, producer_count> next{};
    callback_slot output;
    unsigned count = 0;
    while (queue.try_pop(output)) {
        BOOST_REQUIRE_LT(output.producer, producer_count);
        BOOST_CHECK_EQUAL(output.sequence, next[output.producer]++);
        ++count;
    }
    BOOST_CHECK_EQUAL(count, producer_count * events_per_producer);
}
