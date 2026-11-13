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
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#include <seastar/xtrader/market_data_paths.hh>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace seastar::xtrader {
namespace {

bool is_ascii_letter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool is_ascii_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

void validate_component(const std::string& value, const char* field) {
    if (value.empty() || (!is_ascii_letter(value.front()) && !is_ascii_digit(value.front())) ||
        !std::all_of(value.begin(), value.end(), [] (char ch) {
            return is_ascii_letter(ch) || is_ascii_digit(ch) || ch == '_' || ch == '-';
        })) {
        throw std::invalid_argument(std::string("invalid market data ") + field);
    }
}

void validate_date(const std::string& date) {
    if (date.size() != 8 || !std::all_of(date.begin(), date.end(), is_ascii_digit)) {
        throw std::invalid_argument("market data date must be YYYYMMDD");
    }
    const auto year = std::chrono::year{std::stoi(date.substr(0, 4))};
    const auto month = std::chrono::month{static_cast<unsigned>(std::stoi(date.substr(4, 2)))};
    const auto day = std::chrono::day{static_cast<unsigned>(std::stoi(date.substr(6, 2)))};
    if (static_cast<int>(year) == 0 || !std::chrono::year_month_day{year, month, day}.ok()) {
        throw std::invalid_argument("invalid market data calendar date");
    }
}

void validate_format(const std::string& format) {
    if (format.empty() || !std::all_of(format.begin(), format.end(), [] (char ch) {
            return (ch >= 'a' && ch <= 'z') || is_ascii_digit(ch);
        })) {
        throw std::invalid_argument("market data format must be a lowercase extension without a dot");
    }
}

void validate_absolute_path(const std::filesystem::path& path) {
    if (!path.is_absolute()) {
        throw std::invalid_argument("market data path must be absolute");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            throw std::invalid_argument("market data path cannot contain dot components");
        }
    }
}

} // namespace

// User-directed shared path rules, 2026-11-13. Signature: GitHub Copilot (Auto).
market_data_paths::market_data_paths(std::filesystem::path root,
                                   std::vector<std::string> additional_sources)
    : _root(std::move(root)),
      _sources{"x_trader_data", "historic", "taobao", "virtual_data"} {
    validate_absolute_path(_root);
    _root = _root.lexically_normal();
    if (_root.filename().empty()) {
        _root = _root.parent_path();
    }
    if (_root == _root.root_path()) {
        throw std::invalid_argument("market data root cannot be a filesystem root");
    }
    for (const auto& source : additional_sources) {
        validate_component(source, "source");
        if (!_sources.insert(source).second) {
            throw std::invalid_argument("duplicate market data source");
        }
    }
}

const std::filesystem::path& market_data_paths::root() const noexcept {
    return _root;
}

std::filesystem::path market_data_paths::source_root(const std::string& source) const {
    validate_component(source, "source");
    if (_sources.find(source) == _sources.end()) {
        throw std::invalid_argument("unregistered market data source");
    }
    return _root / source;
}

std::filesystem::path market_data_paths::dataset_directory(const market_data_key& key) const {
    const auto source = source_root(key.source);
    validate_component(key.asset_class, "asset class");
    validate_component(key.exchange, "exchange");
    validate_component(key.instrument, "instrument");
    validate_component(key.timeframe, "timeframe");
    return source / key.asset_class / key.exchange / key.instrument / key.timeframe;
}

std::filesystem::path market_data_paths::data_file(
        const market_data_key& key, const std::string& calendar_date,
        const std::string& format) const {
    validate_date(calendar_date);
    validate_format(format);
    return dataset_directory(key) / (calendar_date + "." + format);
}

std::filesystem::path market_data_paths::manifest_file(const market_data_key& key) const {
    return dataset_directory(key) / "dataset-manifest.conf";
}

std::vector<std::string> market_data_paths::relative_components(
        const std::filesystem::path& path) const {
    validate_absolute_path(path);
    const auto relative = path.lexically_normal().lexically_relative(_root);
    std::vector<std::string> components;
    for (const auto& part : relative) {
        components.push_back(part.string());
    }
    if (components.size() != 6) {
        throw std::invalid_argument("market data file must have five directory levels below root");
    }
    const market_data_key key{components[0], components[1], components[2],
                              components[3], components[4]};
    (void)dataset_directory(key);
    return components;
}

market_data_file market_data_paths::parse_data_file(const std::filesystem::path& path) const {
    const auto parts = relative_components(path);
    const auto& filename = parts[5];
    const auto dot = filename.find('.');
    if (dot == std::string::npos) {
        throw std::invalid_argument("market data file must have a date and format");
    }
    market_data_file file{
        {parts[0], parts[1], parts[2], parts[3], parts[4]},
        filename.substr(0, dot), filename.substr(dot + 1)};
    validate_date(file.calendar_date);
    validate_format(file.format);
    return file;
}

void market_data_paths::validate_filesystem_path(const std::filesystem::path& path) const {
    const auto parts = relative_components(path);
    if (parts[5] != "dataset-manifest.conf") {
        (void)parse_data_file(path);
    }
    auto prefix = _root;
    for (const auto& part : parts) {
        prefix /= part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("cannot inspect market data path", prefix, error);
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::invalid_argument("market data path contains a symlink below root");
        }
    }
    const auto expected = std::filesystem::weakly_canonical(_root)
        / path.lexically_normal().lexically_relative(_root);
    const auto resolved = std::filesystem::weakly_canonical(path);
    if (resolved != expected) {
        throw std::invalid_argument("market data path redirects below the configured root");
    }
}

} // namespace seastar::xtrader
