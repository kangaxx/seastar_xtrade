// Isolated CTP MdApi owner: callbacks only copy to a bounded queue; owner writes files.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include "market_tick_writer.hh"

#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/file.h>

namespace seastar::xtrader {
namespace {

namespace detail = capture_detail;
using steady_clock = std::chrono::steady_clock;

enum class event_kind { connected, disconnected, login, subscribed, tick, error };
enum class feed_state { idle, connecting, logging_in, subscribing, ready, stopped };

struct capture_event {
    event_kind kind{};
    int error_code = 0;
    int request_id = 0;
    bool is_last = false;
    bool has_payload = false;
    char instrument[81]{};
    uint64_t sequence = 0;
    int64_t received_unix_ms = 0;
    CThostFtdcDepthMarketDataField tick{};
};

void private_directory(const std::filesystem::path& directory) {
    if (!directory.is_absolute()) {
        throw std::invalid_argument("market state directory must be absolute");
    }
    std::filesystem::path prefix;
    for (const auto& part : directory) {
        if (part == "." || part == "..") {
            throw std::invalid_argument("market state directory cannot contain dot components");
        }
        prefix /= part;
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(prefix, error))) {
            throw std::invalid_argument("market state directory cannot contain symbolic links");
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("inspect market state directory", prefix, error);
        }
    }
    if (std::filesystem::create_directories(directory)) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    }
    struct stat status{};
    if (::stat(directory.c_str(), &status) != 0) { detail::io_error("inspect market state directory"); }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 077) != 0) {
        throw std::runtime_error("market state directory must be owner-only and owned by this user");
    }
}

std::string display_number(double number) {
    const auto text = detail::quote_number(number);
    return text.empty() ? "NA" : text;
}

} // namespace

class market_capture_session::implementation final : public CThostFtdcMdSpi {
public:
    implementation(market_capture_config config, market_data_paths paths,
                   std::filesystem::path state_directory, api_factory factory)
        : _config(std::move(config)), _paths(std::move(paths)),
          _state_directory(std::move(state_directory)), _factory(std::move(factory)),
          _writer(_paths, _config.contracts) {
        validate_market_capture_config(_config);
        if (!_factory) { throw std::invalid_argument("market API factory is required"); }
    }

    ~implementation() { release_api(); }

    void start() {
        if (_state != feed_state::idle) {
            throw std::logic_error("a market capture session can only be started once");
        }
        prepare_state();
        _state = feed_state::connecting;
        _deadline = steady_clock::now() + std::chrono::seconds(30);
        _last_sync = steady_clock::now();
        const auto flow = (_state_directory / "flow").string() + "/";
        _api = _factory(flow.c_str());
        if (!_api) { throw std::runtime_error("CTP MdApi creation failed"); }
        _api->RegisterSpi(this);
        _api->RegisterFront(_config.market_front.data());
        _api->Init();
    }

    void poll_for(std::chrono::milliseconds timeout) {
        if (_failed || _state == feed_state::idle || _state == feed_state::stopped) {
            throw std::logic_error("market capture is not active");
        }
        try {
            capture_event event;
            if (take(event, timeout)) { consume(event); }
            require_callbacks();
            const auto now = steady_clock::now();
            if (_state != feed_state::ready && now >= _deadline) {
                throw std::runtime_error("CTP connection/login/subscription timed out after 30 seconds; recording stopped");
            }
            if (now - _last_sync >= std::chrono::seconds(1)) {
                _writer.sync();
                _last_sync = now;
            }
        } catch (...) {
            _failed = true;
            throw;
        }
    }

    void stop() {
        release_api();
        try {
            require_callbacks();
            capture_event event;
            uint64_t discarded_ticks = 0;
            while (take(event, std::chrono::milliseconds(0))) {
                if (_failed) {
                    if (event.kind == event_kind::tick) { ++discarded_ticks; }
                    continue;
                }
                if (event.kind == event_kind::tick) { record(event); }
                if (event.kind == event_kind::error || event.kind == event_kind::disconnected) {
                    throw std::runtime_error("CTP reported an error/disconnection while stopping");
                }
            }
            _writer.close();
            _lock = detail::file_descriptor{};
            _state = feed_state::stopped;
            if (discarded_ticks) {
                throw std::runtime_error("failed capture: unprocessed Tick callbacks not recorded="
                    + std::to_string(discarded_ticks) + "; no continuity guarantee");
            }
        } catch (...) {
            _writer.close();
            _lock = detail::file_descriptor{};
            _state = feed_state::stopped;
            throw;
        }
    }

    bool is_ready() const noexcept { return !_failed && _state == feed_state::ready; }
    uint64_t ticks_written() const noexcept { return _written; }

