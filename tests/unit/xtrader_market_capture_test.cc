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

void test_configuration_initialization(const fs::path& root) {
    const auto key = root / "keys" / "market.key";
    const auto encrypted = root / "config" / "market.enc";
    initialize_market_capture_config(config(), encrypted, key);
    const auto original_key = read(key);
    const auto original_config = read(encrypted);
    require(load_market_capture_config(encrypted, key).password == "test-only",
        "initialization generates a usable key and authenticated config");
    require_failure([&] { initialize_market_capture_config(config(), encrypted, key); },
        "initialization never overwrites an existing encrypted configuration");
    require(read(key) == original_key && read(encrypted) == original_config,
        "failed repeat initialization preserves both existing files");
    const auto second = root / "config" / "second.enc";
    initialize_market_capture_config(config(), second, key);
    require(read(key) == original_key && load_market_capture_config(second, key).user_id == "offline-user",
        "new config can securely reuse an existing key without rotation");
    const auto unused_key = root / "keys" / "unused.key";
    require_failure([&] { initialize_market_capture_config(config(), root / "wrong.ini", unused_key); },
        "initialization requires an enc filename");
    require(!fs::exists(unused_key), "invalid config path does not generate an orphan key");
    require_failure([&] { initialize_market_capture_config(config(), root / "same.enc", root / "same.enc"); },
        "key and encrypted config cannot share a path");
    replace(root / "bad.key", "invalid");
    require_failure([&] { initialize_market_capture_config(config(), root / "bad.enc", root / "bad.key"); },
        "existing invalid key is rejected rather than replaced");
    require(read(root / "bad.key") == "invalid" && !fs::exists(root / "bad.enc"),
        "invalid key and absent configuration are preserved on failure");
}

std::string legacy_hex(const std::string& value, const std::string& key) {
    const char* digits = "0123456789ABCDEF";
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i] ^ key[i % key.size()] ^ 0x5a);
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

void test_legacy_import(const fs::path& root) {
    fs::create_directories(root);
    const std::string secret = "fixture-secret";
    const auto encoded = [&] (const char* text) { return legacy_hex(text, secret); };
    const auto ini = root / "legacy.ini";
    const auto text = std::string("\xef\xbb\xbf[market]\r\nmarket_front=tcp://127.0.0.1:1\r\n")
        + "broker_id=5355444246465A\r\nuser_id=" + encoded("offline-user")
        + "\r\npassword=" + encoded("test-only") + "\r\n[trader]\r\n"
        + "trade_front=tcp://127.0.0.1:2\r\nbroker_id=" + encoded("broker")
        + "\r\nuser_id=" + encoded("trader-user") + "\r\npassword=" + encoded("trade-pass")
        + "\r\napp_id=" + encoded("test-app") + "\r\nauth_code=" + encoded("test-auth")
        + "\r\nuser_product_Info=" + encoded("test-product") + "\r\n[control]\r\nloop_interval=1\r\n";
    replace(ini, text);
    const std::vector<market_capture_contract> contracts{{"SHFE", "rb2701"}};
    auto imported = import_legacy_market_config(ini, secret, contracts);
    require(imported.broker_id == "offline" && imported.user_id == "offline-user"
        && imported.password == "test-only" && imported.trader
        && imported.trader->trade_front == "tcp://127.0.0.1:2"
        && imported.trader->password == "trade-pass" && imported.trader->app_id == "test-app"
        && imported.trader->auth_code == "test-auth" && imported.trader->user_product_info == "test-product",
        "legacy XOR and case-insensitive fields are imported");
    const auto encrypted = root / "imported.enc";
    const auto key = root / "key";
    initialize_market_capture_config(imported, encrypted, key);
    const auto loaded = load_market_capture_config(encrypted, key);
    require(loaded.trader && loaded.trader->user_id == "trader-user"
        && loaded.trader->password == "trade-pass" && loaded.password == "test-only",
        "trader and market fields survive authenticated encryption");
    require(read(encrypted).find("trade-pass") == std::string::npos, "trader password is not plaintext on disk");
    fixture files(root / "collector");
    fake_md_api api;
    market_capture_session session(load_market_capture_config(encrypted, key),
        files.paths, root / "state", files.factory(api));
    ready(session);
    require(api.has_expected_login && api.subscriptions == std::vector<std::string>{"rb2701"},
        "imported extended configuration reaches only the read-only market API");
    session.stop();
    require_failure([&] { (void)import_legacy_market_config(ini, "", contracts); }, "empty legacy secret rejected");
    replace(root / "bad.ini", text + "\n[market]\npassword=0\n");
    require_failure([&] { (void)import_legacy_market_config(root / "bad.ini", secret, contracts); },
        "duplicate fields rejected");
    auto invalid = text;
    const auto pos = invalid.find("password=" + encoded("test-only"));
    invalid.replace(pos, std::string("password=" + encoded("test-only")).size(), "password=XYZ");
    replace(root / "invalid.ini", invalid);
    require_failure([&] { (void)import_legacy_market_config(root / "invalid.ini", secret, contracts); },
        "invalid legacy hex rejected");
    require(::chmod(ini.c_str(), 0644) == 0, "change legacy fixture permissions");
    require(import_legacy_market_config(ini, secret, contracts).password == "test-only",
        "world-readable legacy import source is accepted");
}

