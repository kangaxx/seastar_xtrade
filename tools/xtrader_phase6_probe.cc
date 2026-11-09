// Phase 6 offline synthetic queue probe; never connects to Redis, CTP or a broker.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/health_check.hh>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <filesystem>
#include <sys/resource.h>
#endif

namespace {
using namespace seastar::xtrader;
using clock_type = performance_metrics::clock;

struct options {
    uint64_t seconds = 5;
    uint64_t rate = 1000;
    uint64_t capacity = 65536;
    uint64_t delay_us = 0;
    uint64_t error_every = 0;
    uint64_t drain_seconds = 5;
    std::string mode = "mixed";
    gate_threshold thresholds;
};

uint64_t number(std::string_view text, uint64_t maximum) {
    uint64_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > maximum) {
        throw std::invalid_argument("invalid numeric argument: " + std::string(text));
    }
    return value;
}

options parse_options(int argc, char** argv) {
    options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (++i == argc) throw std::invalid_argument("missing value for " + std::string(key));
        const std::string_view value(argv[i]);
        if (key == "--seconds") result.seconds = number(value, 604800);
        else if (key == "--rate") result.rate = number(value, 10000000);
        else if (key == "--capacity") result.capacity = number(value, 1000000);
        else if (key == "--delay-us") result.delay_us = number(value, 1000000);
        else if (key == "--error-every") result.error_every = number(value, UINT64_MAX);
        else if (key == "--drain-seconds") result.drain_seconds = number(value, 3600);
        else if (key == "--mode") result.mode = value;
        else if (key == "--max-avg-us") result.thresholds.max_avg_latency_us = number(value, UINT64_MAX);
        else if (key == "--max-p99-us") result.thresholds.max_p99_latency_us = number(value, UINT64_MAX);
        else if (key == "--max-drops") result.thresholds.max_dropped_per_min = number(value, UINT64_MAX);
        else if (key == "--max-queue") result.thresholds.max_queue_depth = number(value, 1000000);
        else throw std::invalid_argument("unknown option: " + std::string(key));
    }
    if (!result.seconds || !result.rate || !result.capacity || !result.drain_seconds
        || (result.mode != "market_data" && result.mode != "order" && result.mode != "mixed")) {
        throw std::invalid_argument("positive seconds/rate/capacity and mode market_data|order|mixed required");
    }
    return result;
}

bool required(const options& config, const std::string& component) {
    return config.mode == "mixed" || component == config.mode
        || component == "dispatch" || component == "drain";
}

struct event {
    clock_type::time_point queued_at;
    const char* component;
    uint64_t sequence;
};

struct workload {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<event> queue;
    bool finished = false;
    std::atomic<uint64_t> consumed{0};
    performance_metrics metrics;
    clock_type::time_point started = clock_type::now();
};

void observe_depth(workload& state, const options& config) {
    for (const char* component : {"market_data", "order", "trade", "dispatch"}) {
        if (required(config, component)) {
            state.metrics.record_queue_depth(component, state.queue.size());
        }
    }
}

void consume(workload& state, const options& config, const std::atomic<bool>& stop,
             clock_type::time_point drain_deadline) {
    while (!stop.load() && clock_type::now() < drain_deadline) {
        event item;
        {
            std::unique_lock lock(state.mutex);
            state.ready.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return !state.queue.empty() || state.finished || stop.load();
            });
            if (state.queue.empty()) {
                if (state.finished) return;
                continue;
            }
            item = state.queue.front();
            state.queue.pop_front();
            observe_depth(state, config);
        }
        const auto started = clock_type::now();
        if (config.delay_us) std::this_thread::sleep_for(std::chrono::microseconds(config.delay_us));
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            clock_type::now() - item.queued_at).count();
        state.metrics.record_latency(item.component, elapsed);
        state.metrics.record_dispatch_latency_us(elapsed);
        if (config.error_every && item.sequence % config.error_every == 0) {
            state.metrics.record_error(item.component);
            state.metrics.record_error("dispatch");
        } else {
            state.metrics.record_success(item.component);
            state.metrics.record_success("dispatch");
        }
        state.metrics.record_drain_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
            clock_type::now() - started).count());
        state.metrics.record_success("drain");
        ++state.consumed;
    }
}

// Keep unwinding safe on standard libraries without C++20 jthread support.
class consumer_guard {
public:
    consumer_guard(workload& state, const options& config, clock_type::time_point deadline)
        : _state(state), _thread([this, &config, deadline] {
            try {
                consume(_state, config, _stop, deadline);
            } catch (...) {
                _failure = std::current_exception();
                _failed.store(true);
            }
        }) {}
    ~consumer_guard() {
        _stop.store(true);
        _state.ready.notify_one();
        join();
    }
    void join() { if (_thread.joinable()) _thread.join(); }
    bool failed() const { return _failed.load(); }
    void rethrow_failure() const { if (_failure) std::rethrow_exception(_failure); }

private:
    workload& _state;
    std::atomic<bool> _stop{false};
    std::atomic<bool> _failed{false};
    std::exception_ptr _failure;
    std::thread _thread;
};

void write_resources(std::ostream& output) {
#ifdef __linux__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
    size_t descriptors = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        ++descriptors;
    }
    output << ",\"peak_rss_kb\":" << usage.ru_maxrss << ",\"fd_count\":" << descriptors;