    void print_status(std::ostream& output) const {
        output << "[MARKET] " << (is_ready() ? "subscribed" : "connecting/login/subscribing")
               << "; subscriptions=" << _subscribed.size() << '/' << _config.contracts.size()
               << "; ticks_written=" << _written << '\n';
        for (const auto& contract : _config.contracts) {
            output << contract.exchange << ':' << contract.instrument << ' ';
            const auto quote = _quotes.find(contract.instrument);
            output << (quote == _quotes.end() ? "(waiting for first tick)" : quote->second) << '\n';
        }
        output << std::flush;
    }

    void OnFrontConnected() override {
        capture_event event;
        event.kind = event_kind::connected;
        post(event);
    }

    void OnFrontDisconnected(int reason) override {
        capture_event event;
        event.kind = event_kind::disconnected;
        event.error_code = reason;
        post(event);
    }

    void OnRspUserLogin(CThostFtdcRspUserLoginField* response, CThostFtdcRspInfoField* error,
                        int request_id, bool is_last) override {
        capture_event event;
        event.kind = event_kind::login;
        event.error_code = error ? error->ErrorID : 0;
        event.has_payload = response != nullptr;
        event.request_id = request_id;
        event.is_last = is_last;
        post(event);
    }

    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* instrument,
                            CThostFtdcRspInfoField* error, int, bool) override {
        capture_event event;
        event.kind = event_kind::subscribed;
        event.error_code = error ? error->ErrorID : 0;
        event.has_payload = instrument != nullptr;
        if (instrument) {
            static_assert(sizeof(instrument->InstrumentID) == sizeof(event.instrument));
            std::memcpy(event.instrument, instrument->InstrumentID, sizeof(event.instrument));
        }
        post(event);
    }

    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override {
        capture_event event;
        event.kind = event_kind::tick;
        event.has_payload = tick != nullptr;
        event.received_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (tick) { event.tick = *tick; }
        post(event);
    }

    void OnRspError(CThostFtdcRspInfoField* error, int, bool) override {
        capture_event event;
        event.kind = event_kind::error;
        event.error_code = error ? error->ErrorID : -1;
        post(event);
    }

