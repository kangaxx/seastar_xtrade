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

#include <seastar/core/sstring.hh>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace seastar::xtrader {

struct latency_bucket {
    uint64_t count = 0;
    uint64_t total_us = 0;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;

    void record(uint64_t us) {
        ++count;
        total_us += us;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    [[nodiscard]] double avg_us() const {
        return count > 0 ? static_cast<double>(total_us) / count : 0.0;
    }
};

struct metrics_snapshot {
    sstring name;
    uint64_t total_events = 0;
    uint64_t dropped_events = 0;
    double avg_latency_us = 0.0;
    uint64_t max_latency_us = 0;
    uint64_t min_latency_us = 0;
    size_t queue_depth = 0;
    double error_rate = 0.0;
};

class performance_metrics {
public:
    static performance_metrics& instance();

    void record_md_latency_us(uint64_t us);
    void record_order_latency_us(uint64_t us);
    void record_trade_latency_us(uint64_t us);
    void record_dispatch_latency_us(uint64_t us);
    void record_drain_latency_us(uint64_t us);

    void record_md_dropped(uint64_t count = 1);
    void record_order_dropped(uint64_t count = 1);
    void record_trade_dropped(uint64_t count = 1);

    void record_queue_depth(const sstring& name, size_t depth);

    void record_error(const sstring& category);

    std::vector<metrics_snapshot> snapshot_all() const;
    metrics_snapshot snapshot_md() const;
    metrics_snapshot snapshot_order() const;
    metrics_snapshot snapshot_trade() const;
    metrics_snapshot snapshot_dispatch() const;
    metrics_snapshot snapshot_drain() const;

    void reset();

private:
    performance_metrics() = default;

    mutable std::atomic<uint64_t> _md_dropped{0};
    mutable std::atomic<uint64_t> _order_dropped{0};
    mutable std::atomic<uint64_t> _trade_dropped{0};

    latency_bucket _md_latency;
    latency_bucket _order_latency;
    latency_bucket _trade_latency;
    latency_bucket _dispatch_latency;
    latency_bucket _drain_latency;

    std::atomic<uint64_t> _total_errors{0};
};

} // namespace seastar::xtrader
