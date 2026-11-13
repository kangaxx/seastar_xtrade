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
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace seastar::xtrader {

// Central file layout without a separate product-family directory.
// Source: user request, 2026-11-13; implement paths only, migrate callers separately.
// Signature: GitHub Copilot (Auto).
struct market_data_key {
    std::string source;
    std::string asset_class;
    std::string exchange;
    std::string instrument;
    std::string timeframe;
};

struct market_data_file {
    market_data_key key;
    std::string calendar_date;
    std::string format;
};

// Path construction and parsing perform no filesystem I/O or directory creation.
class market_data_paths {
public:
    explicit market_data_paths(std::filesystem::path root = "/data",
                               std::vector<std::string> additional_sources = {});

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path source_root(const std::string& source) const;
    [[nodiscard]] std::filesystem::path dataset_directory(const market_data_key& key) const;
    [[nodiscard]] std::filesystem::path data_file(
        const market_data_key& key, const std::string& calendar_date,
        const std::string& format = "csv") const;
    [[nodiscard]] std::filesystem::path manifest_file(const market_data_key& key) const;
    [[nodiscard]] market_data_file parse_data_file(const std::filesystem::path& path) const;

    // Explicit blocking check for startup/worker use; not a race-free file-open API.
    // Rejects existing symlink redirection below root, including between sources.
    void validate_filesystem_path(const std::filesystem::path& path) const;

private:
    [[nodiscard]] std::vector<std::string> relative_components(
        const std::filesystem::path& path) const;

    std::filesystem::path _root;
    std::set<std::string> _sources;
};

} // namespace seastar::xtrader
