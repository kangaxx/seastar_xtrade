// Shared layout and rejection cases, without a reactor, broker or production data.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_data_paths.hh>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

namespace fs = std::filesystem;
using namespace seastar::xtrader;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

market_data_key futures_key() {
    return {"x_trader_data", "futures", "SHFE", "rb2601", "1m"};
}

void test_layout(const fs::path& root) {
    market_data_paths paths(root);
    auto key = futures_key();
    for (const std::string source : {"x_trader_data", "historic", "taobao", "virtual_data"}) {
        key.source = source;
        const auto expected = root / source / "futures" / "SHFE" / "rb2601" / "1m";
        require(paths.dataset_directory(key) == expected, "shared layout without a product-family level");
        require(paths.data_file(key, "20261113") == expected / "20261113.csv", "daily filename");
        require(paths.manifest_file(key) == expected / "dataset-manifest.conf", "manifest location");
        const auto parsed = paths.parse_data_file(paths.data_file(key, "20261113", "parquet"));
        require(parsed.key.source == source && parsed.key.asset_class == "futures"
            && parsed.key.exchange == "SHFE" && parsed.key.instrument == "rb2601"
            && parsed.key.timeframe == "1m" && parsed.calendar_date == "20261113"
            && parsed.format == "parquet", "file identity round trip");
    }
    key = {"virtual_data", "timeseries", "NONE", "temperature_sensor_01", "native"};
    const auto file = paths.data_file(key, "20240229", "jsonl");
    require(paths.parse_data_file(file).key.asset_class == "timeseries", "non-futures identity preserved");
    key.instrument = "RB_CONTINUOUS";
    require(paths.parse_data_file(paths.data_file(key, "20261113")).key.instrument == "RB_CONTINUOUS",
        "explicit sequence name is not truncated or case-folded");
    require(!fs::exists(root), "path construction must not create directories");
}

void test_invalid_inputs(const fs::path& root) {
    market_data_paths paths(root);
    const auto key = futures_key();
    for (const char* invalid : {"", "..", ".", "../escape", "rb/2601", "rb\\2601", "C:rb", "rb 2601"}) {
        auto bad_key = key;
        bad_key.instrument = invalid;
        require_invalid([&] { (void)paths.dataset_directory(bad_key); });
    }
    auto bad_key = key;
    bad_key.instrument = std::string("rb\0hidden", 9);
    require_invalid([&] { (void)paths.data_file(bad_key, "20261113"); });
    for (auto field : {&market_data_key::source, &market_data_key::asset_class,
                      &market_data_key::exchange, &market_data_key::timeframe}) {
        bad_key = key;
        bad_key.*field = "";
        require_invalid([&] { (void)paths.dataset_directory(bad_key); });
    }
    bad_key = key;
    bad_key.source = "unregistered";
    require_invalid([&] { (void)paths.dataset_directory(bad_key); });
    for (const char* date : {"20260229", "20261301", "20260431", "00000101", "2026111", "2026-11-13"}) {
        require_invalid([&] { (void)paths.data_file(key, date); });
    }
    require_invalid([&] { (void)paths.data_file(key, "19000229"); });
    (void)paths.data_file(key, "20000229");
    for (const char* format : {"", ".csv", "CSV", "../csv", "csv.partial", "csv/other"}) {
        require_invalid([&] { (void)paths.data_file(key, "20261113", format); });
    }
    require_invalid([&] { (void)market_data_paths("relative"); });
    require_invalid([&] { (void)market_data_paths(root.root_path()); });
    require_invalid([&] { (void)market_data_paths(root / ".." / "other"); });
    require_invalid([&] { (void)market_data_paths(root, {"historic"}); });
    require_invalid([&] { (void)market_data_paths(root, {"new_source", "new_source"}); });
    require_invalid([&] { (void)market_data_paths(root, {"../escape"}); });
    market_data_paths extended(root, {"other_feed"});
    auto other_key = key;
    other_key.source = "other_feed";
    require(extended.parse_data_file(extended.data_file(other_key, "20261113")).key.source == "other_feed",
        "new sources require explicit registration");
}

void test_path_rejection(const fs::path& root) {
    market_data_paths paths(root);
    const auto file = paths.data_file(futures_key(), "20261113");
    require_invalid([&] { (void)paths.parse_data_file(file.lexically_relative(root)); });
    require_invalid([&] { (void)paths.parse_data_file(root / ".." / file.lexically_relative(root)); });
    require_invalid([&] { (void)paths.parse_data_file(root.string() + "_other/" + file.lexically_relative(root).generic_string()); });
    require_invalid([&] { (void)paths.parse_data_file(file.parent_path() / "extra" / file.filename()); });
    require_invalid([&] { (void)paths.parse_data_file(paths.manifest_file(futures_key())); });
    require_invalid([&] { (void)paths.parse_data_file(file.parent_path() / "20261113.csv.partial"); });
    require_invalid([&] { (void)paths.parse_data_file(file.parent_path() / "20260229.csv"); });
    paths.validate_filesystem_path(file);
    paths.validate_filesystem_path(paths.manifest_file(futures_key()));
}

void test_filesystem_boundaries(const fs::path& root) {
    market_data_paths paths(root);
    const auto file = paths.data_file(futures_key(), "20261113");
    fs::create_directories(file.parent_path());
    paths.validate_filesystem_path(file);
    {
        std::ofstream output(file);
        output << "fixture";
        require(static_cast<bool>(output), "write fixture");
    }
    paths.validate_filesystem_path(file);
#ifndef _WIN32
    // Symlink creation on Windows may require a separate user privilege.
    const auto original_source = paths.source_root("x_trader_data");
    fs::rename(original_source, paths.source_root("historic"));
    fs::create_directory_symlink(paths.source_root("historic"), original_source);
    require_invalid([&] { paths.validate_filesystem_path(file); });
    fs::remove(original_source);
    fs::create_directory_symlink(root.parent_path(), original_source);
    require_invalid([&] { paths.validate_filesystem_path(file); });
    fs::remove(original_source);
    fs::create_directory_symlink(root / "missing_source", original_source);
    require_invalid([&] { paths.validate_filesystem_path(file); });
    fs::remove(original_source);
#endif
}

} // namespace

int main() {
    const auto parent = fs::temp_directory_path();
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root;
    // Atomic creation reserves a unique fixture parent; never touch /data.
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = parent / ("xtrader_paths_" + std::to_string(seed) + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) {
            root = candidate / "data";
            break;
        }
    }
    if (root.empty()) {
        std::cerr << "cannot reserve test directory\n";
        return 1;
    }
    try {
#ifndef _WIN32
        require(market_data_paths{}.root() == fs::path("/data"), "default production root");
#endif
        test_layout(root);
        test_invalid_inputs(root);
        test_path_rejection(root);
        test_filesystem_boundaries(root);
        fs::remove_all(root.parent_path());
        std::cout << "market data paths tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root.parent_path());
        return 1;
    }
}
