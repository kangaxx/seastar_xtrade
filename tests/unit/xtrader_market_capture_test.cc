// Offline menu -> authenticated config -> fake MdApi -> real Tick file chain.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include "market_capture_menu.hh"
#include "market_tick_writer.hh"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;
using namespace seastar::xtrader;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template<typename Function>
void require_failure(Function&& function, const char* message) {
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

template<std::size_t N>
void field(char (&buffer)[N], const std::string& value) {
    require(value.size() < N, "test field fits SDK buffer");
    std::memset(buffer, 0, N);
    std::memcpy(buffer, value.data(), value.size());
}

market_capture_config config() {
    market_capture_config result;
    result.broker_id = "offline";
    result.user_id = "offline-user";
    result.password = "test-only";
    result.market_front = "tcp://127.0.0.1:1";
    result.contracts = {{"SHFE", "rb2701"}, {"SHFE", "ag2612"}};
    return result;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "open test output");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void replace(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(static_cast<bool>(output), "write synthetic fixture");
    require(::chmod(path.c_str(), 0600) == 0, "protect synthetic fixture");
}

CThostFtdcDepthMarketDataField tick(const std::string& instrument = "rb2701",
                                   const std::string& day = "20261113") {
    CThostFtdcDepthMarketDataField result{};
    field(result.InstrumentID, instrument);
    field(result.ExchangeID, "SHFE");
    field(result.ActionDay, day);
    field(result.TradingDay, "20261116");
    field(result.UpdateTime, "21:01:02");
    result.UpdateMillisec = 123;
    result.LastPrice = 3500.25;
    result.BidPrice1 = 3500;
    result.AskPrice1 = 3501;
    result.BidVolume1 = 3;
    result.AskVolume1 = 4;
    result.Volume = 100;
    result.Turnover = 350000;
    result.OpenInterest = 900;
    result.OpenPrice = 3499;
    result.HighestPrice = 3501;
    result.LowestPrice = 3498;
    result.BidPrice2 = std::numeric_limits<double>::max();
    return result;
}

class fake_md_api final : public CThostFtdcMdApi {
public:
    CThostFtdcMdSpi* spi = nullptr;
    int releases = 0;
    int login_error = 0;
    int subscribe_error = 0;
    bool has_expected_login = false;
    std::vector<std::string> subscriptions;

    void Release() override { ++releases; }
    void Init() override { spi->OnFrontConnected(); }
    int Join() override { return 0; }
    const char* GetTradingDay() override { return "20261116"; }
    void RegisterFront(char*) override {}
    void RegisterNameServer(char*) override {}
    void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
    void RegisterSpi(CThostFtdcMdSpi* handler) override { spi = handler; }
    int ReqUserLogin(CThostFtdcReqUserLoginField* request, int id) override {
        has_expected_login = std::string(request->BrokerID) == "offline"
            && std::string(request->UserID) == "offline-user" && std::string(request->Password) == "test-only";
        CThostFtdcRspUserLoginField response{};
        CThostFtdcRspInfoField error{};
        error.ErrorID = login_error;
        spi->OnRspUserLogin(&response, &error, id, true);
        return 0;
    }
    int SubscribeMarketData(char* instruments[], int count) override {
        for (int index = 0; index < count; ++index) {
            subscriptions.emplace_back(instruments[index]);
            CThostFtdcSpecificInstrumentField instrument{};
            field(instrument.InstrumentID, subscriptions.back());
            CThostFtdcRspInfoField error{};
            error.ErrorID = subscribe_error;
            spi->OnRspSubMarketData(&instrument, &error, 0, index + 1 == count);
        }
        return 0;
    }
    int UnSubscribeMarketData(char*[], int) override { return 0; }
    int SubscribeForQuoteRsp(char*[], int) override { return 0; }
    int UnSubscribeForQuoteRsp(char*[], int) override { return 0; }
    int ReqUserLogout(CThostFtdcUserLogoutField*, int) override { return 0; }
    int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*, int) override { return 0; }
    void send(CThostFtdcDepthMarketDataField value) { spi->OnRtnDepthMarketData(&value); }
};

struct fixture {
    fs::path root;
    market_data_paths paths;
    explicit fixture(fs::path directory) : root(std::move(directory)), paths(root / "data") {
        fs::create_directories(paths.root());
    }
    market_capture_session::api_factory factory(fake_md_api& api) {
        return [&api] (const char*) { return &api; };
    }
    fs::path output(const std::string& instrument, const std::string& date) const {
        return paths.data_file({"x_trader_data", "futures", "SHFE", instrument, "tick"}, date);
    }
};

void ready(market_capture_session& session) {
    session.start();
    for (int index = 0; index < 8 && !session.is_ready(); ++index) {
        session.poll_for(std::chrono::milliseconds(0));
    }
    require(session.is_ready(), "all configured subscriptions acknowledged");
}

