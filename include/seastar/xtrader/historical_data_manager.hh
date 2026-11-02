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
#pragma once

#include <seastar/core/sstring.hh>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seastar::xtrader {

struct historical_bar {
    std::string datetime;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    double open_interest = 0.0;
};

enum class data_domain {
    raw,
    dominant,
    live_capture,
};

inline const char* to_string(data_domain d) {
    switch (d) {
        case data_domain::raw:          return "raw";
        case data_domain::dominant:     return "dominant";
        case data_domain::live_capture: return "live_capture";
        default: return "unknown";
    }
}

inline data_domain parse_data_domain(const std::string& s) {
    if (s == "dominant")     return data_domain::dominant;
    if (s == "live_capture") return data_domain::live_capture;
    return data_domain::raw;
}

struct dataset_manifest {
    std::filesystem::path manifest_path;
    std::filesystem::path dataset_dir;
    std::string symbol;
    std::string exchange;
    std::string timeframe;
    std::string source;
    data_domain domain = data_domain::raw;
    std::string active_file;
    std::filesystem::path active_file_path;
    std::string start_datetime;
    std::string end_datetime;
    size_t row_count = 0;
    std::string manifest_hash;
    std::string last_import_mode;
    std::string last_import_time;
    std::string status = "unknown";
};

class historical_data_manager {
public:
    explicit historical_data_manager(std::filesystem::path data_root = "/data/x_trader_data");

    [[nodiscard]] const std::filesystem::path& data_root() const noexcept;

    [[nodiscard]] std::vector<dataset_manifest> scan_datasets(std::string* warning = nullptr) const;

    [[nodiscard]] std::optional<dataset_manifest> find_dataset(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& timeframe,
        std::string* warning = nullptr) const;

    [[nodiscard]] bool load_bars(
        const dataset_manifest& dataset,
        std::vector<historical_bar>& bars,
        std::string* warning = nullptr) const;

private:
    static bool load_key_value_file(
        const std::filesystem::path& file,
        std::vector<std::pair<std::string, std::string>>& values);
    static std::string trim(std::string value);

private:
    std::filesystem::path _data_root;
};

} // namespace seastar::xtrader
