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

#include "data_source_menu.hh"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader::cli {

// Menu selections only: no strategy instance, matching engine or trading channel.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
struct backtest_selection {
    std::filesystem::path data_root;
    std::filesystem::path product_directory;
    std::vector<std::filesystem::path> data_files;
    std::string strategy_id;
};

inline std::string backtest_placeholder_result() {
    return "施工中";
}

namespace backtest_menu_detail {

inline std::vector<std::size_t> parse_numbers(const std::string& line,
                                            std::size_t count, bool multiple) {
    std::vector<std::size_t> numbers;
    auto begin = line.find_first_not_of(" ,\t\r");
    while (begin != std::string::npos) {
        const auto end = line.find_first_of(" ,\t\r", begin);
        const auto length = end == std::string::npos ? line.size() - begin : end - begin;
        std::size_t value = 0;
        const auto result = std::from_chars(line.data() + begin, line.data() + begin + length, value);
        if (result.ec != std::errc{} || result.ptr != line.data() + begin + length ||
            value == 0 || value > count ||
            std::find(numbers.begin(), numbers.end(), value - 1) != numbers.end()) {
            throw std::invalid_argument("select distinct listed numbers");
        }
        numbers.push_back(value - 1);
        begin = end == std::string::npos ? end : line.find_first_not_of(" ,\t\r", end);
    }
    if (numbers.empty() || (!multiple && numbers.size() != 1)) {
        throw std::invalid_argument(multiple ? "select at least one file" : "select exactly one item");
    }
    return numbers;
}

inline std::optional<std::vector<std::size_t>> choose(
        const std::string& title, const std::vector<std::string>& labels, bool multiple,
        std::istream& input, std::ostream& output, std::ostream& errors) {
    output << '\n' << title << '\n';
    if (labels.empty()) {
        output << "[WARN] No matching data files found.\n";
        return std::nullopt;
    }
    for (std::size_t index = 0; index < labels.size(); ++index) {
        output << "  " << index + 1 << ") " << labels[index] << '\n';
    }
    output << "  u) Back (Enter keeps the current selection)\n";
    if (multiple) {
        output << "Select one or more file numbers, separated by spaces or commas.\n";
    }
    std::string line;
    while (true) {
        output << "Select: " << std::flush;
        if (!std::getline(input, line) || line.empty() || line == "u" || line == "back") {
            return std::nullopt;
        }
        try {
            return parse_numbers(line, labels.size(), multiple);
        } catch (const std::invalid_argument& error) {
            errors << "[ERROR] " << error.what() << '\n';
        }
    }
}

inline void require_source(const market_data_paths& paths, const backtest_selection& selection) {
    const auto sources = paths.source_directories();
    if (std::find(sources.begin(), sources.end(), selection.data_root) == sources.end()) {
        throw std::invalid_argument("selected data source is missing or is not a direct real directory");
    }
}

inline bool is_data_file(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [] (unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".csv" || extension == ".jsonl" ||
        extension == ".parquet" || extension == ".zip";
}

inline std::vector<std::filesystem::path> data_files_in(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(directory))) {
        throw std::invalid_argument("selected data directory is missing or is a link");
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (std::filesystem::is_regular_file(entry.symlink_status()) && is_data_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline void select_product(const market_data_paths& paths, backtest_selection& selection,
                           std::istream& input, std::ostream& output, std::ostream& errors) {
    require_source(paths, selection);
    std::vector<std::filesystem::path> directories;
    for (const auto& entry : std::filesystem::directory_iterator(selection.data_root)) {
        if (std::filesystem::is_directory(entry.symlink_status())) {
            directories.push_back(entry.path());
        }
    }
    std::sort(directories.begin(), directories.end());
    std::vector<std::string> labels{"All product types (no directory filter)"};
    for (const auto& directory : directories) {
        labels.push_back(directory.filename().string());
    }
    const auto choice = choose("[Product Types / Directories]", labels, false, input, output, errors);
    if (!choice) {
        return;
    }
    const auto index = choice->front();
    const auto directory = index == 0 ? std::filesystem::path{} : directories[index - 1];
    auto files = data_files_in(directory.empty() ? selection.data_root : directory);
    selection.product_directory = directory;
    selection.data_files = std::move(files);
    output << "[OK] Product selection includes " << selection.data_files.size() << " data file(s).\n";
}

inline void select_files(const market_data_paths& paths, backtest_selection& selection,
                         std::istream& input, std::ostream& output, std::ostream& errors) {
    require_source(paths, selection);
    const auto& directory = selection.product_directory.empty()
        ? selection.data_root : selection.product_directory;
    const auto files = data_files_in(directory);
    std::vector<std::string> labels;
    for (const auto& file : files) {
        labels.push_back(file.lexically_relative(selection.data_root).generic_string());
    }
    const auto choices = choose("[Data Files]", labels, true, input, output, errors);
    if (!choices) {
        return;
    }
    std::vector<std::filesystem::path> selected;
    for (const auto index : *choices) {
        selected.push_back(files[index]);
    }
    selection.data_files = std::move(selected);
    output << "[OK] Selected " << selection.data_files.size() << " data file(s).\n";
}

inline void select_strategy(backtest_selection& selection, std::istream& input,
                            std::ostream& output, std::ostream& errors) {
    if (choose("[Strategies]", {"Virtual (placeholder; no trading logic)"}, false,
               input, output, errors)) {
        selection.strategy_id = "Virtual";
    }
}

inline void require_ready(const market_data_paths& paths, const backtest_selection& selection) {
    require_source(paths, selection);
    if (selection.data_files.empty() || selection.strategy_id != "Virtual") {
        throw std::invalid_argument("select data files and the Virtual strategy before Run");
    }
    for (const auto& file : selection.data_files) {
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file))) {
            throw std::invalid_argument("a selected data file is missing or is a link");
        }
    }
}

} // namespace backtest_menu_detail

