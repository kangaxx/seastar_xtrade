// Offline menu -> authenticated config -> fake MdApi -> real Tick file chain.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture.hh>
#include "market_capture_login.hh"
#include "market_capture_menu.hh"
#include "market_contract_menu.hh"
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
    int login_response_id = 1;
    bool has_login_response = true;
    bool is_login_response_last = true;
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
        has_expected_login = id == 1 && std::string(request->BrokerID) == "offline"
            && std::string(request->UserID) == "offline-user" && std::string(request->Password) == "test-only";
        CThostFtdcRspUserLoginField response{};
        CThostFtdcRspInfoField error{};
        error.ErrorID = login_error;
        spi->OnRspUserLogin(has_login_response ? &response : nullptr, &error,
                           login_response_id, is_login_response_last);
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

// Product lookup, date formatting and cancellation stay offline and share config validation.
// Source: CLI product guidance request, 2026-11-24. Signature: GitHub Copilot (gpt-6.1-sol).
struct contract_menu_result {
    std::optional<std::vector<market_capture_contract>> contracts;
    std::string output;
    std::string errors;
};

contract_menu_result select_contracts(const std::string& commands) {
    std::istringstream input(commands);
    std::ostringstream output, errors;
    auto contracts = cli::run_market_contract_menu(input, output, errors);
    return {std::move(contracts), output.str(), errors.str()};
}

