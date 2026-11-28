/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <seastar/xtrader/historical_data_manager.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace seastar::xtrader {
namespace {

double parse_number(const std::string& value, historical_load_mode mode) {
    size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (mode == historical_load_mode::strict &&
        (consumed != value.size() || !std::isfinite(result))) {
        throw std::invalid_argument("invalid numeric value");
    }
    return result;
}

bool parse_bar_row(const std::vector<std::string>& cols, historical_load_mode mode,
                   historical_bar& bar) {
    if (cols.size() < 6 || (mode == historical_load_mode::strict && cols.size() < 7)) {
        return false;
    }
    try {
        bar.datetime = cols[0];
        bar.open = parse_number(cols[1], mode);
        bar.high = parse_number(cols[2], mode);
        bar.low = parse_number(cols[3], mode);
        bar.close = parse_number(cols[4], mode);
        bar.volume = parse_number(cols[5], mode);
        if (cols.size() > 6) {
            bar.open_interest = parse_number(cols[6], mode);
        }
        return mode != historical_load_mode::strict ||
            (bar.volume >= 0.0 && bar.open_interest >= 0.0 && bar.high >= bar.low &&
             bar.open >= bar.low && bar.open <= bar.high &&
             bar.close >= bar.low && bar.close <= bar.high);
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::out_of_range&) {
        return false;
    }
}

bool valid_bar_header(const std::vector<std::string>& cols) {
    if (cols.size() < 7) {
        return false;
    }
    constexpr std::array<const char*, 6> expected{
        "open", "high", "low", "close", "volume", "open_interest"
    };
    auto lower = [] (std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [] (unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    };
    auto first = lower(cols.front());
    if (first.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        first.erase(0, 3);
    }
    if (first != "date" && first != "datetime" && first != "timestamp") {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (lower(cols[index + 1]) != expected[index]) {
            return false;
        }
    }
    return true;
}

market_data_key dataset_key(const dataset_manifest& dataset) {
    return {dataset.source, dataset.asset_class, dataset.exchange, dataset.symbol, dataset.timeframe};
}

bool same_key(const market_data_key& left, const market_data_key& right) {
    return left.source == right.source && left.asset_class == right.asset_class
        && left.exchange == right.exchange && left.instrument == right.instrument
        && left.timeframe == right.timeframe;
}

struct daily_csv_row {
    market_csv_row fields;
    std::filesystem::path file;
    std::size_t line;
};

// 从 "YYYY-MM-DD HH:MM:SS" 提取 "YYYYMMDD"；无法解析返回空串。
std::string calendar_date_of(const std::string& datetime) {
    if (datetime.size() < 10 || datetime[4] != '-' || datetime[7] != '-') {
        return {};
    }
    std::string date = datetime.substr(0, 10);
    date.erase(7, 1);
    date.erase(4, 1);
    return date;
}

std::vector<daily_csv_row> read_daily_csv(const std::filesystem::path& root,
        const dataset_manifest& dataset, market_data_kind kind,
        const std::string& start_date, const std::string& end_date) {
    if (dataset.layout_version != 2 || dataset.kind != kind
            || data_kind_for_timeframe(dataset.timeframe) != kind || dataset.files.empty()) {
        throw std::invalid_argument("dataset does not have the requested daily Tick/bar schema");
    }
    std::vector<daily_csv_row> rows;
    auto files = dataset.files;
    std::sort(files.begin(), files.end());
    if (std::adjacent_find(files.begin(), files.end()) != files.end()) {
        throw std::invalid_argument("duplicate daily file in dataset");
    }
    for (const auto& path : files) {
        market_data_paths::validate_contained_path(root, path);
        const auto paths = market_data_paths::for_data_file(path, root);
        const auto file = paths.parse_data_file(path);
        paths.validate_filesystem_path(path);
        if (!same_key(file.key, dataset_key(dataset)) || file.format != "csv") {
            throw std::invalid_argument("daily file identity differs from selected dataset");
        }
        if ((!start_date.empty() && file.calendar_date < start_date)
                || (!end_date.empty() && file.calendar_date > end_date)) {
            continue;
        }
        std::ifstream input(path);
        if (!input.is_open()) { throw std::runtime_error("cannot open daily CSV: " + path.string()); }
        std::string line;
        if (!std::getline(input, line) || line + '\n' != market_csv_header(kind)) {
            throw std::runtime_error("invalid daily CSV schema: " + path.string());
        }
        std::size_t line_number = 1;
        while (std::getline(input, line)) {
            ++line_number;
            if (input.eof()) { throw std::runtime_error("incomplete final CSV row: " + path.string()); }
            try {
                auto row = parse_market_csv_row(line);
                if (validate_market_csv_row(file.key, row) != file.calendar_date) {
                    throw std::invalid_argument("row date differs from daily filename");
                }
                rows.push_back({std::move(row), path, line_number});
            } catch (const std::exception& error) {
                throw std::runtime_error(path.string() + " at line " + std::to_string(line_number)
                    + ": " + error.what());
            }
        }
        if (input.bad()) { throw std::runtime_error("error reading daily CSV: " + path.string()); }
    }
    return rows;
}

} // namespace

historical_data_manager::historical_data_manager(std::filesystem::path data_root)
    : _data_root(std::filesystem::absolute(data_root)) {
    if (_data_root.filename().empty() && _data_root != _data_root.root_path()) {
        _data_root = _data_root.parent_path();
    }
}

const std::filesystem::path& historical_data_manager::data_root() const noexcept {
    return _data_root;
}

std::string historical_data_manager::trim(std::string value) {
    auto not_space = [] (unsigned char ch) {
        return std::isspace(ch) == 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool historical_data_manager::load_key_value_file(
    const std::filesystem::path& file,
    std::vector<std::pair<std::string, std::string>>& values) {

    std::ifstream input(file);
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#') {
            continue;
        }

        const auto pos = stripped.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        auto key = trim(stripped.substr(0, pos));
        auto value = trim(stripped.substr(pos + 1));
        if (!key.empty()) {
            values.emplace_back(std::move(key), std::move(value));
        }
    }

    return !input.bad();
}

std::vector<dataset_manifest> historical_data_manager::scan_datasets(std::string* warning) const {
    std::vector<dataset_manifest> datasets;
    if (warning != nullptr) {
        warning->clear();
    }

    std::error_code ec;
    if (!std::filesystem::exists(_data_root, ec) || !std::filesystem::is_directory(_data_root, ec)) {
        if (warning != nullptr) {
            *warning = "Cannot inspect historical data root: " + _data_root.generic_string();
            if (ec) {
                *warning += ": " + ec.message();
            }
        }
        return datasets;
    }

    std::map<std::filesystem::path, dataset_manifest> daily;
    std::vector<std::filesystem::path> candidates;
    std::filesystem::recursive_directory_iterator it(_data_root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        const auto status = entry.symlink_status(ec);
        const bool is_regular_file = std::filesystem::is_regular_file(status);
        if (ec) {
            break;
        }

        if (std::filesystem::is_symlink(status)) {
            it.disable_recursion_pending();
            if (warning != nullptr) { *warning = "Market data scan refuses a symlink: " + entry.path().string(); }
            return {};
        }
        const auto name = entry.path().filename().string();
        if (is_regular_file && name.size() == 12 && name.substr(8) == ".csv"
                && std::all_of(name.begin(), name.begin() + 8,
                    [] (char ch) { return ch >= '0' && ch <= '9'; })) {
            candidates.push_back(entry.path());
            it.increment(ec);
            continue;
        }
        if (!is_regular_file || name != "dataset-manifest.conf") {
            it.increment(ec);
            continue;
        }

        std::vector<std::pair<std::string, std::string>> kv;
        if (!load_key_value_file(entry.path(), kv)) {
            if (warning != nullptr) {
                *warning = "Cannot read dataset manifest: " + entry.path().generic_string();
            }
            return datasets;
        }

        std::unordered_map<std::string, std::string> values;
        for (auto& [k, v] : kv) {
            values[k] = v;
        }

        dataset_manifest m;
        m.manifest_path = entry.path();
        m.dataset_dir = entry.path().parent_path();
        m.symbol = values.count("symbol") > 0 ? values["symbol"] : "";
        m.exchange = values.count("exchange") > 0 ? values["exchange"] : "";
        m.timeframe = values.count("timeframe") > 0 ? values["timeframe"] : "";
        m.source = values.count("source") > 0 ? values["source"] : "";
        m.asset_class = values.count("asset_class") > 0 ? values["asset_class"] : "";
        m.kind = m.timeframe == "tick" ? market_data_kind::tick : market_data_kind::bar;
        m.active_file = values.count("active_file") > 0 ? values["active_file"] : "";

        if (!m.active_file.empty()) {
            std::filesystem::path active_path(m.active_file);
            if (active_path.is_relative()) {
                active_path = m.dataset_dir / active_path;
            }
            try {
                market_data_paths::validate_contained_path(_data_root, entry.path());
                market_data_paths::validate_contained_path(m.dataset_dir, active_path);
                m.active_file_path = active_path;
            } catch (const std::exception& error) {
                if (warning != nullptr) { *warning = "Invalid legacy manifest path: " + std::string(error.what()); }
                return {};
            }
        }
        m.status = "legacy-manifest";
        datasets.push_back(std::move(m));
        it.increment(ec);
    }

    if (ec && warning != nullptr) {
        *warning = "Cannot scan historical data root: " + _data_root.generic_string()
            + ": " + ec.message();
    }
    if (ec) { return {}; }
    for (const auto& path : candidates) {
        if (std::any_of(datasets.begin(), datasets.end(),
                [&path] (const auto& legacy) { return legacy.active_file_path == path; })) {
            continue;
        }
        try {
            market_data_paths::validate_contained_path(_data_root, path);
            const auto paths = market_data_paths::for_data_file(path, _data_root);
            const auto file = paths.parse_data_file(path);
            paths.validate_filesystem_path(path);
            auto& dataset = daily[path.parent_path()];
            dataset.dataset_dir = path.parent_path();
            dataset.symbol = file.key.instrument;
            dataset.exchange = file.key.exchange;
            dataset.timeframe = file.key.timeframe;
            dataset.source = file.key.source;
            dataset.asset_class = file.key.asset_class;
            dataset.kind = data_kind_for_timeframe(file.key.timeframe);
            dataset.layout_version = 2;
            dataset.files.push_back(path);
            dataset.status = "daily-csv";
        } catch (const std::exception& error) {
            if (warning != nullptr) { *warning = "Invalid daily dataset: " + std::string(error.what()); }
            return {};
        }
    }
    for (auto& [directory, dataset] : daily) {
        if (std::any_of(datasets.begin(), datasets.end(),
                [&directory] (const auto& legacy) { return legacy.dataset_dir == directory; })) {
            if (warning != nullptr) { *warning = "Daily files and legacy manifest coexist: " + directory.string(); }
            return {};
        }
        std::sort(dataset.files.begin(), dataset.files.end());
        dataset.active_file_path = dataset.files.front();
        dataset.active_file = dataset.active_file_path.filename().string();
        datasets.push_back(std::move(dataset));
    }
    std::sort(datasets.begin(), datasets.end(), [] (const auto& left, const auto& right) {
        return left.dataset_dir < right.dataset_dir;
    });
    return datasets;
}

std::optional<dataset_manifest> historical_data_manager::find_dataset(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& timeframe,
    std::string* warning) const {

    const auto datasets = scan_datasets(warning);
    if (warning != nullptr && !warning->empty()) { return std::nullopt; }
    const dataset_manifest* selected = nullptr;
    for (const auto& ds : datasets) {
        if (ds.symbol == symbol && ds.exchange == exchange && ds.timeframe == timeframe) {
            if (selected) {
                if (warning != nullptr) { *warning = "Ambiguous dataset; select one source root explicitly"; }
                return std::nullopt;
            }
            selected = &ds;
        }
    }
    if (selected) { return *selected; }

    if (warning != nullptr && warning->empty()) {
        *warning = "Dataset not found under data root: " + _data_root.generic_string();
    }
    return std::nullopt;
}

bool historical_data_manager::load_bars(
    const dataset_manifest& dataset,
    std::vector<historical_bar>& bars,
    std::string* warning,
    historical_load_mode mode,
    const std::string& start_date,
    const std::string& end_date) const {

    bars.clear();
    if (warning != nullptr) {
        warning->clear();
    }

    if (mode != historical_load_mode::permissive && mode != historical_load_mode::strict) {
        if (warning != nullptr) {
            *warning = "Invalid historical data load mode";
        }
        return false;
    }
    if (dataset.kind != market_data_kind::bar || dataset.timeframe == "tick") {
        if (warning != nullptr) { *warning = "Tick dataset requires load_ticks(), not the bar loader"; }
        return false;
    }
    if (dataset.layout_version == 2) {
        try {
            const auto rows = read_daily_csv(_data_root, dataset, market_data_kind::bar,
                                             start_date, end_date);
            for (const auto& row : rows) {
                historical_bar bar;
                if (!parse_bar_row(row.fields, historical_load_mode::strict, bar)) {
                    throw std::runtime_error("cannot decode validated daily bar");
                }
                bar.source_line = row.line;
                bar.source_file = row.file;
                bars.push_back(std::move(bar));
            }
            if (bars.empty()) { throw std::runtime_error("daily dataset contains no data rows"); }
            return true;
        } catch (const std::exception& error) {
            bars.clear();
            if (warning != nullptr) { *warning = error.what(); }
            return false;
        }
    }

    if (dataset.active_file_path.empty()) {
        if (warning != nullptr) {
            *warning = "Dataset has no active_file configured: " + dataset.manifest_path.generic_string();
        }
        return false;
    }

    try {
        market_data_paths::validate_contained_path(_data_root, dataset.active_file_path);
        if (!dataset.dataset_dir.empty()) {
            market_data_paths::validate_contained_path(dataset.dataset_dir, dataset.active_file_path);
        }
    } catch (const std::exception& error) {
        if (warning != nullptr) { *warning = error.what(); }
        return false;
    }
    std::ifstream file(dataset.active_file_path);
    if (!file.is_open()) {
        if (warning != nullptr) {
            *warning = "Active data file missing: " + dataset.active_file_path.generic_string();
        }
        return false;
    }

    std::string line;
    bool header_skipped = false;
    size_t expected_columns = 0;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (trim(line).empty()) {
            continue;
        }
        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> cols;
        while (std::getline(ss, cell, ',')) {
            cols.push_back(trim(cell));
        }
        if (!line.empty() && line.back() == ',') {
            cols.emplace_back();
        }
        if (!header_skipped) {
            header_skipped = true;
            if (mode == historical_load_mode::strict) {
                if (!valid_bar_header(cols)) {
                    if (warning != nullptr) {
                        *warning = "Invalid dataset header in " + dataset.active_file_path.generic_string()
                            + " at line " + std::to_string(line_number);
                    }
                    return false;
                }
                expected_columns = cols.size();
                continue;
            }
            if (line.find("datetime") != std::string::npos || line.find("date") != std::string::npos) {
                continue;
            }
        }

        historical_bar bar;
        if ((mode == historical_load_mode::strict && cols.size() != expected_columns) ||
            !parse_bar_row(cols, mode, bar)) {
            if (mode == historical_load_mode::strict) {
                bars.clear();
                if (warning != nullptr) {
                    *warning = "Invalid dataset row in " + dataset.active_file_path.generic_string()
                        + " at line " + std::to_string(line_number);
                }
                return false;
            }
            continue;
        }
        if (!start_date.empty() || !end_date.empty()) {
            const auto date = calendar_date_of(bar.datetime);
            if (date.empty()) {
                if (mode == historical_load_mode::strict) {
                    bars.clear();
                    if (warning != nullptr) {
                        *warning = "Invalid datetime in " + dataset.active_file_path.generic_string()
                            + " at line " + std::to_string(line_number);
                    }
                    return false;
                }
                continue;
            }
            if ((!start_date.empty() && date < start_date) || (!end_date.empty() && date > end_date)) {
                continue;
            }
        }
        bar.source_line = line_number;
        bar.source_file = dataset.active_file_path;
        bars.push_back(std::move(bar));
    }

    if (file.bad()) {
        bars.clear();
        if (warning != nullptr) {
            *warning = "Error reading dataset: " + dataset.active_file_path.generic_string();
        }
        return false;
    }

    if (bars.empty()) {
        if (warning != nullptr) {
            *warning = "Dataset file exists but no valid rows were loaded: " + dataset.active_file_path.generic_string();
        }
        return mode != historical_load_mode::strict;
    }

    return true;
}

bool historical_data_manager::load_ticks(const dataset_manifest& dataset,
        std::vector<market_csv_row>& ticks, std::string* warning) const {
    ticks.clear();
    if (warning != nullptr) { warning->clear(); }
    try {
        auto rows = read_daily_csv(_data_root, dataset, market_data_kind::tick, "", "");
        for (auto& row : rows) { ticks.push_back(std::move(row.fields)); }
        if (ticks.empty()) { throw std::runtime_error("daily Tick dataset contains no rows"); }
        return true;
    } catch (const std::exception& error) {
        ticks.clear();
        if (warning != nullptr) { *warning = error.what(); }
        return false;
    }
}

} // namespace seastar::xtrader
