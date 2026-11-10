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

} // namespace

historical_data_manager::historical_data_manager(std::filesystem::path data_root)
    : _data_root(std::move(data_root)) {
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

    std::filesystem::recursive_directory_iterator it(_data_root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        const bool is_regular_file = entry.is_regular_file(ec);
        if (ec) {
            break;
        }

        if (!is_regular_file || entry.path().filename() != "dataset-manifest.conf") {
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
        m.active_file = values.count("active_file") > 0 ? values["active_file"] : "";

        if (!m.active_file.empty()) {
            std::filesystem::path active_path(m.active_file);
            if (active_path.is_relative()) {
                active_path = m.dataset_dir / active_path;
            }
            m.active_file_path = active_path.lexically_normal();
        }

        datasets.push_back(std::move(m));
        it.increment(ec);
    }

    if (ec && warning != nullptr) {
        *warning = "Cannot scan historical data root: " + _data_root.generic_string()
            + ": " + ec.message();
    }

    return datasets;
}

std::optional<dataset_manifest> historical_data_manager::find_dataset(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& timeframe,
    std::string* warning) const {

    const auto datasets = scan_datasets(warning);
    for (const auto& ds : datasets) {
        if (ds.symbol == symbol && ds.exchange == exchange && ds.timeframe == timeframe) {
            return ds;
        }
    }

    if (warning != nullptr && warning->empty()) {
        *warning = "Dataset not found under data root: " + _data_root.generic_string();
    }
    return std::nullopt;
}

bool historical_data_manager::load_bars(
    const dataset_manifest& dataset,
    std::vector<historical_bar>& bars,
    std::string* warning,
    historical_load_mode mode) const {

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

    if (dataset.active_file_path.empty()) {
        if (warning != nullptr) {
            *warning = "Dataset has no active_file configured: " + dataset.manifest_path.generic_string();
        }
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
        bar.source_line = line_number;
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

} // namespace seastar::xtrader