void test_authenticated_config(const fs::path& root) {
    fs::create_directories(root);
    const auto key = root / "key";
    const auto encrypted = root / "config.enc";
    create_market_capture_key(key);
    save_market_capture_config(config(), encrypted, key);
    require(read(encrypted).find("test-only") == std::string::npos, "password is not stored in plaintext");
    const auto loaded = load_market_capture_config(encrypted, key);
    require(loaded.user_id == "offline-user" && loaded.password == "test-only"
        && loaded.contracts.size() == 2, "authenticated configuration round trip");
    require_failure([&] { create_market_capture_key(key); }, "key must not be overwritten");
    create_market_capture_key(root / "other.key");
    require_failure([&] { (void)load_market_capture_config(encrypted, root / "other.key"); }, "wrong key rejected");
    auto tampered = read(encrypted);
    tampered.back() ^= 1;
    replace(root / "tampered.enc", tampered);
    require_failure([&] { (void)load_market_capture_config(root / "tampered.enc", key); }, "tampering rejected");
    replace(root / "plain.enc", "password=test-only\n");
    require_failure([&] { (void)load_market_capture_config(root / "plain.enc", key); }, "plaintext rejected");
    fs::create_symlink(key, root / "linked.key");
    require_failure([&] { (void)load_market_capture_config(encrypted, root / "linked.key"); }, "linked key rejected");
    require(::chmod(encrypted.c_str(), 0644) == 0, "make synthetic config readable for negative case");
    require_failure([&] { (void)load_market_capture_config(encrypted, key); }, "unsafe file permissions rejected");
    auto invalid = config();
    invalid.contracts.push_back(invalid.contracts.front());
    require_failure([&] { validate_market_capture_config(invalid); }, "duplicate contract rejected");
    require_failure([&] { (void)parse_market_capture_contracts("SHFE:rb2701,"); }, "empty contract rejected");
}

void test_menu_to_tick_chain(const fs::path& root) {
    fixture files(root);
    const auto key = root / "key";
    const auto encrypted = root / "config.enc";
    create_market_capture_key(key);
    save_market_capture_config(config(), encrypted, key);
    std::istringstream input("3\n\n3\nSTART\nu\n");
    std::ostringstream output;
    std::ostringstream errors;
    unsigned launches = 0;
    cli::run_market_capture_menu(encrypted, key, input, output, errors,
        [&] (const fs::path& selected_config, const fs::path& selected_key) {
            ++launches;
            fake_md_api api;
            market_capture_session session(load_market_capture_config(selected_config, selected_key),
                files.paths, root / "state", files.factory(api));
            ready(session);
            require(api.has_expected_login && api.subscriptions == std::vector<std::string>{"rb2701", "ag2612"},
                "decrypted credentials and concrete subscriptions reach only fake MdApi");
            api.send(tick());
            session.poll_for(std::chrono::milliseconds(0));
            api.send(tick("ag2612"));
            session.poll_for(std::chrono::milliseconds(0));
            // Stopping drains a callback already accepted but not yet processed.
            auto next_day = tick("rb2701", "20261114");
            field(next_day.ExchangeID, "");
            api.send(next_day);
            session.stop();
            require(session.ticks_written() == 3 && api.releases == 1, "all first/pending ticks persisted and API released");
            return 0;
        });
    require(launches == 1 && errors.str().empty(), "cancel does not connect; explicit confirmation launches once");
    for (const auto& file : {files.output("rb2701", "20261113"), files.output("ag2612", "20261113"),
                             files.output("rb2701", "20261114")}) {
        const auto text = read(file);
        require(std::count(text.begin(), text.end(), '\n') == 2, "one schema header and one complete raw Tick");
        const auto newline = text.find('\n');
        require(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(newline), ',')
            == std::count(text.begin() + static_cast<std::ptrdiff_t>(newline + 1), text.end(), ','),
            "CSV header and row have matching columns");
        require(text.find("20261116,21:01:02,123,3500.25,100,") != std::string::npos,
            "trading day/time, actual price and cumulative volume preserved");
        require(text.find("1.797693") == std::string::npos, "CTP missing price sentinel is not recorded as a price");
    }
    require(output.str().find("test-only") == std::string::npos
        && output.str().find("offline-user") == std::string::npos, "menu does not expose credentials");
}

