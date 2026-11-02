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
#include <seastar/xtrader/performance_metrics.hh>

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace seastar::xtrader {

struct gate_threshold {
    double max_avg_latency_us = 1000.0;
    double max_error_rate = 0.001;
    uint64_t max_dropped_per_min = 100;
    size_t max_queue_depth = 65536;
};

struct health_report {
    sstring component;
    sstring status;
    metrics_snapshot metrics;
    bool within_threshold = true;
    sstring violation_detail;
    sstring checked_at;
    sstring source_env;
};

enum class health_status {
    healthy,
    degraded,
    unhealthy,
};

inline const char* to_string(health_status s) {
    switch (s) {
        case health_status::healthy:    return "healthy";
        case health_status::degraded:   return "degraded";
        case health_status::unhealthy:  return "unhealthy";
        default: return "unknown";
    }
}

class health_check {
public:
    health_check(const sstring& source_env = "vm");

    void set_gate_thresholds(const gate_threshold& t) { _thresholds = t; }
    const gate_threshold& thresholds() const noexcept { return _thresholds; }

    health_report check_market_data() const;
    health_report check_order_path() const;
    health_report check_trade_path() const;
    health_report check_dispatch() const;
    health_report check_drain() const;

    health_status aggregate_status() const;

    std::vector<health_report> full_report() const;

private:
    health_report make_report(const metrics_snapshot& m,
                               const sstring& component) const;

    gate_threshold _thresholds;
    sstring _source_env;
};

} // namespace seastar::xtrader