void test_plaintext_import(const fs::path& root) {
    fs::create_directories(root);
    const auto source = root / "plain.ini";
    const auto text = std::string("\xef\xbb\xbf[market]\r\nmarket_front=tcp://127.0.0.1:1\r\n")
        + "broker_id=offline\r\nuser_id=offline-user\r\npassword=market-pass\r\n"
          "[trader]\r\ntrade_front=tcp://127.0.0.1:2\r\nbroker_id=trader-broker\r\n"
          "user_id=trader-user\r\npassword=trade-pass\r\napp_id=test-app\r\n"
          "auth_code=test-auth\r\nuser_product_Info=test-product\r\n"
          "[control]\r\nbind_cpu_core=0\r\nloop_interval=1\r\n";
    replace(source, text);
    require(::chmod(source.c_str(), 0644) == 0, "make plaintext import source readable by other users");
    auto imported = import_plaintext_market_config(source);
    require(imported.broker_id == "offline" && imported.user_id == "offline-user"
        && imported.password == "market-pass" && imported.trader
        && imported.trader->password == "trade-pass" && imported.trader->app_id == "test-app"
        && imported.contracts.empty(), "plaintext INI fields imported without prompting for credentials or contracts");

    const auto encrypted = root / "market-plaintext-import.enc";
    const auto key = root / "key";
    initialize_market_capture_config(imported, encrypted, key);
    const auto loaded = load_market_capture_config(encrypted, key);
    require(loaded.contracts.empty() && loaded.trader && loaded.trader->auth_code == "test-auth"
        && loaded.password == "market-pass", "empty-contract plaintext import survives encryption");
    const auto envelope = read(encrypted);
    require(envelope.find("market-pass") == std::string::npos
        && envelope.find("trade-pass") == std::string::npos, "plaintext credentials are encrypted at rest");
    fake_md_api api;
    require_failure([&] {
        market_capture_session session(load_market_capture_config(encrypted, key),
            market_data_paths(root / "data"), root / "state",
            [&] (const char*) { return &api; });
    }, "a configuration without contracts cannot start a collector");
    require(api.spi == nullptr, "missing contracts are rejected before creating the CTP API");

    auto selected = root / "previous.enc";
    std::ostringstream output;
    std::ostringstream errors;
    std::istringstream input(source.string() + "\nSimNow account\n");
    cli::run_plaintext_import_menu(selected, key, input, output, errors,
        [&] (const fs::path& path) {
            require(path == source, "plaintext importer receives only the supplied source path");
            return encrypted;
        });
    require(selected == encrypted && errors.str().empty(), "successful plaintext import selects generated config");
    const auto profile = cli::load_market_profile(encrypted);
    require(profile && profile->alias == "SimNow account" && profile->key == key,
        "plaintext import saves the chosen account alias and key");
    std::istringstream cancelled("u\n");
    cli::run_plaintext_import_menu(selected, key, cancelled, output, errors,
        [] (const fs::path&) -> fs::path { throw std::runtime_error("must not run"); });
    require(selected == encrypted, "cancelled plaintext import preserves selected config");
}