void test_failures(const fs::path& root) {
    fixture files(root);
    {
        fake_md_api api;
        api.login_error = 7;
        market_capture_session session(config(), files.paths, root / "login_state", files.factory(api));
        session.start();
        session.poll_for(std::chrono::milliseconds(0));
        require_failure([&] { session.poll_for(std::chrono::milliseconds(0)); }, "failed login cannot subscribe");
        require(api.subscriptions.empty(), "no subscription after login failure");
        session.stop();
    }
    {
        fake_md_api api;
        api.subscribe_error = 3;
        market_capture_session session(config(), files.paths, root / "sub_state", files.factory(api));
        require_failure([&] { ready(session); }, "failed subscription is visible");
        require(!session.is_ready(), "failed subscription never reports ready");
        session.stop();
    }
    {
        fake_md_api api;
        market_capture_session session(config(), files.paths, root / "disconnect_state", files.factory(api));
        ready(session);
        api.spi->OnFrontDisconnected(0x1001);
        api.send(tick());
        require_failure([&] { session.poll_for(std::chrono::milliseconds(0)); }, "disconnect stops recording explicitly");
        require(!session.is_ready(), "disconnect invalidates readiness");
        require_failure([&] { session.stop(); }, "post-disconnect pending Tick is reported, not recorded");
    }
    {
        fake_md_api api;
        market_capture_session session(config(), files.paths, root / "overflow_state", files.factory(api));
        ready(session);
        for (int index = 0; index < 1025; ++index) { api.send(tick()); }
        require_failure([&] { session.poll_for(std::chrono::milliseconds(0)); }, "callback overflow is fatal, not silent loss");
        require_failure([&] { session.stop(); }, "stop must also report a callback gap");
    }
    require(!fs::exists(files.output("rb2701", "20261113")), "failed sessions never fabricate Tick data");
}

void test_output_boundaries(const fs::path& root) {
    fixture files(root);
    fake_md_api api;
    market_capture_session session(config(), files.paths, root / "state", files.factory(api));
    ready(session);
    {
        fake_md_api second;
        market_capture_session duplicate(config(), files.paths, root / "state", files.factory(second));
        require_failure([&] { duplicate.start(); }, "same state cannot run a second collector");
        require(second.spi == nullptr, "state lock checked before SDK initialization");
    }
    api.send(tick("unconfigured"));
    require_failure([&] { session.poll_for(std::chrono::milliseconds(0)); }, "unconfigured Tick is rejected");
    require(!session.is_ready(), "a fatal payload error invalidates readiness");
    session.stop();
    {
        fake_md_api invalid_api;
        market_capture_session invalid(config(), files.paths, root / "date_state", files.factory(invalid_api));
        ready(invalid);
        invalid_api.send(tick("rb2701", "20260230"));
        require_failure([&] { invalid.poll_for(std::chrono::milliseconds(0)); }, "invalid ActionDay is rejected");
        invalid.stop();
    }
    const auto file = files.output("rb2701", "20261113");
    fs::create_directories(file.parent_path());
    replace(root / "outside", "outside\n");
    fs::create_symlink(root / "outside", file);
    fake_md_api linked_api;
    market_capture_session linked(config(), files.paths, root / "link_state", files.factory(linked_api));
    ready(linked);
    linked_api.send(tick());
    require_failure([&] { linked.poll_for(std::chrono::milliseconds(0)); }, "output symlink rejected");
    require(read(root / "outside") == "outside\n", "symlink target remains unchanged");
    linked.stop();
}

void test_append_and_file_lock(const fs::path& root) {
    fixture files(root);
    {
        fake_md_api api;
        market_capture_session session(config(), files.paths, root / "state1", files.factory(api));
        ready(session);
        api.send(tick());
        session.poll_for(std::chrono::milliseconds(0));
        fake_md_api other;
        market_capture_session concurrent(config(), files.paths, root / "state2", files.factory(other));
        ready(concurrent);
        other.send(tick());
        require_failure([&] { concurrent.poll_for(std::chrono::milliseconds(0)); }, "same daily Tick file is single-writer");
        concurrent.stop();
        session.stop();
    }
    {
        fake_md_api api;
        market_capture_session restarted(config(), files.paths, root / "state3", files.factory(api));
        ready(restarted);
        api.send(tick());
        restarted.poll_for(std::chrono::milliseconds(0));
        restarted.stop();
    }
    const auto file = files.output("rb2701", "20261113");
    const auto text = read(file);
    require(std::count(text.begin(), text.end(), '\n') == 3, "restart appends without duplicate header");
    replace(file, text + "incomplete");
    fake_md_api api;
    market_capture_session damaged(config(), files.paths, root / "state4", files.factory(api));
    ready(damaged);
    api.send(tick());
    require_failure([&] { damaged.poll_for(std::chrono::milliseconds(0)); }, "partial last row is not silently repaired");
    damaged.stop();
}

} // namespace

int main() {
    const auto parent = fs::temp_directory_path();
    auto pattern = (parent / "xtrader_market_capture_XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    if (!created) { std::cerr << "cannot create offline test fixture\n"; return 1; }
    const fs::path root(created);
    try {
        test_authenticated_config(root / "crypto");
        test_menu_to_tick_chain(root / "chain");
        test_failures(root / "failures");
        test_output_boundaries(root / "boundaries");
        test_append_and_file_lock(root / "append");
        fs::remove_all(root);
        std::cout << "market capture offline encrypted-config/menu/SDK/file cases passed; no real connection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
