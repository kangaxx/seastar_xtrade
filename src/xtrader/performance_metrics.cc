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
#include <seastar/xtrader/performance_metrics.hh>

#include <algorithm>

namespace seastar::xtrader {

performance_metrics& performance_metrics::instance() {
    static performance_metrics inst;
    return inst;
}

void performance_metrics::record_md_latency_us(uint64_t us) {
    _md_latency.record(us);
}

void performance_metrics::record_order_latency_us(uint64_t us) {
    _order_latency.record(us);
}

void performance_metrics::record_trade_latency_us(uint64_t us) {
    _trade_latency.record(us);
}

void performance_metrics::record_dispatch_latency_us(uint64_t us) {
    _dispatch_latency.record(us);
}

void performance_metrics::record_drain_latency_us(uint64_t us) {
    _drain_latency.record(us);
}

void performance_metrics::record_md_dropped(uint64_t count) {
    _md_dropped.fetch_add(count);
}

void performance_metrics::record_order_dropped(uint64_t count) {
    _order_dropped.fetch_add(count);
}

void performance_metrics::record_trade_dropped(uint64_t count) {
    _trade_dropped.fetch_add(count);
}

void performance_metrics::record_error(const sstring&) {
    _total_errors.fetch_add(1);
}

void performance_metrics::record_queue_depth(const sstring&, size_t) {
}

metrics_snapshot performance_metrics::snapshot_md() const {
    metrics_snapshot s;
    s.name = "market_data";
    s.total_events = _md_latency.count;
    s.dropped_events = _md_dropped.load();
    s.avg_latency_us = _md_latency.avg_us();
    s.min_latency_us = _md_latency.min_us;
    s.max_latency_us = _md_latency.max_us;
    s.error_rate = s.total_events > 0
        ? static_cast<double>(_total_errors.load()) / s.total_events : 0.0;
    return s;
}

metrics_snapshot performance_metrics::snapshot_order() const {
    metrics_snapshot s;
    s.name = "order";
    s.total_events = _order_latency.count;
    s.dropped_events = _order_dropped.load();
    s.avg_latency_us = _order_latency.avg_us();
    s.min_latency_us = _order_latency.min_us;
    s.max_latency_us = _order_latency.max_us;
    return s;
}

metrics_snapshot performance_metrics::snapshot_trade() const {
    metrics_snapshot s;
    s.name = "trade";
    s.total_events = _trade_latency.count;
    s.dropped_events = _trade_dropped.load();
    s.avg_latency_us = _trade_latency.avg_us();
    s.min_latency_us = _trade_latency.min_us;
    s.max_latency_us = _trade_latency.max_us;
    return s;
}

metrics_snapshot performance_metrics::snapshot_dispatch() const {
    metrics_snapshot s;
    s.name = "dispatch";
    s.total_events = _dispatch_latency.count;
    s.avg_latency_us = _dispatch_latency.avg_us();
    s.min_latency_us = _dispatch_latency.min_us;
    s.max_latency_us = _dispatch_latency.max_us;
    return s;
}

metrics_snapshot performance_metrics::snapshot_drain() const {
    metrics_snapshot s;
    s.name = "drain";
    s.total_events = _drain_latency.count;
    s.avg_latency_us = _drain_latency.avg_us();
    s.min_latency_us = _drain_latency.min_us;
    s.max_latency_us = _drain_latency.max_us;
    return s;
}

std::vector<metrics_snapshot> performance_metrics::snapshot_all() const {
    return {
        snapshot_md(),
        snapshot_order(),
        snapshot_trade(),
        snapshot_dispatch(),
        snapshot_drain(),
    };
}

void performance_metrics::reset() {
    _md_dropped.store(0);
    _order_dropped.store(0);
    _trade_dropped.store(0);
    _total_errors.store(0);
    _md_latency = latency_bucket{};
    _order_latency = latency_bucket{};
    _trade_latency = latency_bucket{};
    _dispatch_latency = latency_bucket{};
    _drain_latency = latency_bucket{};
}

} // namespace seastar::xtrader
