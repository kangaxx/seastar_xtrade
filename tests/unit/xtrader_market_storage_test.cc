// End-to-end daily import/capture readback without a reactor or CTP connection.
// Source: user request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#include <seastar/xtrader/data_importer.hh>
#include <seastar/xtrader/historical_data_manager.hh>
#include "market_tick_writer.hh"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace seastar::xtrader;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

template<typename Function>
void rejects(Function&& function) {
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("expected explicit rejection");
}

void write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.flush();
    require(static_cast<bool>(output), "write test fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

import_options options_for(const fs::path& root, const fs::path& input) {
    const auto config = root / "import.conf";
    write(config, "data_root=" + (root / "data").string()
        + "\nreport_root=" + (root / "reports").string() + "\n");
    import_options options;
    options.source_path = input.string();
    options.config_path = config.string();
    return options;
}

const std::string TAOBAO_HEADER = "date,open,high,low,close,volume,money,open_interest,symbol\n";

void test_bar_periods(const fs::path& root) {
    const market_data_paths paths(root / "data");
    for (const std::string period : {"5s", "2h", "1d", "1week", "2d", "4h"}) {
        const auto input = root / "external" / ("rb2601.SHFE." + period + ".csv");
        auto options = options_for(root, input);
        write(input, TAOBAO_HEADER
            + "2026/11/22 09:00:05,100,102,99,101,10,1000,20,\"batch,rb2601\"\n"
            + "2026/11/23 09:00:05,101,103,100,102,11,1100,21,rb2601\n");
        const auto imported = data_importer{}.execute(options);
        require(imported.success && imported.rows_added == 2, "Taobao import: " + imported.message);
        require(imported.file_reports.front().target_files.size() == 2, "partition both calendar dates");
        const market_data_key key{"taobao", "futures", "SHFE", "rb2601", period};
        const auto first_file = paths.data_file(key, "20261122");
        require(imported.target_root == paths.source_root("taobao") && fs::exists(first_file),
            "purchased data uses registered Taobao source and standard layout");
        require(!fs::exists(paths.manifest_file(key)), "daily data does not require a competing active_file manifest");
        const auto before = read(first_file);
        const auto repeated = data_importer{}.execute(options);
        require(repeated.success && repeated.rows_skipped == 2 && read(first_file) == before,
            "incremental import is idempotent");

        historical_data_manager manager(paths.source_root("taobao"));
        std::string warning;
        const auto dataset = manager.find_dataset("rb2601", "SHFE", period, &warning);
        require(dataset.has_value() && warning.empty(), "discover periodic dataset: " + warning);
        require(dataset->files.size() == 2 && dataset->kind == market_data_kind::bar
            && dataset->source == "taobao" && dataset->asset_class == "futures",
            "source, kind, asset and arbitrary timeframe survive discovery");
        std::vector<historical_bar> bars;
        require(manager.load_bars(*dataset, bars, &warning, historical_load_mode::strict),
            "read both daily partitions: " + warning);
        require(bars.size() == 2 && bars.front().datetime == "2026-11-22 09:00:05"
            && bars.back().close == 102 && bars.front().open_interest == 20,
            "seconds, OHLC and open interest are not reordered/lost");
        require(bars.back().source_line == 2 && bars.back().source_file == paths.data_file(key, "20261123"),
            "multi-file readers preserve the actual row location");
        std::vector<market_csv_row> ticks;
        require(!manager.load_ticks(*dataset, ticks, &warning) && ticks.empty(),
            "bar files cannot enter the Tick reader");
        const auto second_before = read(paths.data_file(key, "20261123"));
        write(input, TAOBAO_HEADER + "2026/11/22 09:00:05,100,102,99,100,10,1000,20,rb2601\n");
        options.mode = import_mode::overwrite;
        const auto overwritten = data_importer{}.execute(options);
        require(overwritten.success && overwritten.rows_overwritten == 1
            && read(paths.data_file(key, "20261123")) == second_before,
            "overwrite replaces matching timestamps, not unrelated dates");
    }
}

void test_live_tick_and_import(const fs::path& root) {
    fs::create_directories(root);
    const market_data_paths paths(root / "data");
    CThostFtdcDepthMarketDataField tick{};
    std::strcpy(tick.ExchangeID, "SHFE");
    std::strcpy(tick.InstrumentID, "rb2601");
    std::strcpy(tick.ActionDay, "20261122");
    std::strcpy(tick.TradingDay, "20261123");
    std::strcpy(tick.UpdateTime, "21:00:05");
    tick.UpdateMillisec = 7;
    tick.Volume = 10;
    tick.LastPrice = 101;
    tick.BidPrice1 = 100;
    tick.AskPrice1 = 102;
    const market_data_key live{"x_trader_data", "futures", "SHFE", "rb2601", "tick"};
    capture_detail::market_tick_writer collector(paths, {{"SHFE", "rb2601"}});
    collector.write(tick, 1, 1'795'000'000'000LL);
    const auto live_file = paths.data_file(live, "20261122");
    const auto live_text = read(live_file);

    historical_data_manager live_reader(paths.source_root());
    std::string warning;
    const auto dataset = live_reader.find_dataset("rb2601", "SHFE", "tick", &warning);
    require(dataset.has_value(), "captured Tick is discoverable without manifest");
    std::vector<market_csv_row> ticks;
    require(live_reader.load_ticks(*dataset, ticks, &warning) && ticks.size() == 1,
        "read live raw Tick: " + warning);
    require(ticks.front()[6] == "20261122" && ticks.front()[7] == "20261123"
        && ticks.front()[9] == "7" && ticks.front()[10] == "101",
        "retain action/trading dates, milliseconds and raw fields");
    std::vector<historical_bar> bars;
    require(!live_reader.load_bars(*dataset, bars, &warning) && bars.empty(),
        "Tick cannot be silently read as OHLC");

    market_data_writer competing(paths);
    rejects([&] { (void)competing.merge(live, ticks, true); });
    collector.close();
    const auto input = root / "downloaded" / "rb2601.SHFE.tick.csv";
    write(input, live_text);
    auto options = options_for(root, input);
    options.source_type = import_source_type::ctp_clean_csv;
    options.destination_source = "taobao";
    const auto imported = data_importer{}.execute(options);
    require(imported.success, "canonical Tick import: " + imported.message);
    auto purchased = live;
    purchased.source = "taobao";
    require(read(paths.data_file(purchased, "20261122")) == live_text,
        "import uses identical Tick header and payload; only source directory differs");
    historical_data_manager purchased_reader(paths.source_root("taobao"));
    const auto purchased_dataset = purchased_reader.find_dataset("rb2601", "SHFE", "tick", &warning);
    require(purchased_dataset && purchased_reader.load_ticks(*purchased_dataset, ticks, &warning),
        "identical reader handles imported Tick");
    historical_data_manager all_sources(paths.root());
    require(!all_sources.find_dataset("rb2601", "SHFE", "tick", &warning)
        && warning.find("Ambiguous") != std::string::npos, "cross-source ambiguity is not resolved arbitrarily");
}

void test_explicit_identity_and_failures(const fs::path& root) {
    const market_data_paths paths(root / "data", {"vendor_batch"});
    const auto input = root / "outside" / "arbitrary-name.csv";
    auto options = options_for(root, input);
    options.instrument = "IF2601";
    options.exchange = "CFFEX";
    options.timeframe = "1w";
    options.destination_source = "vendor_batch";
    write(input, TAOBAO_HEADER + "2026-11-23,100,102,99,101,10,1000,20,IF2601\n");
    const auto imported = data_importer{}.execute(options);
    require(imported.success, "arbitrary input location/name and explicit identity");
    const market_data_key key{"vendor_batch", "futures", "CFFEX", "IF2601", "1week"};
    const auto destination = paths.data_file(key, "20261123");
    require(fs::exists(destination), "case is preserved and weekly alias has one canonical directory");
    std::string warning;
    historical_data_manager reader(paths.source_root("vendor_batch"));
    const auto dataset = reader.find_dataset("IF2601", "CFFEX", "1week", &warning);
    std::vector<historical_bar> bars;
    require(dataset && reader.load_bars(*dataset, bars, &warning), "custom source readback");
    const auto before = read(destination);
    for (const std::string bad : {"2026-02-29", "2026-11-24 25:00:00"}) {
        write(input, TAOBAO_HEADER + bad + ",100,102,99,101,10,1000,20,IF2601\n");
        const auto failed = data_importer{}.execute(options);
        require(!failed.success && failed.invalid_rows == 1 && read(destination) == before,
            "invalid dates/times do not publish any source rows");
    }
    write(input, TAOBAO_HEADER + "2026-11-23,100,102,99,101,10,1000,20,IF2601\n");
    write(destination, before + "incomplete");
    const auto corrupted = read(destination);
    require(!data_importer{}.execute(options).success && read(destination) == corrupted,
        "existing corrupt data is not silently truncated/repaired");
    require(!reader.load_bars(*dataset, bars, &warning) && bars.empty(),
        "reader rejects partial data without returning partial success");
    for (const char* period : {"", "0s", "01m", "-1h", "2months", "../1m"}) {
        rejects([&] { (void)market_data_paths::normalize_timeframe(period); });
    }
    options.timeframe = "tick";
    require(!data_importer{}.execute(options).success, "bar schema conflicts with Tick timeframe");
    options.timeframe = "1week";
    options.destination_source = "../escape";
    require(!data_importer{}.execute(options).success, "destination source cannot escape shared root");
}

void test_legacy_and_links(const fs::path& root) {
    const auto legacy = root / "legacy";
    const auto directory = legacy / "dominant" / "SHFE" / "rb99" / "1m";
    const auto csv = directory / "rb99.SHFE.1m.imported.csv";
    const auto manifest = directory / "dataset-manifest.conf";
    write(csv, "date,open,high,low,close,volume,open_interest\n"
        "2026/11/23 09:00,100,102,99,101,10,20\n");
    const std::string metadata = "symbol=rb99\nexchange=SHFE\ntimeframe=1m\nsource=taobao_csv\n";
    write(manifest, metadata + "active_file=" + csv.filename().string() + "\n");
    historical_data_manager reader(legacy);
    std::string warning;
    auto datasets = reader.scan_datasets(&warning);
    std::vector<historical_bar> bars;
    require(warning.empty() && datasets.size() == 1 && datasets.front().layout_version == 1
        && reader.load_bars(datasets.front(), bars, &warning, historical_load_mode::strict),
        "confined legacy aggregate files remain readable without migration");
    const auto old_daily_name = directory / "20261123.csv";
    fs::rename(csv, old_daily_name);
    write(manifest, metadata + "active_file=20261123.csv\n");
    datasets = reader.scan_datasets(&warning);
    require(warning.empty() && datasets.size() == 1 && datasets.front().layout_version == 1
        && reader.load_bars(datasets.front(), bars, &warning),
        "a legacy manifest owns its date-named file rather than being reclassified by scan order");
    write(manifest, metadata + "active_file=" + (root / "outside.csv").string() + "\n");
    require(reader.scan_datasets(&warning).empty() && !warning.empty(),
        "legacy absolute active_file cannot escape selected root");
    const market_data_paths paths(root / "data");
    fs::create_directories(paths.root());
    fs::create_directory_symlink(legacy, paths.source_root("taobao"));
    market_data_writer writer(paths);
    const market_data_key key{"taobao", "futures", "SHFE", "rb2601", "5s"};
    rejects([&] { (void)writer.merge(key,
        {{"2026-11-23 09:00:05", "100", "102", "99", "101", "10", "20", "", ""}}, false); });
    historical_data_manager linked_reader(paths.root());
    require(linked_reader.scan_datasets(&warning).empty() && !warning.empty(),
        "writer and reader both refuse redirected sources");
}
} // namespace

int main() {
    fs::path root;
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = fs::temp_directory_path()
            / ("xtrader_storage_" + std::to_string(seed) + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) { root = candidate; break; }
    }
    if (root.empty()) { std::cerr << "cannot reserve fixture directory\n"; return 1; }
    try {
        test_bar_periods(root / "bars");
        test_live_tick_and_import(root / "ticks");
        test_explicit_identity_and_failures(root / "explicit");
        test_legacy_and_links(root / "legacy-boundaries");
        fs::remove_all(root);
        std::cout << "shared market storage/import/readback cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