void test_market_config_selection(const fs::path& root) {
    fs::create_directories(root);
    const auto key = root / "key";
    const auto first = root / "a.enc";
    const auto second = root / "b.enc";
    initialize_market_capture_config(config(), first, key);
    initialize_market_capture_config(config(), second, key);
    auto selected = first;
    auto selected_key = key;
    std::ostringstream output;
    std::ostringstream errors;
    std::istringstream input("2\n\n");
    cli::select_market_config(selected, selected_key, input, output, errors);
    require(selected == second && selected_key == key && errors.str().empty(),
        "startup selection changes the chosen encrypted profile");
    std::istringstream invalid("9\n");
    cli::select_market_config(selected, selected_key, invalid, output, errors);
    require(selected == second && errors.str().find("[ERROR]") != std::string::npos,
        "invalid selection keeps the existing profile");
    unsigned imports = 0;
    std::istringstream cancelled("\n\n\n");
    cli::run_legacy_import_menu(selected, selected_key, cancelled, output, errors,
        [&] (const fs::path&, const fs::path&) { ++imports; return 0; });
    require(imports == 0 && selected == second, "cancelled import does not change selection");
    const auto third = root / "c.enc";
    std::istringstream failed(third.string() + "\n\nIMPORT\n");
    cli::run_legacy_import_menu(selected, selected_key, failed, output, errors,
        [] (const fs::path&, const fs::path&) { return 2; });
    require(selected == second, "failed import keeps previously selected profile");
    std::istringstream successful(third.string() + "\n\nIMPORT\n");
    cli::run_legacy_import_menu(selected, selected_key, successful, output, errors,
        [&] (const fs::path& path, const fs::path& selected_key_file) {
            ++imports;
            initialize_market_capture_config(config(), path, selected_key_file);
            return 0;
        });
    require(imports == 1 && selected == third && selected_key == key,
        "successful import adopts both selected paths");
}

void test_initial_ctp_key_menu(const fs::path& root) {
    auto encrypted = root / "old.enc";
    auto key = root / "old.key";
    const auto selected_config = root / "config" / "selected.enc";
    const auto selected_key = root / "keys" / "selected.key";
    const auto commands = selected_config.string() + "\n" + selected_key.string() + "\n";
    std::ostringstream output;
    std::ostringstream errors;
    unsigned calls = 0;
    const auto initialize = [&] (const fs::path& config_file, const fs::path& key_file) {
        ++calls;
        initialize_market_capture_config(config(), config_file, key_file);
        return 0;
    };
    std::istringstream cancelled(commands + "\n");
    cli::run_initial_ctp_key_menu(encrypted, key, cancelled, output, errors, initialize);
    require(calls == 0 && encrypted == root / "old.enc" && !fs::exists(selected_key),
        "cancel does not generate a key or change selected paths");
    std::istringstream failed(commands + "INITIALIZE\n");
    cli::run_initial_ctp_key_menu(encrypted, key, failed, output, errors,
        [] (const fs::path&, const fs::path&) { return 2; });
    require(encrypted == root / "old.enc" && key == root / "old.key"
        && errors.str().find("[ERROR]") != std::string::npos, "utility failure preserves current paths and is visible");
    std::istringstream confirmed(commands + "INITIALIZE\n");
    cli::run_initial_ctp_key_menu(encrypted, key, confirmed, output, errors, initialize);
    require(calls == 1 && encrypted == selected_config && key == selected_key,
        "successful initializer updates the capture paths for this CLI session");
    require(load_market_capture_config(encrypted, key).contracts.size() == 2,
        "menu initialization produces a decryptable configuration with configured contracts");
    require(output.str().find("test-only") == std::string::npos
        && output.str().find("offline-user") == std::string::npos, "initialization menu does not expose credentials");
}

