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
// Phase 6: bounded, thread-safe metrics with explicit component attribution.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/performance_metrics.hh>

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace seastar::xtrader {
namespace {
constexpr std::array<std::string_view, 5> component_names{
    "market_data", "order", "trade", "dispatch", "drain"};
}

void latency_bucket::record(uint64_t us) {
    ++count;
    total_us += us;
    min_us = std::min(min_us, us);
    max_us = std::max(max_us, us);
    ++histogram[std::bit_width(us)];
}

double latency_bucket::avg_us() const {
    return count ? static_cast<double>(total_us / count) : 0.0;
}

uint64_t latency_bucket::percentile(unsigned percent) const {
    if (percent == 0 || percent > 100) {
        throw std::invalid_argument("percentile must be in [1, 100]");
    }
    if (!count) {
        return 0;
    }
    const uint64_t rank = (count / 100) * percent
        + ((count % 100) * percent + 99) / 100;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < histogram.size(); ++i) {
        cumulative += histogram[i];
        if (cumulative >= rank) {
            const auto upper = i == 64 ? UINT64_MAX : (uint64_t{1} << i) - 1;
            return std::min(upper, max_us);
        }
    }
    throw std::logic_error("latency histogram count mismatch");
}

performance_metrics& performance_metrics::instance() {
    static performance_metrics metrics;
    return metrics;
}

size_t performance_metrics::component_index(std::string_view name) {
    const auto it = std::find(component_names.begin(), component_names.end(), name);
    if (it == component_names.end()) {
        throw std::invalid_argument("unknown metrics component: " + std::string(name));
    }
    return static_cast<size_t>(it - component_names.begin());
}

void performance_metrics::record_latency(std::string_view component, uint64_t us) {
    const auto index = component_index(component);
    std::lock_guard lock(_mutex);
    _components[index].latency.record(us);
}

void performance_metrics::record_md_latency_us(uint64_t us) { record_latency("market_data", us); }
void performance_metrics::record_order_latency_us(uint64_t us) { record_latency("order", us); }
void performance_metrics::record_trade_latency_us(uint64_t us) { record_latency("trade", us); }
void performance_metrics::record_dispatch_latency_us(uint64_t us) { record_latency("dispatch", us); }
void performance_metrics::record_drain_latency_us(uint64_t us) { record_latency("drain", us); }

void performance_metrics::record_dropped(std::string_view component, uint64_t count,
                                        clock::time_point now) {
    const auto index = component_index(component);
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (second < 0) {
        throw std::invalid_argument("metrics timestamp must be nonnegative");
    }
    std::lock_guard lock(_mutex);
    auto& metrics = _components[index];
    metrics.dropped += count;
    auto& bucket = metrics.recent_drops[static_cast<size_t>(second) % metrics.recent_drops.size()];
    if (bucket.second > second) {
        return;
    }
    if (bucket.second != second) {
        bucket = {second, 0};
    }
    bucket.count += count;
}

void performance_metrics::record_md_dropped(uint64_t count) { record_dropped("market_data", count); }
void performance_metrics::record_order_dropped(uint64_t count) { record_dropped("order", count); }
void performance_metrics::record_trade_dropped(uint64_t count) { record_dropped("trade", count); }

void performance_metrics::record_queue_depth(std::string_view name, size_t depth) {
    const auto index = component_index(name);
    std::lock_guard lock(_mutex);
    auto& metrics = _components[index];
    metrics.queue_depth = depth;
    metrics.peak_queue_depth = std::max(metrics.peak_queue_depth, depth);
}

void performance_metrics::record_error(std::string_view category) {
    const auto index = component_index(category);
    std::lock_guard lock(_mutex);
    ++_components[index].errors;
}

void performance_metrics::record_success(std::string_view category) {
    const auto index = component_index(category);
    std::lock_guard lock(_mutex);
    ++_components[index].successes;
}

metrics_snapshot performance_metrics::snapshot_locked(size_t index, clock::time_point now) const {
    const auto& metrics = _components[index];
    const auto& latency = metrics.latency;
    metrics_snapshot result;
    result.name = component_names[index];
    result.total_events = latency.count;
    result.dropped_events = metrics.dropped;
    result.error_count = metrics.errors;
    const long double outcomes = static_cast<long double>(metrics.successes) + metrics.errors;
    result.error_rate = outcomes ? static_cast<double>(metrics.errors / outcomes) : 0.0;
    result.avg_latency_us = latency.avg_us();
    result.min_latency_us = latency.count ? latency.min_us : 0;
    result.max_latency_us = latency.max_us;
    result.p50_latency_us = latency.percentile(50);
    result.p95_latency_us = latency.percentile(95);
    result.p99_latency_us = latency.percentile(99);
    result.queue_depth = metrics.queue_depth;
    result.peak_queue_depth = metrics.peak_queue_depth;
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    for (const auto& bucket : metrics.recent_drops) {
        if (bucket.second >= 0 && bucket.second <= second && second - bucket.second <= 60) {
            result.dropped_last_minute += bucket.count;
        }
    }
    return result;
}

metrics_snapshot performance_metrics::snapshot(std::string_view component, clock::time_point now) const {
    const auto index = component_index(component);
    std::lock_guard lock(_mutex);
    return snapshot_locked(index, now);
}

std::vector<metrics_snapshot> performance_metrics::snapshot_all(clock::time_point now) const {
    std::lock_guard lock(_mutex);
    std::vector<metrics_snapshot> result;
    for (size_t i = 0; i < _components.size(); ++i) {
        result.push_back(snapshot_locked(i, now));
    }
    return result;
}

metrics_snapshot performance_metrics::snapshot_md() const { return snapshot("market_data"); }
metrics_snapshot performance_metrics::snapshot_order() const { return snapshot("order"); }
metrics_snapshot performance_metrics::snapshot_trade() const { return snapshot("trade"); }
metrics_snapshot performance_metrics::snapshot_dispatch() const { return snapshot("dispatch"); }
metrics_snapshot performance_metrics::snapshot_drain() const { return snapshot("drain"); }

void performance_metrics::reset() {
    std::lock_guard lock(_mutex);
    _components = {};
}
} // namespace seastar::xtrader