void test_guided_contract_selection() {
    cli::set_ui_language(cli::ui_language::chinese);
    const auto chinese = select_contracts("1\n1\n202701\n白银\n202702\n3\n1\n202701\nd\n");
    require(chinese.contracts && chinese.contracts->size() == 3 && chinese.errors.empty(),
        "exchange/product numbers and Chinese names can be combined in one selection");
    require((*chinese.contracts)[0].exchange == "SHFE" && (*chinese.contracts)[0].instrument == "rb2701"
        && (*chinese.contracts)[1].instrument == "ag2702"
        && (*chinese.contracts)[2].exchange == "CZCE" && (*chinese.contracts)[2].instrument == "SA701",
        "year/month entry generates correct exchange, case and CZCE three-digit suffix");
    require(chinese.output.find("上海期货交易所") != std::string::npos
        && chinese.output.find("白银") != std::string::npos
        && chinese.output.find("不是实时挂牌清单") != std::string::npos,
        "Chinese guidance shows readable names and does not claim a live contract catalog");
    cli::set_ui_language(cli::ui_language::english);
    const auto english = select_contracts("RB2701\nm2705\nsa701\nif2703\nsc2701\nLC2701\nd\n");
    require(english.contracts && english.contracts->size() == 6 && english.errors.empty(),
        "full contracts infer all six exchanges without an exchange code");
    const std::string exchanges[] = {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
    const std::string instruments[] = {"rb2701", "m2705", "SA701", "IF2703", "sc2701", "lc2701"};
    for (std::size_t index = 0; index < 6; ++index) {
        require((*english.contracts)[index].exchange == exchanges[index]
            && (*english.contracts)[index].instrument == instruments[index], "CTP symbol case is canonical");
    }
    const auto search = select_contracts("copp\nInternational copper\n202701\nd\n");
    require(search.contracts && search.contracts->size() == 1
        && search.contracts->front().exchange == "INE" && search.contracts->front().instrument == "bc2701",
        "ambiguous product search is resolved explicitly instead of guessing an exchange");
    const auto browse = select_contracts("2\n999\n豆粕\n202705\n4\nIF\n202703\n5\n原油\n202701\n6\nlc\n202701\nd\n");
    require(browse.contracts && browse.contracts->size() == 4
        && browse.errors.find("[ERROR]") != std::string::npos,
        "all exchange browsers accept names/codes and allow correcting invalid choices");
    const auto multiple = select_contracts("SHFE:ag2701,GFEX:custom2701\nr\nm\n202705\nd\n");
    require(multiple.contracts && multiple.contracts->size() == 2 && multiple.errors.empty()
        && multiple.contracts->front().instrument == "ag2701"
        && multiple.contracts->back().instrument == "m2705",
        "explicit legacy pairs including unlisted products remain available, with undo");
}

void test_contract_selection_errors() {
    const auto corrected = select_contracts("d\nr\nunknown\nRB2713\nrb\n202713\n202700\n2701\n202701\n"
        "SHFE:ag2701,SHFE:rb2701\nDCE:m2701,\nBAD:m2701\nrb2701\nag2701\nd\n");
    require(corrected.contracts && corrected.contracts->size() == 2
        && corrected.contracts->front().instrument == "rb2701"
        && corrected.contracts->back().instrument == "ag2701",
        "invalid month, unknown exchange, malformed pairs and duplicates cannot corrupt the selected list");
    require(corrected.errors.find("[ERROR]") != std::string::npos, "invalid choices are visible and retryable");
    for (const std::string commands : {"", "u\n", "rb2701\nu\n", "rb2701\n",
            "1\nu\nu\n", "rb\nu\nu\n", "rb\n", "1\n"}) {
        require(!select_contracts(commands).contracts,
            "EOF or cancel anywhere cannot commit even a partially selected contract list");
    }
    std::string full;
    for (unsigned index = 0; index < 64; ++index) { full += "GFEX:custom" + std::to_string(index) + "\n"; }
    const auto limited = select_contracts(full + "GFEX:overflow\nr\nGFEX:replacement\nd\n");
    require(limited.contracts && limited.contracts->size() == 64
        && limited.contracts->back().instrument == "replacement"
        && limited.errors.find("64") != std::string::npos,
        "shared 64-contract limit rejects additions atomically and allows correction");
    validate_market_capture_contracts({});
}

void test_contract_selection_to_tick(const fs::path& root) {
    fixture files(root);
    const auto key = root / "key";
    const auto encrypted = root / "config.enc";
    auto unbound = config();
    unbound.contracts.clear();
    initialize_market_capture_config(unbound, encrypted, key);
    const auto original = read(encrypted);
    auto account = load_market_capture_config(encrypted, key);
    auto selection = select_contracts("螺纹钢\n202701\nd\n");
    require(selection.contracts.has_value(), "guided contracts are explicitly committed before SDK startup");
    account.contracts = std::move(*selection.contracts);
    validate_market_capture_config(account);
    fake_md_api api;
    market_capture_session session(std::move(account), files.paths, root / "state", files.factory(api));
    ready(session);
    api.send(tick());
    session.poll_for(std::chrono::milliseconds(0));
    session.stop();
    require(api.subscriptions == std::vector<std::string>{"rb2701"}
        && session.ticks_written() == 1 && fs::exists(files.output("rb2701", "20261113")),
        "guided product maps to the actual SDK subscription and shared Tick writer");
    require(read(encrypted) == original && load_market_capture_config(encrypted, key).contracts.empty(),
        "runtime selection never rewrites encrypted account credentials or saved contracts");
}

void test_bilingual_login_menu() {
    auto account = config();
    account.password = " English password with spaces ";
    cli::set_ui_language(cli::ui_language::chinese);
    std::istringstream input("SHOW\nu\n");
    std::ostringstream output;
    require(!cli::run_market_login_menu(account, input, output, true),
        "SHOW and u retain their non-connecting behavior in Chinese");
    require(output.str().find("CTP 账号操作") != std::string::npos
        && output.str().find("终端回滚") != std::string::npos
        && output.str().find("\" English password with spaces \"") != std::string::npos,
        "Chinese review localizes warnings but preserves credential bytes");
    const char* language = std::getenv("XTRADER_LANGUAGE");
    require(language != nullptr && std::string(language) == "zh", "worker language is exported for sibling launch");
    std::istringstream start("START\n");
    std::ostringstream start_output;
    require(cli::run_market_login_menu(account, start, start_output, true), "START remains valid in Chinese");
    require(start_output.str().find(account.password) == std::string::npos, "language selection does not reveal passwords automatically");
    cli::set_ui_language(cli::ui_language::english);
}

void test_login_review() {
    auto account = config();
    account.password = " market-password ";
    account.trader.emplace();
    auto& trader = *account.trader;
    trader.trade_front = "tcp://127.0.0.1:2";
    trader.broker_id = "other";
    trader.user_id = "trader-user";
    trader.password = "trader-password";
    trader.app_id = "offline-app";
    trader.auth_code = "offline-auth-code";
    trader.user_product_info = "offline-product";
    const auto has_secrets = [] (const std::string& text) {
        return text.find("market-password") != std::string::npos
            || text.find("trader-password") != std::string::npos
            || text.find("offline-auth-code") != std::string::npos;
    };
    std::ostringstream output;
    std::istringstream confirm("START\n");
    require(cli::run_market_login_menu(account, confirm, output, true),
        "START begins capture directly without a second LOGIN confirmation");
    require(!has_secrets(output.str())
        && output.str().find("UserID: \"offline-user\"") != std::string::npos
        && output.str().find("Market front: \"tcp://127.0.0.1:1\"") != std::string::npos
        && output.str().find("BrokerID: \"offline\"") != std::string::npos
        && output.str().find("1) START") != std::string::npos
        && output.str().find("2) SHOW") != std::string::npos
        && output.str().find("market.password:") == std::string::npos,
        "compact action menu identifies the account without requiring a full configuration review");
    output.str("");
    std::istringstream reveal("sHoW\nsTaRt\n");
    require(cli::run_market_login_menu(account, reveal, output, true),
        "SHOW returns to the same menu and START works case-insensitively");
    require(output.str().find("market.password: \" market-password \"") != std::string::npos
        && output.str().find("trader.password: \"trader-password\"") != std::string::npos
        && output.str().find("trader.auth_code: \"offline-auth-code\"") != std::string::npos
        && output.str().find("trader.user_id: \"trader-user\"") != std::string::npos
        && output.str().find("trader.broker_id: \"other\"") != std::string::npos
        && output.str().find("trader.trade_front: \"tcp://127.0.0.1:2\"") != std::string::npos
        && output.str().find("trader.app_id: \"offline-app\"") != std::string::npos
        && output.str().find("trader.user_product_info: \"offline-product\"") != std::string::npos
        && output.str().find("NOT used for this login") != std::string::npos,
        "full review preserves password spaces and distinguishes stored trader fields from MD login");
    output.str("");
    std::istringstream redirected("SHOW\nSTART\n");
    require(cli::run_market_login_menu(account, redirected, output, false)
        && !has_secrets(output.str()) && redirected.peek() == 'S',
        "non-terminal capture never reveals secrets or consumes commands meant for capture input");
    trader.password.clear();
    trader.auth_code.clear();
    output.str("");
    cli::print_market_login_info(account, output, true);
    require(output.str().find("trader.password: \"(not configured)\"") != std::string::npos
        && output.str().find("trader.auth_code: \"(not configured)\"") != std::string::npos,
        "optional absent secrets are visibly unconfigured");
}

void test_login_review_cancellation() {
    auto account = config();
    account.contracts.clear();
    for (const auto* commands : {"\n", "u\n", "invalid\n", "", "SHOW\n", "2\nu\n", "show\n0\n"}) {
        std::istringstream input(commands);
        std::ostringstream output;
        require(!cli::run_market_login_menu(account, input, output, true)
            && output.str().find("[CANCELLED]") != std::string::npos
            && output.str().find("no connection made") != std::string::npos,
            "SHOW, unknown input or EOF cannot start capture without an explicit START");
        require(output.str().find("Contracts: 0 (enter contracts after START)") != std::string::npos
            && account.contracts.empty(), "opening account actions never requires or changes contracts");
    }
    for (const auto* commands : {"1\n", "start\n", "2\n1\n", "invalid\nSHOW\nSTART\n"}) {
        std::istringstream input(commands);
        std::ostringstream output;
        require(cli::run_market_login_menu(account, input, output, true),
            "numbered commands and recovery after an invalid action reach START without LOGIN");
        if (std::string(commands).starts_with("invalid")) {
            require(output.str().find("[ERROR] Unknown account action") != std::string::npos,
                "unknown commands report an error rather than silently cancelling");
        }
    }
}

void test_market_status(const fs::path& root) {
    fixture files(root);
    fake_md_api api;
    market_capture_session session(config(), files.paths, root / "state", files.factory(api));
    const auto has_state = [&session] (const char* expected) {
        std::ostringstream output;
        session.print_status(output);
        return output.str().find(expected) != std::string::npos
            && output.str().find("test-only") == std::string::npos
            && output.str().find("offline-user") == std::string::npos;
    };
    require(has_state("[MARKET] idle (not connected);"), "status distinguishes local configuration from a connection");
    session.start();
    require(has_state("[MARKET] connecting;"), "status shows connection attempts before the front callback");
    session.poll_for(std::chrono::milliseconds(0));
    require(has_state("[MARKET] logging_in;"), "status identifies a submitted login request");
    session.poll_for(std::chrono::milliseconds(0));
    require(has_state("[MARKET] subscribing (login succeeded);"), "subscription state explicitly confirms MD login");
    for (int index = 0; index < 2; ++index) { session.poll_for(std::chrono::milliseconds(0)); }
    require(has_state("[MARKET] subscribed;"), "ready status still requires all subscription acknowledgements");
    session.stop();
    require(has_state("[MARKET] stopped;"), "stopped status cannot be mistaken for a new connection attempt");
}

void test_startup_data_directories(const fs::path& root) {
    fs::create_directories(root);
    const market_data_paths paths(root / "data");
    fake_md_api api;
    market_capture_session session(config(), paths, root / "state1",
        [&api] (const char*) { return &api; });
    ready(session);
    require(fs::is_directory(paths.root()) && fs::is_directory(paths.source_root()),
        "startup creates the missing data root and capture source before connecting");
    for (const auto& directory : {paths.root(), paths.source_root()}) {
        struct stat status{};
        require(::stat(directory.c_str(), &status) == 0 && (status.st_mode & 0007) == 0,
            "new data directories do not grant permissions to other users");
    }
    api.send(tick());
    session.poll_for(std::chrono::milliseconds(0));
    session.stop();
    const auto file = paths.data_file(
        {"x_trader_data", "futures", "SHFE", "rb2701", "tick"}, "20261113");
    const auto saved = read(file);
    require(::chmod(paths.root().c_str(), 0755) == 0
        && ::chmod(paths.source_root().c_str(), 0700) == 0, "set existing directory permissions");
    fake_md_api next_api;
    market_capture_session restarted(config(), paths, root / "state2",
        [&next_api] (const char*) { return &next_api; });
    ready(restarted);
    restarted.stop();
    struct stat root_status{};
    struct stat source_status{};
    require(::stat(paths.root().c_str(), &root_status) == 0
        && ::stat(paths.source_root().c_str(), &source_status) == 0
        && (root_status.st_mode & 0777) == 0755 && (source_status.st_mode & 0777) == 0700
        && read(file) == saved, "repeated startup preserves existing permissions and Tick files");
}

void test_startup_directory_failures(const fs::path& root) {
    fs::create_directories(root);
    for (const bool blocked_source : {false, true}) {
        const market_data_paths paths(root / (blocked_source ? "source_file" : "root_file"));
        if (blocked_source) { fs::create_directories(paths.root()); }
        const auto blocked = blocked_source ? paths.source_root() : paths.root();
        replace(blocked, "keep existing file\n");
        unsigned api_creations = 0;
        fake_md_api api;
        market_capture_session session(config(), paths, root / "state",
            [&] (const char*) { ++api_creations; return &api; });
        require_failure([&] { session.start(); }, "a file occupying a data directory cannot be overwritten");
        require(api_creations == 0 && read(blocked) == "keep existing file\n" && !fs::exists(root / "state"),
            "invalid data directories fail before SDK creation or state-directory creation");
    }
    const market_data_paths paths(root / "linked_source");
    const auto outside = root / "outside";
    fs::create_directories(paths.root());
    fs::create_directories(outside);
    fs::create_directory_symlink(outside, paths.source_root());
    unsigned api_creations = 0;
    fake_md_api api;
    market_capture_session session(config(), paths, root / "state",
        [&] (const char*) { ++api_creations; return &api; });
    require_failure([&] { session.start(); }, "a linked source cannot redirect directory provisioning");
    require(api_creations == 0 && fs::is_empty(outside),
        "source redirection is rejected without creating files outside the source or connecting CTP");
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
    std::istringstream input("3\nshow\nu\n3\nstart\nu\n");
    std::ostringstream output;
    std::ostringstream errors;
    unsigned launches = 0;
    unsigned connections = 0;
    cli::run_market_capture_menu(encrypted, key, input, output, errors,
        [&] (const fs::path& selected_config, const fs::path& selected_key) {
            ++launches;
            fake_md_api api;
            auto account = load_market_capture_config(selected_config, selected_key);
            std::ostringstream worker_output;
            if (!cli::run_market_login_menu(account, input, worker_output, true)) {
                require(api.spi == nullptr && api.subscriptions.empty(),
                    "returning from account actions does not initialize the SDK or subscribe");
                require(worker_output.str().find("market.password: \"test-only\"") != std::string::npos,
                    "option 3 can SHOW passwords immediately without first selecting START");
                return 0;
            }
            ++connections;
            market_capture_session session(std::move(account),
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
    require(launches == 2 && connections == 1 && errors.str().empty(),
        "option 3 opens actions immediately; only START connects, without a second confirmation");
    require(output.str().find("Type START to confirm") == std::string::npos,
        "capture menu has no extra START prompt before the account actions");
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
    require(output.str().find("login has NOT been attempted. This is not a login failure.") != std::string::npos,
        "local selection cannot be mistaken for a rejected account");

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
            const auto account = load_market_capture_config(chosen, chosen_key);
            require(account.password == "test-only",
                "selected account can be decrypted with its own key");
            std::ostringstream worker_output;
            require(cli::run_market_login_menu(account, capture, worker_output, true),
                "selected profile reaches its action menu directly");
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

// Exercise MD response correlation and terminal payload checks without a real front.
// Source: code=0 login report, 2026-11-24. Signature: GitHub Copilot (gpt-6.1-sol).
void test_login_response_variants(const fs::path& root) {
    fixture files(root);
    {
        fake_md_api api;
        api.login_response_id = 0;
        market_capture_session session(config(), files.paths, root / "zero_id", files.factory(api));
        ready(session);
        require(api.has_expected_login && api.subscriptions.size() == 2,
            "zero-ID response completes the only pending login and subscribes once");
        api.send(tick());
        session.poll_for(std::chrono::milliseconds(0));
        session.stop();
        require(session.ticks_written() == 1 && fs::exists(files.output("rb2701", "20261113")),
            "zero-ID login can write a real Tick through the shared writer");
    }
    {
        fake_md_api api;
        api.has_login_response = false;
        api.is_login_response_last = false;
        market_capture_session session(config(), files.paths, root / "partial", files.factory(api));
        session.start();
        session.poll_for(std::chrono::milliseconds(0));
        session.poll_for(std::chrono::milliseconds(0));
        require(api.subscriptions.empty() && !session.is_ready(),
            "empty non-final response does not subscribe or fail prematurely");
        CThostFtdcRspUserLoginField response{};
        api.spi->OnRspUserLogin(&response, nullptr, 1, true);
        for (int index = 0; index < 3; ++index) { session.poll_for(std::chrono::milliseconds(0)); }
        require(session.is_ready(), "successful final response with absent error info completes login");
        session.stop();
    }
    for (int variant = 0; variant < 3; ++variant) {
        fake_md_api api;
        if (variant == 0) { api.login_response_id = 2; }
        if (variant == 1) { api.has_login_response = false; }
        if (variant == 2) { api.login_response_id = 0; api.login_error = 7; }
        market_capture_session session(config(), files.paths, root / ("rejected_" + std::to_string(variant)),
                                       files.factory(api));
        session.start();
        session.poll_for(std::chrono::milliseconds(0));
        std::string failure;
        try { session.poll_for(std::chrono::milliseconds(0)); }
        catch (const std::runtime_error& error) { failure = error.what(); }
        const char* expected[] = {"request ID mismatch", "final login response has no payload", "login response failed"};
        require(failure.find(expected[variant]) != std::string::npos
            && failure.find(variant == 2 ? "code=7" : "code=0") != std::string::npos
            && failure.find("request_id=") != std::string::npos
            && failure.find("state=logging_in") != std::string::npos
            && failure.find("has_payload=") != std::string::npos
            && failure.find("is_last=true") != std::string::npos,
            "login failure reports its actual reason and callback context");
        require(api.subscriptions.empty() && !session.is_ready(),
            "mismatched, missing or failed final response never subscribes");
        session.stop();
    }
    {
        fake_md_api api;
        market_capture_session session(config(), files.paths, root / "duplicate", files.factory(api));
        ready(session);
        CThostFtdcRspUserLoginField response{};
        api.spi->OnRspUserLogin(&response, nullptr, 0, true);
        require_failure([&] { session.poll_for(std::chrono::milliseconds(0)); },
            "zero-ID compatibility cannot accept a second login outside logging_in");
        require(api.subscriptions.size() == 2 && !session.is_ready(),
            "duplicate response cannot issue another subscription");
        session.stop();
    }
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
        std::ostringstream status;
        session.print_status(status);
        require(status.str().find("[MARKET] failed;") != std::string::npos,
            "rejected login is distinct from an unattempted login or pending connection");
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
        cli::set_ui_language(cli::ui_language::english);
        test_guided_contract_selection();
        test_contract_selection_errors();
        test_contract_selection_to_tick(root / "contract_selection");
        test_bilingual_login_menu();
        test_login_review();
        test_login_review_cancellation();
        test_market_status(root / "status");
        test_startup_data_directories(root / "startup_directories");
        test_startup_directory_failures(root / "startup_directory_failures");
        test_authenticated_config(root / "crypto");
        test_configuration_initialization(root / "initialization");
        test_legacy_import(root / "legacy");
        test_plaintext_import(root / "plaintext");
        test_market_config_selection(root / "selection");
        test_account_profiles(root / "profiles");
        test_initial_ctp_key_menu(root / "initialization_menu");
        test_menu_to_tick_chain(root / "chain");
        test_login_response_variants(root / "login_responses");
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