#else
    output << ",\"peak_rss_kb\":null,\"fd_count\":null";
#endif
}

bool report(workload& state, const options& config, uint64_t attempted, bool final, bool prior_failed) {
    health_check health("offline-synthetic", state.metrics);
    health.set_gate_thresholds(config.thresholds);
    const auto reports = health.full_report();
    size_t pending;
    {
        std::lock_guard lock(state.mutex);
        pending = state.queue.size();
    }
    bool failed = prior_failed || (final && pending != 0);
    for (const auto& item : reports) {
        if (required(config, item.metrics.name)
            && (!item.within_threshold || item.metrics.peak_queue_depth > config.thresholds.max_queue_depth)) {
            failed = true;
        }
    }
    std::cout << "{\"schema\":1,\"synthetic\":true,\"final\":" << (final ? "true" : "false")
        << ",\"mode\":\"" << config.mode << "\",\"attempted\":" << attempted
        << ",\"consumed\":" << state.consumed.load()
        << ",\"pending\":" << pending
        << ",\"elapsed_seconds\":" << std::chrono::duration<double>(clock_type::now() - state.started).count()
        << ",\"previous_gate_failure\":" << (prior_failed ? "true" : "false")
        << ",\"status\":\"" << (failed ? "failed" : "passed") << "\",\"components\":[";
    bool first = true;
    for (const auto& item : reports) {
        if (!first) std::cout << ',';
        first = false;
        const auto& m = item.metrics;
        std::cout << "{\"name\":\"" << m.name << "\",\"required\":"
            << (required(config, m.name) ? "true" : "false") << ",\"status\":\"" << item.status
            << "\",\"detail\":\"" << item.violation_detail
            << "\",\"events\":" << m.total_events << ",\"errors\":" << m.error_count
            << ",\"error_rate\":" << m.error_rate << ",\"dropped\":" << m.dropped_events
            << ",\"dropped_last_minute\":" << m.dropped_last_minute
            << ",\"queue_depth\":" << m.queue_depth << ",\"peak_queue_depth\":" << m.peak_queue_depth
            << ",\"peak_queue_exceeded\":"
            << (m.peak_queue_depth > config.thresholds.max_queue_depth ? "true" : "false")
            << ",\"avg_us\":" << m.avg_latency_us << ",\"p50_upper_us\":" << m.p50_latency_us
            << ",\"p95_upper_us\":" << m.p95_latency_us << ",\"p99_upper_us\":" << m.p99_latency_us
            << ",\"max_us\":" << m.max_latency_us << '}';
    }
    std::cout << ']';
    write_resources(std::cout);
    std::cout << "}\n" << std::flush;
    if (!std::cout) throw std::runtime_error("cannot write probe report");
    return failed;
}

int run(const options& config) {
    workload state;
    const auto start = clock_type::now();
    const auto deadline = start + std::chrono::seconds(config.seconds);
    const auto drain_deadline = deadline + std::chrono::seconds(config.drain_seconds);
    consumer_guard consumer(state, config, drain_deadline);
    auto next_report = start + std::chrono::seconds(1);
    const auto interval = std::chrono::nanoseconds(1000000000 / config.rate);
    auto next_event = start;
    uint64_t attempted = 0;
    bool failed = false;
    while (clock_type::now() < deadline && !consumer.failed()) {
        static constexpr const char* components[]{"market_data", "order", "trade"};
        const auto* component = config.mode == "mixed" ? components[attempted % 3] : config.mode.c_str();
        const event item{clock_type::now(), component, ++attempted};
        {
            std::lock_guard lock(state.mutex);
            if (state.queue.size() == config.capacity) state.metrics.record_dropped(component);
            else state.queue.push_back(item);
            observe_depth(state, config);
        }
        state.ready.notify_one();
        if (clock_type::now() >= next_report) {
            failed = report(state, config, attempted, false, failed);
            next_report = clock_type::now() + std::chrono::seconds(1);
        }
        next_event += interval;
        // Do not generate an artificial catch-up burst after scheduler stalls.
        next_event = std::max(next_event, clock_type::now());
        std::this_thread::sleep_until(next_event);
    }
    {
        std::lock_guard lock(state.mutex);
        state.finished = true;
    }
    state.ready.notify_one();
    consumer.join();
    consumer.rethrow_failure();
    uint64_t dropped = 0;
    for (const char* component : {"market_data", "order", "trade"}) {
        dropped += state.metrics.snapshot(component).dropped_events;
    }
    if (attempted != state.consumed.load() + dropped + state.queue.size()) {
        throw std::runtime_error("synthetic event accounting mismatch");
    }
    return report(state, config, attempted, true, failed) ? 1 : 0;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Offline synthetic queue probe; NOT a CTP/Seastar throughput or live acceptance test.\n"
            << "--seconds N --rate N --capacity N --mode market_data|order|mixed\n"
            << "--delay-us N --error-every N --max-avg-us N --max-p99-us N --max-drops N --max-queue N\n"
            << "--drain-seconds N (default 5; undrained backlog fails the run)\n"
            << "Exit: 0 synthetic gates passed, 1 gate failure, 2 invalid input/runtime error.\n";
        return 0;
    }
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Phase 6 probe: " << error.what() << '\n';
        return 2;
    }
}
