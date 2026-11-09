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
    uint64_t max_p99_latency_us = 5000;
    uint64_t max_critical_dropped_per_min = 0;
};

struct health_report {
    std::string component;
    std::string status;
    metrics_snapshot metrics;
    bool within_threshold = true;
    std::string violation_detail;
    std::string checked_at;
    std::string source_env;
};

enum class health_status {
    healthy,
    degraded,
    unhealthy,
    unknown,
};

inline const char* to_string(health_status s) {
    switch (s) {
        case health_status::healthy:    return "healthy";
        case health_status::degraded:   return "degraded";
        case health_status::unhealthy:  return "unhealthy";
        case health_status::unknown:    return "unknown";
        default: return "unknown";
    }
}

class health_check {
public:
    // Phase 6 gates; source: user request, 2026-11-09, GitHub Copilot (Auto).
    explicit health_check(const std::string& source_env = "vm",
                          const performance_metrics& metrics = performance_metrics::instance());

    void set_gate_thresholds(const gate_threshold& t);
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
                               const std::string& component) const;

    gate_threshold _thresholds;
    std::string _source_env;
    const performance_metrics& _metrics;
};

} // namespace seastar::xtrader
