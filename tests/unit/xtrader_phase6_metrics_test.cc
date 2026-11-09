// Phase 6 metrics/gate regression tests, independent of CTP and the reactor.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/health_check.hh>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace seastar::xtrader;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Func>
void require_invalid(Func&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("expected invalid_argument");
}

void test_histogram() {
    latency_bucket bucket;
    require(bucket.percentile(99) == 0, "empty percentile");
    for (uint64_t value : {0, 1, 2, 3, 4, 8}) bucket.record(value);
    require(bucket.count == 6 && bucket.min_us == 0 && bucket.max_us == 8, "latency bounds");
    require(bucket.avg_us() == 3 && bucket.percentile(50) == 3, "average and p50");
    require(bucket.percentile(95) == 8 && bucket.percentile(99) == 8, "tail clamped to max");
    bucket.record(UINT64_MAX);
    require(bucket.percentile(100) == UINT64_MAX, "overflow bucket");
    require_invalid([&] { (void)bucket.percentile(0); });
    require_invalid([&] { (void)bucket.percentile(101); });
    latency_bucket bounded;
    for (unsigned i = 0; i < 100; ++i) bounded.record(i);
    require(bounded.percentile(50) == 63 && bounded.percentile(95) == 99,
            "quantiles are conservative log2 upper bounds");
}

void test_metrics() {
    performance_metrics metrics;
    const auto at = performance_metrics::clock::time_point(std::chrono::seconds(100));
    metrics.record_dropped("market_data", 3, at);
    metrics.record_dropped("market_data", 2, at + std::chrono::seconds(1));
    require(metrics.snapshot("market_data", at + std::chrono::seconds(60)).dropped_last_minute == 5,
            "boundary second retained");
    const auto expired = metrics.snapshot("market_data", at + std::chrono::seconds(61));
    require(expired.dropped_last_minute == 2 && expired.dropped_events == 5, "rolling versus lifetime");
    require(metrics.snapshot("market_data", at + std::chrono::seconds(62)).dropped_last_minute == 0,
            "window expiry");
    metrics.record_dropped("market_data", 7, at + std::chrono::seconds(61));
    require(metrics.snapshot("market_data", at + std::chrono::seconds(61)).dropped_last_minute == 9,
            "window slot reuse");
    metrics.record_dropped("market_data", 1, at);
    require(metrics.snapshot("market_data", at + std::chrono::seconds(61)).dropped_last_minute == 9,
            "delayed old observation does not overwrite the current window");
    metrics.record_error("order");
    for (unsigned i = 0; i < 3; ++i) metrics.record_success("order");
    require(metrics.snapshot_order().error_rate == 0.25, "component outcome denominator");
    require(metrics.snapshot_trade().error_rate == 0, "errors isolated by component");
    metrics.record_queue_depth("dispatch", 10);
    metrics.record_queue_depth("dispatch", 2);
    const auto queue = metrics.snapshot_dispatch();
    require(queue.queue_depth == 2 && queue.peak_queue_depth == 10, "current and peak depth");
    require_invalid([&] { metrics.record_error("misspelled"); });
    metrics.reset();
    require(metrics.snapshot_md().dropped_events == 0
        && metrics.snapshot_order().error_count == 0
        && metrics.snapshot_dispatch().peak_queue_depth == 0, "reset all state");
}

void test_health() {
    performance_metrics metrics;
    health_check health("test", metrics);
    require(health.aggregate_status() == health_status::unknown, "no data cannot pass");
    require(!health.check_market_data().within_threshold, "unknown gate fails closed");
    for (const char* name : {"market_data", "order", "trade", "dispatch", "drain"}) {
        metrics.record_latency(name, 1000);
        metrics.record_success(name);
    }
    gate_threshold thresholds;
    thresholds.max_avg_latency_us = 1000;
    thresholds.max_p99_latency_us = 1000;
    thresholds.max_error_rate = 0.5;
    thresholds.max_dropped_per_min = 1;
    thresholds.max_queue_depth = 2;
    health.set_gate_thresholds(thresholds);
    metrics.record_error("market_data");
    metrics.record_md_dropped();
    metrics.record_queue_depth("market_data", 2);
    require(health.aggregate_status() == health_status::healthy, "inclusive threshold boundaries");
    metrics.record_queue_depth("market_data", 3);
    require(health.check_market_data().status == "degraded", "queue gate");
    metrics.record_md_dropped();
    require(health.check_market_data().status == "unhealthy", "multiple violations");
    require(health.check_market_data().violation_detail.find("dropped_last_minute") != std::string::npos,
            "drop violation explained");
    metrics.reset();
    metrics.record_md_latency_us(1001);
    require(health.check_market_data().violation_detail.find("p99_upper_us") != std::string::npos,
            "p99 gate enforced");
    thresholds.max_avg_latency_us = std::numeric_limits<double>::quiet_NaN();
    require_invalid([&] { health.set_gate_thresholds(thresholds); });
    thresholds.max_avg_latency_us = 1000;
    thresholds.max_error_rate = 1.1;
    require_invalid([&] { health.set_gate_thresholds(thresholds); });
    metrics.reset();
    metrics.record_md_dropped(101);
    require(health.check_market_data().status != "unknown", "no samples do not hide known loss");
    metrics.record_order_dropped();
    require(health.check_order_path().status == "degraded", "order loss defaults to zero tolerance");
}

void test_concurrent_recording() {
    performance_metrics metrics;
    std::vector<std::thread> writers;
    for (unsigned i = 0; i < 4; ++i) {
        writers.emplace_back([&metrics] {
            for (unsigned j = 0; j < 1000; ++j) {
                metrics.record_md_latency_us(j);
                metrics.record_success("market_data");
                metrics.record_md_dropped();
            }
        });
    }
    for (unsigned i = 0; i < 100; ++i) {
        const auto snapshot = metrics.snapshot_all();
        require(snapshot.size() == 5, "concurrent snapshots");
    }
    for (auto& writer : writers) writer.join();
    const auto result = metrics.snapshot_md();
    require(result.total_events == 4000 && result.dropped_events == 4000, "thread-safe counts");
    require(result.avg_latency_us == 499.5, "thread-safe histogram");
}
}

int main() {
    try {
        test_histogram();
        test_metrics();
        test_health();
        test_concurrent_recording();
        std::cout << "[OK] Phase 6 metrics/gate regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Phase 6 regression: " << error.what() << '\n';
        return 1;
    }
}
