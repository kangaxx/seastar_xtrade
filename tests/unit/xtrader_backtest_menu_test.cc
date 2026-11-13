// Backtest setup interactions only; Run uses the same under-construction result.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include "backtest_menu.hh"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using namespace seastar::xtrader;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_file(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "fixture; this file is never decoded\n";
    require(static_cast<bool>(output), "create data fixture");
}

struct fixture_files {
    fs::path first;
    fs::path second;
    fs::path equity;
    fs::path historical;
};

fixture_files prepare_data(const market_data_paths& paths) {
    fixture_files files{
        paths.data_file({"x_trader_data", "futures", "SHFE", "rb2601", "1m"}, "20261112"),
        paths.data_file({"x_trader_data", "futures", "SHFE", "rb2601", "1m"}, "20261113"),
        paths.data_file({"x_trader_data", "equity", "NYSE", "IBM", "1d"}, "20261113"),
        paths.data_file({"historic", "forex", "NONE", "EURUSD", "native"}, "20261113", "jsonl")};
    for (const auto& file : {files.first, files.second, files.equity, files.historical}) {
        write_file(file);
    }
    write_file(files.first.parent_path() / "dataset-manifest.conf");
    write_file(files.first.parent_path() / "notes.txt");
    write_file(files.first.parent_path() / "20261114.csv.partial");
#ifndef _WIN32
    fs::create_symlink(files.first, files.first.parent_path() / "linked.csv");
    fs::create_directory_symlink(paths.source_root("historic"), paths.source_root() / "linked_directory");
#endif
    return files;
}

struct menu_result {
    std::vector<cli::backtest_selection> runs;
    std::string output;
    std::string errors;
};

menu_result run_menu(const market_data_paths& paths, const std::string& commands) {
    menu_result result;
    std::istringstream input(commands);
    std::ostringstream output;
    std::ostringstream errors;
    cli::run_backtest_menu(paths, input, output, errors,
        [&result] (const cli::backtest_selection& selection) {
            result.runs.push_back(selection);
            return cli::backtest_placeholder_result();
        });
    result.output = output.str();
    result.errors = errors.str();
    return result;
}

std::string source_number(const market_data_paths& paths, const std::string& source) {
    const auto directories = paths.source_directories();
    const auto found = std::find(directories.begin(), directories.end(), paths.source_root(source));
    require(found != directories.end(), "source fixture exists");
    return std::to_string(std::distance(directories.begin(), found) + 1);
}

void test_default_multi_file_run(const market_data_paths& paths, const fixture_files& files) {
    // Product choices: all, equity, futures. Then explicitly choose both futures files.
    const auto result = run_menu(paths, "2\n3\n3\n2,1\n4\n1\n5\nu\n");
    require(result.runs.size() == 1 && result.errors.empty(), "configured Run is reached once");
    const auto& selection = result.runs.front();
    require(selection.data_root == paths.source_root(), "default is the system-captured source");
    require(selection.product_directory == paths.source_root() / "futures", "actual product directory selected");
    require(selection.data_files == std::vector<fs::path>{files.second, files.first},
        "multiple actual files are transferred in requested order");
    require(selection.strategy_id == "Virtual", "Virtual placeholder strategy selected");
    require(result.output.find("[Backtest Run]\n施工中") != std::string::npos, "Run reports construction, not success");
    require(result.output.find("notes.txt") == std::string::npos
        && result.output.find("csv.partial") == std::string::npos
        && result.output.find("linked.csv") == std::string::npos,
        "metadata, incomplete files and links are not offered as data");
}

void test_product_selection_without_file_picker(const market_data_paths& paths,
                                                const fixture_files& files) {
    const auto result = run_menu(paths, "2\n3\n4\n1\n5\nu\n");
    require(result.runs.size() == 1 && result.errors.empty(), "product-only selection can reach Run");
    require(result.runs.front().data_files == std::vector<fs::path>{files.first, files.second},
        "a selected product includes its real data files");
}

void test_alternative_source_and_single_file(const market_data_paths& paths,
                                             const fixture_files& files) {
    const auto result = run_menu(paths, "1\n" + source_number(paths, "historic")
        + "\n3\n1\n4\n1\n5\nu\n");
    require(result.runs.size() == 1 && result.errors.empty(), "alternative source can be configured");
    require(result.runs.front().data_root == paths.source_root("historic")
        && result.runs.front().data_files == std::vector<fs::path>{files.historical},
        "single file and selected source reach the Run handler");
}

void test_invalid_choices_and_unconfigured_run(const market_data_paths& paths,
                                               const fixture_files& files) {
    const auto result = run_menu(paths,
        "5\n2\n3\n3\n0\n999\n1,1\n1junk\n1\n4\n1\n5\nu\n");
    require(result.runs.size() == 1, "invalid and unconfigured actions never call Run");
    require(result.errors.find("select data files") != std::string::npos
        && result.errors.find("select distinct listed numbers") != std::string::npos,
        "missing configuration and invalid file choices are visible");
    require(result.runs.front().data_files == std::vector<fs::path>{files.first},
        "corrected single file replaces the product-wide selection");
}

void test_source_and_product_changes(const market_data_paths& paths, const fixture_files& files) {
    const auto changed_source = run_menu(paths, "2\n3\n4\n1\n1\n"
        + source_number(paths, "historic") + "\n5\nu\n");
    require(changed_source.runs.empty() && changed_source.errors.find("select data files") != std::string::npos,
        "changing source invalidates previously selected files");
    const auto changed_product = run_menu(paths, "2\n3\n2\n2\n4\n1\n5\nu\n");
    require(changed_product.runs.size() == 1
        && changed_product.runs.front().data_files == std::vector<fs::path>{files.equity},
        "changing product replaces old files instead of mixing products");
    const auto cancelled = run_menu(paths, "3\nu\n4\nu\n5\nu\n");
    require(cancelled.runs.empty(), "cancelled choices do not create a runnable configuration");
}

void test_empty_and_missing_sources(const fs::path& root) {
    market_data_paths paths(root);
    const auto missing = run_menu(paths, "3\n5\nu\n");
    require(missing.runs.empty() && missing.errors.find("[ERROR]") != std::string::npos
        && !fs::exists(root), "missing data is reported without creating directories");
    fs::create_directories(paths.source_root());
    const auto empty = run_menu(paths, "3\n4\n1\n5\nu\n");
    require(empty.runs.empty() && empty.output.find("[WARN]") != std::string::npos,
        "empty data never runs despite a selected strategy");
}

} // namespace

int main() {
    const auto parent = fs::temp_directory_path();
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path fixture;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = parent / ("xtrader_backtest_menu_" + std::to_string(seed) + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) {
            fixture = candidate;
            break;
        }
    }
    if (fixture.empty()) {
        std::cerr << "cannot reserve backtest-menu test directory\n";
        return 1;
    }
    try {
        market_data_paths paths(fixture / "data");
        const auto files = prepare_data(paths);
        test_default_multi_file_run(paths, files);
        test_product_selection_without_file_picker(paths, files);
        test_alternative_source_and_single_file(paths, files);
        test_invalid_choices_and_unconfigured_run(paths, files);
        test_source_and_product_changes(paths, files);
        test_empty_and_missing_sources(fixture / "missing_data");
        fs::remove_all(fixture);
        std::cout << "backtest menu setup and placeholder cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(fixture);
        return 1;
    }
}
