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
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace seastar::xtrader {

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

    return true;
}

std::vector<dataset_manifest> historical_data_manager::scan_datasets(std::string* warning) const {
    std::vector<dataset_manifest> datasets;

    std::error_code ec;
    if (!std::filesystem::exists(_data_root, ec) || !std::filesystem::is_directory(_data_root, ec)) {
        if (warning != nullptr) {
            *warning = "Historical data root not found: " + _data_root.generic_string();
        }
        return datasets;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(_data_root, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_regular_file() || entry.path().filename() != "dataset-manifest.conf") {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> kv;
        if (!load_key_value_file(entry.path(), kv)) {
            continue;
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

    if (warning != nullptr) {
        *warning = "Dataset not found under data root: " + _data_root.generic_string();
    }
    return std::nullopt;
}

bool historical_data_manager::load_bars(
    const dataset_manifest& dataset,
    std::vector<historical_bar>& bars,
    std::string* warning) const {

    bars.clear();

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
    while (std::getline(file, line)) {
        if (!header_skipped) {
            header_skipped = true;
            if (line.find("datetime") != std::string::npos || line.find("date") != std::string::npos) {
                continue;
            }
        }

        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> cols;
        while (std::getline(ss, cell, ',')) {
            cols.push_back(trim(cell));
        }
        if (cols.size() < 6) {
            continue;
        }

        try {
            historical_bar bar;
            bar.datetime = cols[0];
            bar.open = std::stod(cols[1]);
            bar.high = std::stod(cols[2]);
            bar.low = std::stod(cols[3]);
            bar.close = std::stod(cols[4]);
            bar.volume = std::stod(cols[5]);
            if (cols.size() > 6) {
                bar.open_interest = std::stod(cols[6]);
            }
            bars.push_back(std::move(bar));
        } catch (const std::exception&) {
            continue;
        }
    }

    if (bars.empty() && warning != nullptr) {
        *warning = "Dataset file exists but no valid rows were loaded: " + dataset.active_file_path.generic_string();
    }

    return true;
}

} // namespace seastar::xtrader
