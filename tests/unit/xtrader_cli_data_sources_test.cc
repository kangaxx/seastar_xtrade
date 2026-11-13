// CLI data-source selection using the same menu as xtrader_cli, without a reactor.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include "data_source_menu.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;
using namespace seastar::xtrader;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_listing_and_selection(const fs::path& root) {
    market_data_paths paths(root);
    auto selected = paths.source_root();
    require(selected == root / "x_trader_data", "default comes from the path manager");
    for (const char* name : {"x_trader_data", "historic", "taobao", "virtual_data",
                             "custom_feed", "legacy source", "lost+found"}) {
        fs::create_directories(root / name);
    }
    fs::create_directory(root / "historic" / "nested");
    {
        std::ofstream output(root / "not_a_directory");
        output << "fixture";
        require(static_cast<bool>(output), "create regular-file fixture");
    }
#ifndef _WIN32
    fs::create_directory_symlink(root.parent_path(), root / "linked_source");
#endif
    const auto directories = paths.source_directories();
    require(directories.size() == 7 && std::is_sorted(directories.begin(), directories.end()),
        "only immediate real directories are listed, including new source names");

    for (const char* name : {"historic", "taobao", "virtual_data", "custom_feed",
                             "legacy source", "lost+found", "x_trader_data"}) {
        const auto expected = root / name;
        const auto found = std::find(directories.begin(), directories.end(), expected);
        require(found != directories.end(), "selectable directory exists");
        const auto number = std::distance(directories.begin(), found) + 1;
        std::istringstream input(std::to_string(number) + "\n");
        std::ostringstream output;
        std::ostringstream errors;
        require(cli::select_data_source(paths, selected, input, output, errors), "selection succeeds");
        require(selected == expected && errors.str().empty(), "selected root is passed unchanged");
        require(output.str().find("x_trader_data [default]") != std::string::npos,
            "default source is labelled");
        require(output.str().find("[OK] Data source: " + std::string(name)) != std::string::npos,
            "selection feedback names the actual source");
    }
}

void test_invalid_and_cancelled_selection(const fs::path& root) {
    market_data_paths paths(root);
    auto selected = root / "taobao";
    for (const char* command : {"", "\n", "u\n", "up\n", "back\n"}) {
        std::istringstream input(command);
        std::ostringstream output;
        std::ostringstream errors;
        require(!cli::select_data_source(paths, selected, input, output, errors), "cancel or EOF returns");
        require(selected == root / "taobao" && errors.str().empty(), "cancel preserves selection");
    }
    const auto directories = paths.source_directories();
    const auto found = std::find(directories.begin(), directories.end(), root / "historic");
    const auto number = std::distance(directories.begin(), found) + 1;
    std::istringstream input("0\n999\nabc\n-1\n1junk\n/data/historic\n"
                             + std::to_string(number) + "\n");
    std::ostringstream output;
    std::ostringstream errors;
    require(cli::select_data_source(paths, selected, input, output, errors), "invalid choices can be corrected");
    require(selected == root / "historic", "valid numbered choice takes effect");
    require(errors.str().find("[ERROR]") != std::string::npos, "invalid choices are visible");
}

void test_listing_failures(const fs::path& parent) {
    const auto root = parent / "missing";
    market_data_paths paths(root);
    auto selected = paths.source_root();
    const auto original = selected;
    {
        std::istringstream input("1\n");
        std::ostringstream output;
        std::ostringstream errors;
        require(!cli::select_data_source(paths, selected, input, output, errors), "missing root fails");
        require(errors.str().find("[ERROR]") != std::string::npos && selected == original,
            "listing failure does not silently select a different root");
        require(!fs::exists(root), "missing directories are not created");
    }
    fs::create_directory(root);
    {
        std::istringstream input("1\n");
        std::ostringstream output;
        std::ostringstream errors;
        require(!cli::select_data_source(paths, selected, input, output, errors), "empty root has no choices");
        require(output.str().find("[WARN]") != std::string::npos && selected == original,
            "empty listing is visible and preserves the default");
    }
    fs::create_directory(root / "historic");
    {
        std::istringstream input("\n");
        std::ostringstream output;
        std::ostringstream errors;
        require(!cli::select_data_source(paths, selected, input, output, errors), "keep missing default");
        require(selected == original, "available alternatives do not cause automatic fallback");
    }
    {
        std::istringstream input("1\n");
        std::ostringstream output;
        std::ostringstream errors;
        require(cli::select_data_source(paths, selected, input, output, errors), "explicitly select alternative");
        require(selected == root / "historic", "user can switch when the default directory is absent");
    }
}

} // namespace

int main() {
    const auto parent = fs::temp_directory_path();
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path fixture;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = parent / ("xtrader_sources_" + std::to_string(seed) + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) {
            fixture = candidate;
            break;
        }
    }
    if (fixture.empty()) {
        std::cerr << "cannot reserve source-menu test directory\n";
        return 1;
    }
    try {
        test_listing_and_selection(fixture / "data");
        test_invalid_and_cancelled_selection(fixture / "data");
        test_listing_failures(fixture);
        fs::remove_all(fixture);
        std::cout << "CLI data source menu tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(fixture);
        return 1;
    }
}