void test_menu_to_tick_chain(const fs::path& root) {
    fixture files(root);
    auto key = root / "key";
    auto encrypted = root / "config.enc";
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

void test_account_profiles(const fs::path& root) {
    fs::create_directories(root);
    const auto first = root / "a.enc";
    const auto second = root / "b.enc";
    const auto first_key = root / "a.key";
    const auto second_key = root / "b.key";
    initialize_market_capture_config(config(), first, first_key);
    initialize_market_capture_config(config(), second, second_key);
    const auto credentials = read(second);
    cli::save_market_profile(first, {"Sim account", first_key});
    const std::string alias = "\xe4\xbb\xbf\xe7\x9c\x9f \"CTP\"";
    cli::save_market_profile(second, {alias, second_key});
    std::ostringstream output;
    std::ostringstream errors;
    auto selected = first;
    auto key = first_key;
    std::istringstream pick("2\n");
    cli::select_market_config(selected, key, pick, output, errors);
    require(selected == second && key == second_key && errors.str().empty(),
        "numbered selection automatically adopts the account's matching key");
    require(output.str().find(alias) != std::string::npos,
        "selection lists persisted UTF-8 aliases without decrypting credentials");

    std::istringstream rename("r\nNew name\n");
    cli::select_market_config(selected, key, rename, output, errors);
    require(cli::load_market_profile(second)->alias == "New name" && read(second) == credentials,
        "renaming persists without modifying encrypted credentials");
    const auto saved = read(cli::market_profile_file(second));
    std::istringstream cancel_alias("r\nu\n");
    cli::select_market_config(selected, key, cancel_alias, output, errors);
    require(read(cli::market_profile_file(second)) == saved, "cancelled rename leaves metadata unchanged");
    std::istringstream invalid_alias("r\ninvalid\talias\n");
    cli::select_market_config(selected, key, invalid_alias, output, errors);
    require(read(cli::market_profile_file(second)) == saved
        && errors.str().find("[ERROR]") != std::string::npos, "invalid alias is rejected visibly");
    errors.str("");
    errors.clear();

    unsigned launches = 0;
    std::istringstream capture("4\n1\n3\nSTART\nu\n");
    cli::run_market_capture_menu(selected, key, capture, output, errors,
        [&] (const fs::path& chosen, const fs::path& chosen_key) {
            ++launches;
            require(chosen == first && chosen_key == first_key,
                "capture launches the account and key selected from the account list");
            require(load_market_capture_config(chosen, chosen_key).password == "test-only",
                "selected account can be decrypted with its own key");
            return 0;
        });
    require(launches == 1 && selected == first && key == first_key,
        "capture selection survives returning to the root menu");
    require(errors.str().empty() && output.str().find("test-only") == std::string::npos
        && output.str().find("offline-user") == std::string::npos,
        "account menu and launch confirmation do not expose credentials");
    fs::remove(second_key);
    std::istringstream missing("2\n");
    cli::select_market_config(selected, key, missing, output, errors);
    require(selected == first && key == first_key && errors.str().find("[ERROR]") != std::string::npos,
        "missing saved key does not silently reuse the current account's key");
    replace(cli::market_profile_file(second), "invalid metadata\n");
    std::istringstream corrupt("2\n");
    cli::select_market_config(selected, key, corrupt, output, errors);
    require(selected == first && key == first_key, "corrupt metadata cannot replace selected account");
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
        test_configuration_initialization(root / "initialization");
        test_legacy_import(root / "legacy");
        test_plaintext_import(root / "plaintext");
        test_market_config_selection(root / "selection");
        test_account_profiles(root / "profiles");
        test_initial_ctp_key_menu(root / "initialization_menu");
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
