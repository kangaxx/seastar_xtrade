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
// Phase 6: explicit no-data status and complete latency/error/drop/queue gates.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/health_check.hh>

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace seastar::xtrader {
namespace {
std::string iso8601_now() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return result.str();
}
}

health_check::health_check(const std::string& source_env, const performance_metrics& metrics)
    : _source_env(source_env), _metrics(metrics) {}

void health_check::set_gate_thresholds(const gate_threshold& thresholds) {
    if (!std::isfinite(thresholds.max_avg_latency_us) || thresholds.max_avg_latency_us < 0
        || !std::isfinite(thresholds.max_error_rate) || thresholds.max_error_rate < 0
        || thresholds.max_error_rate > 1) {
        throw std::invalid_argument("health thresholds require nonnegative latency and error rate in [0, 1]");
    }
    _thresholds = thresholds;
}

health_report health_check::make_report(const metrics_snapshot& metrics,
                                       const std::string& component) const {
    health_report report;
    report.component = component;
    report.metrics = metrics;
    report.checked_at = iso8601_now();
    report.source_env = _source_env;
    std::ostringstream violations;
    unsigned failed = 0;
    auto check = [&](bool passed, const char* name, auto value, auto limit) {
        if (!passed) {
            if (failed++) violations << "; ";
            violations << name << "(" << value << " > " << limit << ")";
        }
    };
    check(metrics.avg_latency_us <= _thresholds.max_avg_latency_us,
          "avg_latency_us", metrics.avg_latency_us, _thresholds.max_avg_latency_us);
    check(metrics.p99_latency_us <= _thresholds.max_p99_latency_us,
          "p99_upper_us", metrics.p99_latency_us, _thresholds.max_p99_latency_us);
    check(metrics.error_rate <= _thresholds.max_error_rate,
          "error_rate", metrics.error_rate, _thresholds.max_error_rate);
    const auto drop_limit = metrics.name == "order" || metrics.name == "trade"
        ? _thresholds.max_critical_dropped_per_min : _thresholds.max_dropped_per_min;
    check(metrics.dropped_last_minute <= drop_limit,
          "dropped_last_minute", metrics.dropped_last_minute, drop_limit);
    check(metrics.queue_depth <= _thresholds.max_queue_depth,
          "queue_depth", metrics.queue_depth, _thresholds.max_queue_depth);
    report.within_threshold = metrics.total_events > 0 && failed == 0;
    report.status = failed > 1 ? "unhealthy" : failed ? "degraded"
        : metrics.total_events ? "healthy" : "unknown";
    report.violation_detail = failed ? violations.str()
        : metrics.total_events ? "" : "no latency samples; health is not established";
    return report;
}

health_report health_check::check_market_data() const {
    return make_report(_metrics.snapshot_md(), "market_data");
}
health_report health_check::check_order_path() const {
    return make_report(_metrics.snapshot_order(), "order_path");
}
health_report health_check::check_trade_path() const {
    return make_report(_metrics.snapshot_trade(), "trade_path");
}
health_report health_check::check_dispatch() const {
    return make_report(_metrics.snapshot_dispatch(), "dispatch");
}
health_report health_check::check_drain() const {
    return make_report(_metrics.snapshot_drain(), "drain");
}

std::vector<health_report> health_check::full_report() const {
    std::vector<health_report> reports;
    for (const auto& snapshot : _metrics.snapshot_all()) {
        reports.push_back(make_report(snapshot, snapshot.name));
    }
    return reports;
}

health_status health_check::aggregate_status() const {
    auto status = health_status::healthy;
    for (const auto& report : full_report()) {
        if (report.status == "unhealthy") return health_status::unhealthy;
        if (report.status == "degraded") status = health_status::degraded;
        else if (report.status == "unknown" && status == health_status::healthy) {
            status = health_status::unknown;
        }
    }
    return status;
}
} // namespace seastar::xtrader
