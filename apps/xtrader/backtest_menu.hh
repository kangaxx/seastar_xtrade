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

#include <seastar/xtrader/strategy_registry.hh>

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

// 从约定的 JSON 字符串里提取 "key":"value" 形式的字符串字段。
inline std::string json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const auto value_begin = pos + needle.size();
    const auto value_end = json.find('"', value_begin);
    if (value_end == std::string::npos) {
        return {};
    }
    return json.substr(value_begin, value_end - value_begin);
}

// 从约定的 JSON 字符串里提取 "key":true/false 形式的布尔字段。
inline bool json_bool_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    return json.compare(pos + needle.size(), 4, "true") == 0;
}

inline std::string backtest_placeholder_result() {
    return ui_text("Under construction", "施工中");
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
            throw std::invalid_argument(ui_text("select distinct listed numbers", "请选择列表中不重复的编号"));
        }
        numbers.push_back(value - 1);
        begin = end == std::string::npos ? end : line.find_first_not_of(" ,\t\r", end);
    }
    if (numbers.empty() || (!multiple && numbers.size() != 1)) {
        throw std::invalid_argument(multiple
            ? ui_text("select at least one file", "请至少选择一个文件")
            : ui_text("select exactly one item", "请只选择一项"));
    }
    return numbers;
}

inline std::optional<std::vector<std::size_t>> choose(
        const std::string& title, const std::vector<std::string>& labels, bool multiple,
        std::istream& input, std::ostream& output, std::ostream& errors) {
    output << '\n' << title << '\n';
    if (labels.empty()) {
        output << ui_text("[WARN] No matching data files found.\n", "[WARN] 未找到匹配的数据文件。\n");
        return std::nullopt;
    }
    for (std::size_t index = 0; index < labels.size(); ++index) {
        output << "  " << index + 1 << ") " << labels[index] << '\n';
    }
    output << ui_text("  u) Back (Enter keeps the current selection)\n", "  u) 返回（回车保持当前选择）\n");
    if (multiple) {
        output << ui_text("Select one or more file numbers, separated by spaces or commas.\n",
                          "请选择一个或多个文件编号，以空格或逗号分隔。\n");
    }
    std::string line;
    while (true) {
        output << ui_text("Select: ", "选择：") << std::flush;
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
        throw std::invalid_argument(ui_text("selected data source is missing or is not a direct real directory",
                                           "所选数据来源不存在，或不是实际的一级目录"));
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
        throw std::invalid_argument(ui_text("selected data directory is missing or is a link",
                                           "所选数据目录不存在或是符号链接"));
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
    std::vector<std::string> labels{ui_text("All product types (no directory filter)", "全部产品类型（不按目录筛选）")};
    for (const auto& directory : directories) {
        labels.push_back(directory.filename().string());
    }
    const auto choice = choose(ui_text("[Product Types / Directories]", "[产品类型 / 目录]"), labels, false, input, output, errors);
    if (!choice) {
        return;
    }
    const auto index = choice->front();
    const auto directory = index == 0 ? std::filesystem::path{} : directories[index - 1];
    auto files = data_files_in(directory.empty() ? selection.data_root : directory);
    selection.product_directory = directory;
    selection.data_files = std::move(files);
    output << ui_text("[OK] Product selection includes ", "[OK] 所选产品包含 ") << selection.data_files.size()
           << ui_text(" data file(s).\n", " 个数据文件。\n");
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
    const auto choices = choose(ui_text("[Data Files]", "[数据文件]"), labels, true, input, output, errors);
    if (!choices) {
        return;
    }
    std::vector<std::filesystem::path> selected;
    for (const auto index : *choices) {
        selected.push_back(files[index]);
    }
    selection.data_files = std::move(selected);
    output << ui_text("[OK] Selected ", "[OK] 已选择 ") << selection.data_files.size()
           << ui_text(" data file(s).\n", " 个数据文件。\n");
}

inline void select_strategy(backtest_selection& selection, std::istream& input,
                            std::ostream& output, std::ostream& errors) {
    const auto& registry = seastar::xtrader::strategy_registry::instance();
    const auto keys = registry.keys();
    if (keys.empty()) {
        output << ui_text("[WARN] No strategies registered.\n", "[WARN] 没有已注册的策略。\n");
        return;
    }
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    for (const auto& key : keys) {
        auto strategy = registry.create(key);
        if (!strategy) {
            continue;
        }
        const auto info = strategy->get_info();
        const auto name = json_string_field(info, "name");
        const bool backtest = json_bool_field(info, "backtest_support");
        const bool intraday = json_bool_field(info, "is_intraday");
        ids.push_back(key);
        labels.push_back((name.empty() ? key : name) + " (" + key + ")"
            + (backtest ? "" : ui_text(", no backtest", "，不支持回测"))
            + (intraday ? ui_text(", intraday", "，日内") : ""));
    }
    const auto choice = choose(ui_text("[Strategies]", "[策略]"), labels, false, input, output, errors);
    if (!choice) {
        return;
    }
    const auto& key = ids[choice->front()];
    selection.strategy_id = key;
    output << ui_text("[OK] Strategy: ", "[OK] 策略：") << key << '\n';
}

inline void require_ready(const market_data_paths& paths, const backtest_selection& selection) {
    require_source(paths, selection);
    if (selection.data_files.empty() || selection.strategy_id.empty()) {
        throw std::invalid_argument(ui_text("select data files and a strategy before Run",
                                           "运行前请先选择数据文件和策略"));
    }
    for (const auto& file : selection.data_files) {
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file))) {
            throw std::invalid_argument(ui_text("a selected data file is missing or is a link",
                                               "所选数据文件不存在或是符号链接"));
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
        output << ui_text("\n[Backtest Setup]\nData root: ",
                          "\n[回测设置]\n数据目录：")
               << selection.data_root.string() << ui_text("\nProduct type: ", "\n产品类型：")
               << (selection.product_directory.empty() ? ui_text("(all)", "（全部）") : selection.product_directory.filename().string())
               << ui_text("\nFiles: ", "\n文件数：") << selection.data_files.size()
               << ui_text("\nStrategy: ", "\n策略：")
               << (selection.strategy_id.empty() ? ui_text("(not selected)", "（未选择）") : selection.strategy_id)
               << ui_text("\n1) Select data source\n2) Select product type\n3) Select data files\n"
                          "4) Select strategy\n5) Run\nu) Back\nSelect: ",
                          "\n1) 选择数据来源\n2) 选择产品类型\n3) 选择数据文件\n"
                          "4) 选择策略\n5) 运行\nu) 返回\n选择：") << std::flush;
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
                output << ui_text("[Backtest Run]\n", "[回测运行]\n") << run(selection) << '\n';
            } else {
                errors << ui_text("[ERROR] Unknown backtest menu option.\n", "[ERROR] 无效的回测菜单选项。\n");
            }
        } catch (const std::filesystem::filesystem_error& error) {
            errors << ui_text("[ERROR] Cannot access backtest data: ", "[ERROR] 无法访问回测数据：") << error.what() << '\n';
        } catch (const std::invalid_argument& error) {
            errors << "[ERROR] " << error.what() << '\n';
        }
    }
}

} // namespace seastar::xtrader::cli
