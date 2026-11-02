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
#include <seastar/xtrader/health_check.hh>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace seastar::xtrader {

namespace {

std::string iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // anonymous namespace

health_check::health_check(const sstring& source_env)
    : _source_env(source_env)
{}

health_report health_check::make_report(const metrics_snapshot& m,
                                          const sstring& component) const {
    health_report r;
    r.component = component;
    r.metrics = m;
    r.checked_at = iso8601_now();
    r.source_env = _source_env;

    bool latency_ok = m.avg_latency_us <= _thresholds.max_avg_latency_us;
    bool error_ok = m.error_rate <= _thresholds.max_error_rate;

    if (latency_ok && error_ok) {
        r.status = "healthy";
        r.within_threshold = true;
    } else if (!latency_ok && !error_ok) {
        r.status = "unhealthy";
        r.within_threshold = false;
        r.violation_detail = "latency(" + to_sstring(m.avg_latency_us)
            + "us > " + to_sstring(_thresholds.max_avg_latency_us)
            + "us) error_rate(" + to_sstring(m.error_rate)
            + " > " + to_sstring(_thresholds.max_error_rate) + ")";
    } else if (!latency_ok) {
        r.status = "degraded";
        r.within_threshold = false;
        r.violation_detail = "latency(" + to_sstring(m.avg_latency_us)
            + "us > " + to_sstring(_thresholds.max_avg_latency_us) + "us)";
    } else {
        r.status = "degraded";
        r.within_threshold = false;
        r.violation_detail = "error_rate(" + to_sstring(m.error_rate)
            + " > " + to_sstring(_thresholds.max_error_rate) + ")";
    }

    return r;
}

health_report health_check::check_market_data() const {
    return make_report(performance_metrics::instance().snapshot_md(),
                        "market_data");
}

health_report health_check::check_order_path() const {
    return make_report(performance_metrics::instance().snapshot_order(),
                        "order_path");
}

health_report health_check::check_trade_path() const {
    return make_report(performance_metrics::instance().snapshot_trade(),
                        "trade_path");
}

health_report health_check::check_dispatch() const {
    return make_report(performance_metrics::instance().snapshot_dispatch(),
                        "dispatch");
}

health_report health_check::check_drain() const {
    return make_report(performance_metrics::instance().snapshot_drain(),
                        "drain");
}

health_status health_check::aggregate_status() const {
    bool has_unhealthy = false;
    bool has_degraded = false;

    auto reports = full_report();
    for (const auto& r : reports) {
        if (r.status == "unhealthy") has_unhealthy = true;
        if (r.status == "degraded") has_degraded = true;
    }

    if (has_unhealthy) return health_status::unhealthy;
    if (has_degraded) return health_status::degraded;
    return health_status::healthy;
}

std::vector<health_report> health_check::full_report() const {
    return {
        check_market_data(),
        check_order_path(),
        check_trade_path(),
        check_dispatch(),
        check_drain(),
    };
}

} // namespace seastar::xtrader
