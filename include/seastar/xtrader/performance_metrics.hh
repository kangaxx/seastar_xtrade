/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace seastar::xtrader {

// Phase 6 metrics: bounded histograms and thread-safe CTP/reactor observations.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
struct latency_bucket {
    uint64_t count = 0;
    long double total_us = 0;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    std::array<uint64_t, 65> histogram{};

    void record(uint64_t us);
    [[nodiscard]] double avg_us() const;
    // Conservative upper bounds from log2 buckets, not exact quantiles.
    [[nodiscard]] uint64_t percentile(unsigned percent) const;
};

struct metrics_snapshot {
    std::string name;
    uint64_t total_events = 0;
    uint64_t dropped_events = 0;
    double avg_latency_us = 0.0;
    uint64_t max_latency_us = 0;
    uint64_t min_latency_us = 0;
    size_t queue_depth = 0;
    double error_rate = 0.0;
    uint64_t error_count = 0;
    uint64_t dropped_last_minute = 0;
    uint64_t p50_latency_us = 0;
    uint64_t p95_latency_us = 0;
    uint64_t p99_latency_us = 0;
    size_t peak_queue_depth = 0;
};

class performance_metrics {
public:
    using clock = std::chrono::steady_clock;
    performance_metrics() = default;
    static performance_metrics& instance();

    void record_md_latency_us(uint64_t us);
    void record_order_latency_us(uint64_t us);
    void record_trade_latency_us(uint64_t us);
    void record_dispatch_latency_us(uint64_t us);
    void record_drain_latency_us(uint64_t us);

    void record_md_dropped(uint64_t count = 1);
    void record_order_dropped(uint64_t count = 1);
    void record_trade_dropped(uint64_t count = 1);
    void record_latency(std::string_view component, uint64_t us);
    void record_dropped(std::string_view component, uint64_t count = 1,
                        clock::time_point now = clock::now());

    // Depth is the latest observed queue for this component, not a shard sum.
    void record_queue_depth(std::string_view name, size_t depth);

    void record_error(std::string_view category);
    void record_success(std::string_view category);

    std::vector<metrics_snapshot> snapshot_all(clock::time_point now = clock::now()) const;
    metrics_snapshot snapshot(std::string_view component,
                              clock::time_point now = clock::now()) const;
    metrics_snapshot snapshot_md() const;
    metrics_snapshot snapshot_order() const;
    metrics_snapshot snapshot_trade() const;
    metrics_snapshot snapshot_dispatch() const;
    metrics_snapshot snapshot_drain() const;

    void reset();

private:
    struct drop_bucket {
        int64_t second = -1;
        uint64_t count = 0;
    };
    struct component_metrics {
        latency_bucket latency;
        uint64_t dropped = 0;
        uint64_t errors = 0;
        uint64_t successes = 0;
        size_t queue_depth = 0;
        size_t peak_queue_depth = 0;
        // Include the boundary second: a conservative 60..61 second window.
        std::array<drop_bucket, 61> recent_drops{};
    };
    static size_t component_index(std::string_view name);
    metrics_snapshot snapshot_locked(size_t index, clock::time_point now) const;
    mutable std::mutex _mutex;
    std::array<component_metrics, 5> _components{};
};

} // namespace seastar::xtrader