inline void run_backtest_menu(const market_data_paths& paths, std::istream& input,
                             std::ostream& output, std::ostream& errors,
                             const std::function<std::string(const backtest_selection&)>& run) {
    if (!run) {
        throw std::invalid_argument("backtest menu requires an explicit Run handler");
    }
    backtest_selection selection{paths.source_root(), {}, {}, {}};
    std::string line;
    while (true) {
        output << "\n[Backtest Setup - execution under construction]\nData root: "
               << selection.data_root.string() << "\nProduct type: "
               << (selection.product_directory.empty() ? "(all)" : selection.product_directory.filename().string())
               << "\nFiles: " << selection.data_files.size()
               << "\nStrategy: " << (selection.strategy_id.empty() ? "(not selected)" : selection.strategy_id)
               << "\n1) Select data source\n2) Select product type\n3) Select data files\n"
                  "4) Select strategy\n5) Run (under construction)\nu) Back\nSelect: " << std::flush;
        if (!std::getline(input, line) || line == "u" || line == "back") {
            return;
        }
        try {
            if (line == "1") {
                const auto previous = selection.data_root;
                select_data_source(paths, selection.data_root, input, output, errors);
                if (previous != selection.data_root) {
                    selection.product_directory.clear();
                    selection.data_files.clear();
                }
            } else if (line == "2") {
                backtest_menu_detail::select_product(paths, selection, input, output, errors);
            } else if (line == "3") {
                backtest_menu_detail::select_files(paths, selection, input, output, errors);
            } else if (line == "4") {
                backtest_menu_detail::select_strategy(selection, input, output, errors);
            } else if (line == "5") {
                backtest_menu_detail::require_ready(paths, selection);
                output << "[Backtest Run]\n" << run(selection) << '\n';
            } else {
                errors << "[ERROR] Unknown backtest menu option.\n";
            }
        } catch (const std::filesystem::filesystem_error& error) {
            errors << "[ERROR] Cannot access backtest data: " << error.what() << '\n';
        } catch (const std::invalid_argument& error) {
            errors << "[ERROR] " << error.what() << '\n';
        }
    }
}

} // namespace seastar::xtrader::cli
