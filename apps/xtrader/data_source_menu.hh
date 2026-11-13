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

#include <seastar/xtrader/market_data_paths.hh>

#include <charconv>
#include <cstddef>
#include <istream>
#include <ostream>
#include <system_error>

namespace seastar::xtrader::cli {

// Read-only source selection, testable without initializing a reactor or broker.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
inline bool select_data_source(const market_data_paths& paths,
                               std::filesystem::path& selected_root,
                               std::istream& input, std::ostream& output,
                               std::ostream& errors) {
    std::vector<std::filesystem::path> directories;
    try {
        directories = paths.source_directories();
    } catch (const std::filesystem::filesystem_error& error) {
        errors << "[ERROR] Cannot list data sources: " << error.what() << '\n';
        return false;
    }
    output << "\n[Data Sources]\nRoot: " << paths.root().string()
           << "\nCurrent: " << selected_root.string() << '\n';
    if (directories.empty()) {
        output << "[WARN] No data source directories found; current selection unchanged.\n";
        return false;
    }
    for (std::size_t index = 0; index < directories.size(); ++index) {
        output << "  " << index + 1 << ") " << directories[index].filename().string();
        if (directories[index] == paths.source_root()) {
            output << " [default]";
        }
        if (directories[index] == selected_root) {
            output << " [selected]";
        }
        output << '\n';
    }
    output << "  u) Back\nPress Enter to keep the current source.\n";
    std::string line;
    while (true) {
        output << "Select: " << std::flush;
        if (!std::getline(input, line) || line.empty() ||
            line == "u" || line == "up" || line == "back") {
            return false;
        }
        std::size_t number = 0;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), number);
        if (result.ec != std::errc{} || result.ptr != line.data() + line.size() ||
            number == 0 || number > directories.size()) {
            errors << "[ERROR] Select a listed directory number, or u to go back.\n";
            continue;
        }
        selected_root = directories[number - 1];
        output << "[OK] Data source: " << selected_root.filename().string()
               << "\nData root: " << selected_root.string() << '\n';
        return true;
    }
}

} // namespace seastar::xtrader::cli