private:
    void prepare_state() {
        if (::access(_paths.root().c_str(), R_OK | X_OK) != 0) {
            detail::io_error("data root must exist and be accessible before connecting CTP");
        }
        const auto source = _paths.source_root();
        const auto writable = std::filesystem::exists(source) ? source : _paths.root();
        if (::access(writable.c_str(), W_OK | X_OK) != 0) {
            detail::io_error("grant this user write access to /data/x_trader_data before connecting CTP");
        }
        for (const auto& contract : _config.contracts) {
            _paths.validate_filesystem_path(_paths.data_file(
                {"x_trader_data", "futures", contract.exchange, contract.instrument, "tick"}, "20000101"));
        }
        private_directory(_state_directory);
        private_directory(_state_directory / "flow");
        _lock = detail::file_descriptor(::open((_state_directory / "collector.lock").c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
        if (_lock.get() < 0) { detail::io_error("open collector lock"); }
        struct stat status{};
        if (::fstat(_lock.get(), &status) != 0) { detail::io_error("inspect collector lock"); }
        if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid()
                || status.st_nlink != 1 || (status.st_mode & 077) != 0) {
            throw std::runtime_error("invalid collector lock file");
        }
        if (::flock(_lock.get(), LOCK_EX | LOCK_NB) != 0) {
            detail::io_error("collector state is already in use");
        }
    }

    void release_api() noexcept {
        if (_api) {
            _api->RegisterSpi(nullptr);
            _api->Release();
            _api = nullptr;
        }
    }

    void post(capture_event& event) noexcept {
        try {
            std::lock_guard lock(_mutex);
            if (_count == _events.size() || _sequence == std::numeric_limits<uint64_t>::max()) {
                _callback_failed.store(true);
            } else {
                if (event.kind == event_kind::tick) { event.sequence = ++_sequence; }
                _events[(_head + _count) % _events.size()] = event;
                ++_count;
            }
        } catch (...) {
            // SDK callback exceptions cannot escape its thread; owner reports the fatal loss.
            _callback_failed.store(true);
        }
        _available.notify_one();
    }

    bool take(capture_event& event, std::chrono::milliseconds timeout) {
        std::unique_lock lock(_mutex);
        _available.wait_for(lock, timeout, [this] { return _count || _callback_failed.load(); });
        require_callbacks();
        if (!_count) { return false; }
        event = _events[_head];
        _head = (_head + 1) % _events.size();
        --_count;
        return true;
    }

    void require_callbacks() const {
        if (_callback_failed.load()) {
            throw std::runtime_error("CTP callback queue overflow/failure: tick loss is possible; recording stopped");
        }
    }

    void login() {
        if (_state != feed_state::connecting) {
            throw std::runtime_error("unexpected CTP reconnection; restart the collector explicitly");
        }
        CThostFtdcReqUserLoginField login{};
        std::memcpy(login.BrokerID, _config.broker_id.data(), _config.broker_id.size());
        std::memcpy(login.UserID, _config.user_id.data(), _config.user_id.size());
        std::memcpy(login.Password, _config.password.data(), _config.password.size());
        _state = feed_state::logging_in;
        const auto result = _api->ReqUserLogin(&login, 1);
        // The SDK consumes the request synchronously; do not retain a second password copy.
        OPENSSL_cleanse(&login, sizeof(login));
        if (result != 0) {
            throw std::runtime_error("CTP MD login request rejected, code=" + std::to_string(result));
        }
    }

    void subscribe(const capture_event& event) {
        if (_state != feed_state::logging_in || event.request_id != 1
                || event.error_code != 0 || !event.has_payload) {
            throw std::runtime_error("CTP MD login response failed, code=" + std::to_string(event.error_code));
        }
        if (!event.is_last) { return; }
        std::vector<char*> instruments;
        for (auto& contract : _config.contracts) { instruments.push_back(contract.instrument.data()); }
        _state = feed_state::subscribing;
        const auto result = _api->SubscribeMarketData(instruments.data(), static_cast<int>(instruments.size()));
        if (result != 0) {
            throw std::runtime_error("CTP subscription request rejected, code=" + std::to_string(result));
        }
    }

    void acknowledge(const capture_event& event) {
        if ((_state != feed_state::subscribing && _state != feed_state::ready)
                || event.error_code != 0 || !event.has_payload) {
            throw std::runtime_error("CTP subscription response failed, code=" + std::to_string(event.error_code));
        }
        const auto instrument = detail::sdk_text(event.instrument);
        const auto found = std::find_if(_config.contracts.begin(), _config.contracts.end(),
            [&instrument] (const auto& contract) { return contract.instrument == instrument; });
        if (found == _config.contracts.end() || !_subscribed.insert(instrument).second) {
            throw std::runtime_error("unexpected or duplicate CTP subscription acknowledgement");
        }
        if (_subscribed.size() == _config.contracts.size()) { _state = feed_state::ready; }
    }

    void record(const capture_event& event) {
        if (!event.has_payload || (_state != feed_state::subscribing && _state != feed_state::ready)) {
            throw std::runtime_error("CTP tick arrived without an active subscription request");
        }
        _writer.write(event.tick, event.sequence, event.received_unix_ms);
        ++_written;
        const auto& tick = event.tick;
        _quotes[detail::sdk_text(tick.InstrumentID)] =
            detail::sdk_text(tick.UpdateTime) + " last=" + display_number(tick.LastPrice)
            + " bid1=" + display_number(tick.BidPrice1) + " ask1=" + display_number(tick.AskPrice1)
            + " volume=" + std::to_string(tick.Volume) + " open_interest=" + display_number(tick.OpenInterest);
    }

    void consume(const capture_event& event) {
        switch (event.kind) {
        case event_kind::connected: login(); break;
        case event_kind::login: subscribe(event); break;
        case event_kind::subscribed: acknowledge(event); break;
        case event_kind::tick: record(event); break;
        case event_kind::disconnected:
            throw std::runtime_error("CTP MD disconnected, code=" + std::to_string(event.error_code)
                + "; recording stopped, restart explicitly after checking the feed");
        case event_kind::error:
            throw std::runtime_error("CTP MD error, code=" + std::to_string(event.error_code));
        }
    }

    market_capture_config _config;
    market_data_paths _paths;
    std::filesystem::path _state_directory;
    api_factory _factory;
    detail::market_tick_writer _writer;
    detail::file_descriptor _lock;
    CThostFtdcMdApi* _api = nullptr;
    feed_state _state = feed_state::idle;
    bool _failed = false;
    steady_clock::time_point _deadline;
    steady_clock::time_point _last_sync;
    std::set<std::string> _subscribed;
    std::map<std::string, std::string> _quotes;
    uint64_t _written = 0;
    std::mutex _mutex;
    std::condition_variable _available;
    std::array<capture_event, 1024> _events;
    std::size_t _head = 0;
    std::size_t _count = 0;
    uint64_t _sequence = 0;
    std::atomic<bool> _callback_failed{false};
};

market_capture_session::market_capture_session(market_capture_config config, market_data_paths paths,
        std::filesystem::path state_directory, api_factory factory)
    : _impl(std::make_unique<implementation>(std::move(config), std::move(paths),
          std::move(state_directory), std::move(factory))) {}

market_capture_session::~market_capture_session() = default;
void market_capture_session::start() { _impl->start(); }
void market_capture_session::poll_for(std::chrono::milliseconds timeout) { _impl->poll_for(timeout); }
void market_capture_session::stop() { _impl->stop(); }
bool market_capture_session::is_ready() const noexcept { return _impl->is_ready(); }
uint64_t market_capture_session::ticks_written() const noexcept { return _impl->ticks_written(); }
void market_capture_session::print_status(std::ostream& output) const { _impl->print_status(output); }

} // namespace seastar::xtrader
