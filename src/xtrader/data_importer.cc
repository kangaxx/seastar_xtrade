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
#include <seastar/xtrader/data_importer.hh>
#include <seastar/xtrader/market_data_writer.hh>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace seastar::xtrader {
namespace {

struct internal_import_config {
    std::filesystem::path target_root = market_data_paths{}.root();
    std::filesystem::path report_root = "./exports/replay/import_reports";
    bool recursive_scan = true;
    std::string destination_source;
    std::string asset_class = "futures";
    std::string instrument;
    std::string exchange;
    std::string taobao_timeframe = "1m";
    bool timeframe_explicit = false;
};

std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string to_upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::vector<std::string> split_csv_line(std::string line, char) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    auto tokens = parse_market_csv_row(line);
    for (auto& token : tokens) { token = trim_copy(token); }
    return tokens;
}

bool parse_bool_value(const std::string& value, bool default_value) {
    const std::string normalized = to_lower_copy(trim_copy(value));
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "y") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "n") {
        return false;
    }
    (void)default_value;
    throw std::invalid_argument("invalid recursive_scan boolean");
}

bool parse_taobao_datetime(const std::string& value, std::string& normalized) {
    const bool slash_date = value.find('/') != std::string::npos;
    const bool has_time = value.find(' ') != std::string::npos;
    const auto colons = std::count(value.begin(), value.end(), ':');
    if (value.empty() || (has_time && (colons < 1 || colons > 2))
            || value.back() < '0' || value.back() > '9') {
        return false;
    }
    const char* format = slash_date ? "%Y/%m/%d" : "%Y-%m-%d";
    if (has_time) {
        format = slash_date
            ? (colons == 2 ? "%Y/%m/%d %H:%M:%S" : "%Y/%m/%d %H:%M")
            : (colons == 2 ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M");
    }
    std::tm datetime{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::get_time(&datetime, format);
    if (input.fail()) { return false; }
    input >> std::ws;
    if (!input.eof()) { return false; }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setfill('0') << std::setw(4) << datetime.tm_year + 1900 << '-'
           << std::setw(2) << datetime.tm_mon + 1 << '-' << std::setw(2) << datetime.tm_mday << ' '
           << std::setw(2) << datetime.tm_hour << ':' << std::setw(2) << datetime.tm_min << ':'
           << std::setw(2) << datetime.tm_sec;
    normalized = output.str();
    return true;
}

bool load_key_value_config(const std::filesystem::path& path, std::map<std::string, std::string>& values) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t pos = trimmed.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        values[to_lower_copy(trim_copy(trimmed.substr(0, pos)))] = trim_copy(trimmed.substr(pos + 1));
    }
    return !file.bad();
}

internal_import_config load_import_config(const std::filesystem::path& config_path, bool required) {
    internal_import_config config;
    std::map<std::string, std::string> values;
    if (!load_key_value_config(config_path, values)) {
        if (required || std::filesystem::exists(config_path)) {
            throw std::runtime_error("cannot read import config: " + config_path.string());
        }
        return config;
    }
    {
        auto find_value = [&values] (const std::string& key) -> std::string {
            auto it = values.find(key);
            return it == values.end() ? std::string() : it->second;
        };

        const std::string target_root = find_value("data_root");
        const std::string report_root = find_value("report_root");
        const std::string recursive_scan = find_value("recursive_scan");
        const std::string taobao_timeframe = find_value("taobao_timeframe");

        if (!target_root.empty()) {
            config.target_root = target_root;
        }
        if (!report_root.empty()) {
            config.report_root = report_root;
        }
        if (!recursive_scan.empty()) {
            config.recursive_scan = parse_bool_value(recursive_scan, config.recursive_scan);
        }
        if (!taobao_timeframe.empty()) {
            config.taobao_timeframe = taobao_timeframe;
            config.timeframe_explicit = true;
        }
        if (!find_value("timeframe").empty()) {
            config.taobao_timeframe = find_value("timeframe");
            config.timeframe_explicit = true;
        }
        config.destination_source = find_value("destination_source");
        config.instrument = find_value("instrument");
        config.exchange = find_value("exchange");
        if (!find_value("asset_class").empty()) { config.asset_class = find_value("asset_class"); }
        if (!find_value("taobao_data_domain").empty()) {
            throw std::invalid_argument("taobao_data_domain is obsolete; use asset_class and destination_source");
        }
    }
    return config;
}

std::filesystem::path resolve_config_path(const import_options& options) {
    if (!options.config_path.empty()) {
        return std::filesystem::path(options.config_path);
    }

    const char* env_value = std::getenv("XTRADER_IMPORT_CONFIG");
    if (env_value != nullptr && env_value[0] != '\0') {
        return std::filesystem::path(env_value);
    }

    return std::filesystem::path("/xtrader_conf/import.conf");
}

std::vector<std::filesystem::path> collect_source_files(const std::filesystem::path& source_path, bool recursive_scan) {
    std::vector<std::filesystem::path> files;
    std::error_code error_code;
    if (std::filesystem::is_regular_file(source_path, error_code)) {
        files.push_back(source_path);
        return files;
    }
    if (error_code) {
        throw std::filesystem::filesystem_error("inspect import source", source_path, error_code);
    }

    if (!std::filesystem::is_directory(source_path, error_code)) {
        if (error_code) {
            throw std::filesystem::filesystem_error("inspect import source directory", source_path, error_code);
        }
        return files;
    }

    if (recursive_scan) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source_path, error_code)) {
            if (error_code) {
                break;
            }
            if (entry.is_regular_file() && to_lower_copy(entry.path().extension().string()) == ".csv") {
                files.push_back(entry.path());
            }
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(source_path, error_code)) {
            if (error_code) {
                break;
            }
            if (entry.is_regular_file() && to_lower_copy(entry.path().extension().string()) == ".csv") {
                files.push_back(entry.path());
            }
        }
    }

    if (error_code) {
        throw std::filesystem::filesystem_error("scan import source", source_path, error_code);
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool parse_taobao_file_name(const std::filesystem::path& path, std::string& symbol,
                          std::string& exchange, std::string& timeframe) {
    const std::string stem = path.stem().string();
    const std::size_t separator_pos = stem.find('.');
    if (separator_pos == std::string::npos) {
        return false;
    }

    symbol = stem.substr(0, separator_pos);
    const auto period_pos = stem.find('.', separator_pos + 1);
    exchange = to_upper_copy(stem.substr(separator_pos + 1,
        period_pos == std::string::npos ? std::string::npos : period_pos - separator_pos - 1));
    if (period_pos != std::string::npos) {
        timeframe = market_data_paths::normalize_timeframe(stem.substr(period_pos + 1));
    }
    return !symbol.empty() && !exchange.empty();
}

bool build_taobao_row(const std::vector<std::string>& tokens, market_csv_row& row) {
    if (tokens.size() < 9) {
        return false;
    }

    std::string normalized_datetime;
    if (!parse_taobao_datetime(tokens[0], normalized_datetime)) {
        return false;
    }

    row = {normalized_datetime, tokens[1], tokens[2], tokens[3], tokens[4],
           tokens[5], tokens[7], tokens[6], tokens[8]};
    return true;
}

std::string current_timestamp_for_file_name() {
    auto now = std::chrono::system_clock::now();
    std::time_t current = std::chrono::system_clock::to_time_t(now);
    const auto fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) % 1'000'000'000;
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &current);
#else
    localtime_r(&current, &local_time);
#endif
    std::ostringstream output;
    output << std::put_time(&local_time, "%Y%m%d_%H%M%S")
           << '_'
           << std::setfill('0')
           << std::setw(9)
           << fraction.count();
    return output.str();
}

std::filesystem::path write_report(
    const std::filesystem::path& report_root,
    const import_options& options,
    const import_run_result& result) {

    std::error_code error_code;
    std::filesystem::create_directories(report_root, error_code);
    if (error_code) { return {}; }
    const std::filesystem::path report_path = report_root / ("import_report_" + current_timestamp_for_file_name() + ".md");
    std::ofstream output(report_path, std::ios::trunc);
    if (!output.is_open()) {
        return {};
    }

    output << "# Import Report\n\n";
    output << "## Summary\n\n";
    output << "- source_path: " << options.source_path << "\n";
    output << "- source_type: " << to_string(options.source_type) << "\n";
    output << "- import_mode: " << to_string(options.mode) << "\n";
    output << "- target_root: " << result.target_root.generic_string() << "\n";
    output << "- scanned_files: " << result.scanned_files << "\n";
    output << "- success_files: " << result.success_files << "\n";
    output << "- skipped_files: " << result.skipped_files << "\n";
    output << "- failed_files: " << result.failed_files << "\n";
    output << "- rows_added: " << result.rows_added << "\n";
    output << "- rows_overwritten: " << result.rows_overwritten << "\n";
    output << "- rows_skipped: " << result.rows_skipped << "\n";
    output << "- invalid_rows: " << result.invalid_rows << "\n";
    output << "- message: " << result.message << "\n\n";

    output << "## File Result Matrix\n\n";
    output << "| source_file | status | message | rows_read | rows_added | rows_overwritten | rows_skipped | invalid_rows |\n";
    output << "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& file_report : result.file_reports) {
        output << "| "
               << file_report.source_file.generic_string() << " | "
               << file_report.status << " | "
               << file_report.message << " | "
               << file_report.rows_read << " | "
               << file_report.rows_added << " | "
               << file_report.rows_overwritten << " | "
               << file_report.rows_skipped << " | "
               << file_report.invalid_rows << " |\n";
    }
    output << "\n";
    for (const auto& report : result.file_reports) {
        output << "- " << report.source_file.generic_string() << " (" << report.timeframe << "):\n";
        for (const auto& path : report.target_files) { output << "  - " << path.generic_string() << "\n"; }
    }
    output.flush();
    if (!output) { return {}; }
    return report_path;
}

import_file_report import_csv_file(
    const std::filesystem::path& source_file,
    const internal_import_config& config,
    import_mode mode,
    const market_data_paths& paths,
    import_source_type source_type) {

    import_file_report report;
    report.source_file = source_file;
    report.timeframe = config.taobao_timeframe;
    report.status = "failed";

    std::string dataset_symbol;
    std::string exchange, file_timeframe;
    if (!config.instrument.empty() && !config.exchange.empty()) {
        dataset_symbol = config.instrument;
        exchange = config.exchange;
    } else if (!parse_taobao_file_name(source_file, dataset_symbol, exchange, file_timeframe)) {
        report.message = "use <instrument>.<exchange>[.<timeframe>].csv, or configure instrument and exchange";
        return report;
    }

    report.dataset_symbol = dataset_symbol;
    report.exchange = exchange;

    std::ifstream file(source_file);
    if (!file.is_open()) {
        report.message = "failed to open source file";
        return report;
    }

    std::string header_line;
    if (!std::getline(file, header_line)) {
        report.message = "source file is empty";
        return report;
    }

    auto header_tokens = split_csv_line(header_line, ',');
    if (!header_tokens.empty() && header_tokens.front().compare(0, 3, "\xEF\xBB\xBF") == 0) {
        header_tokens.front().erase(0, 3);
    }
    const std::vector<std::string> taobao_header = {
        "date", "open", "high", "low", "close", "volume", "money", "open_interest", "symbol"
    };
    const auto tick_header = parse_market_csv_row(market_csv_header(market_data_kind::tick).substr(
        0, market_csv_header(market_data_kind::tick).size() - 1));
    const auto bar_header = parse_market_csv_row(market_csv_header(market_data_kind::bar).substr(
        0, market_csv_header(market_data_kind::bar).size() - 1));
    auto normalized_header = header_tokens;
    for (auto& token : normalized_header) { token = to_lower_copy(token); }
    const bool raw_tick = normalized_header == tick_header;
    const bool taobao_bar = normalized_header == taobao_header;
    const bool canonical_bar = normalized_header == bar_header
        || normalized_header == std::vector<std::string>{
            "date", "open", "high", "low", "close", "volume", "open_interest"};
    if ((source_type == import_source_type::taobao_csv && !taobao_bar)
            || (source_type == import_source_type::ctp_clean_csv && !raw_tick && !canonical_bar)) {
        report.message = "unsupported CSV schema; select taobao_csv or canonical ctp_clean_csv as appropriate";
        return report;
    }
    auto timeframe = file_timeframe.empty() ? config.taobao_timeframe : file_timeframe;
    if (config.timeframe_explicit) {
        if (!file_timeframe.empty() && file_timeframe != config.taobao_timeframe) {
            report.message = "filename timeframe differs from explicitly configured timeframe";
            return report;
        }
        timeframe = config.taobao_timeframe;
    }
    if (raw_tick && !config.timeframe_explicit && file_timeframe.empty()) { timeframe = "tick"; }
    if ((data_kind_for_timeframe(timeframe) == market_data_kind::tick) != raw_tick) {
        report.message = "CSV schema differs from selected Tick/bar timeframe";
        return report;
    }
    report.timeframe = timeframe;
    const market_data_key key{config.destination_source, config.asset_class,
        exchange, dataset_symbol, timeframe};
    (void)paths.dataset_directory(key);
    std::vector<market_csv_row> rows;
    std::string line;
    std::size_t line_number = 1;
    while (std::getline(file, line)) {
        ++line_number;
        if (trim_copy(line).empty()) {
            continue;
        }
        auto tokens = split_csv_line(line, ',');
        market_csv_row row;
        if (tokens.size() != header_tokens.size()
                || (taobao_bar && !build_taobao_row(tokens, row))) {
            ++report.invalid_rows;
            report.message = "invalid CSV row at line " + std::to_string(line_number) + "; file not imported";
            return report;
        }
        if (!taobao_bar) {
            row = std::move(tokens);
            if (!raw_tick) {
                std::string datetime;
                if (!parse_taobao_datetime(row[0], datetime)) {
                    ++report.invalid_rows;
                    report.message = "invalid datetime at line " + std::to_string(line_number);
                    return report;
                }
                row[0] = datetime;
                row.resize(9);
            }
        }
        try {
            (void)validate_market_csv_row(key, row);
        } catch (const std::invalid_argument& error) {
            ++report.invalid_rows;
            report.message = "invalid row at line " + std::to_string(line_number) + ": " + error.what();
            return report;
        } catch (const std::out_of_range& error) {
            ++report.invalid_rows;
            report.message = "invalid row at line " + std::to_string(line_number) + ": " + error.what();
            return report;
        }
        ++report.rows_read;
        const auto datetime = raw_tick ? row[6] + " " + row[8] : row[0];
        if (report.earliest_timestamp.empty() || datetime < report.earliest_timestamp) {
            report.earliest_timestamp = datetime;
        }
        if (report.latest_timestamp.empty() || datetime > report.latest_timestamp) {
            report.latest_timestamp = datetime;
        }
        rows.push_back(std::move(row));
    }
    if (file.bad()) { report.message = "error reading source CSV"; return report; }
    if (rows.empty()) {
        report.message = "no data rows imported";
        return report;
    }
    market_data_writer writer(paths);
    const auto written = writer.merge(key, rows, mode == import_mode::overwrite);
    report.target_files = written.files;
    report.target_file = written.files.front();
    report.rows_added = written.added;
    report.rows_overwritten = written.overwritten;
    report.rows_skipped = written.skipped;
    report.status = (report.rows_added == 0 && report.rows_overwritten == 0) ? "skipped" : "success";
    report.message = (report.status == "skipped") ? "all matching timestamps already existed" : "import completed";
    return report;
}

} // namespace

bool parse_source_type(const std::string& value, import_source_type& source_type) {
    const std::string normalized = to_lower_copy(trim_copy(value));
    if (normalized == "taobao" || normalized == "taobao_csv" || normalized == "t") {
        source_type = import_source_type::taobao_csv;
        return true;
    }
    if (normalized == "ctp" || normalized == "ctp_clean_csv" || normalized == "c") {
        source_type = import_source_type::ctp_clean_csv;
        return true;
    }
    return false;
}

bool parse_import_mode(const std::string& value, import_mode& mode) {
    const std::string normalized = to_lower_copy(trim_copy(value));
    if (normalized == "incremental" || normalized == "inc" || normalized == "i") {
        mode = import_mode::incremental;
        return true;
    }
    if (normalized == "overwrite" || normalized == "cover" || normalized == "o") {
        mode = import_mode::overwrite;
        return true;
    }
    return false;
}

std::string to_string(import_source_type source_type) {
    switch (source_type) {
    case import_source_type::taobao_csv:
        return "taobao_csv";
    case import_source_type::ctp_clean_csv:
        return "ctp_clean_csv";
    }
    return "unknown";
}

std::string to_string(import_mode mode) {
    switch (mode) {
    case import_mode::incremental:
        return "incremental";
    case import_mode::overwrite:
        return "overwrite";
    }
    return "unknown";
}

import_run_result data_importer::execute(const import_options& options) const {
    import_run_result result;
    if (options.source_path.empty()) {
        result.message = "import source path is empty";
        return result;
    }

    const std::filesystem::path config_path = resolve_config_path(options);
    internal_import_config config;
    std::optional<market_data_paths> storage;
    try {
        const char* environment_config = std::getenv("XTRADER_IMPORT_CONFIG");
        config = load_import_config(config_path, !options.config_path.empty()
            || (environment_config != nullptr && environment_config[0] != '\0'));
        if (!options.destination_source.empty()) { config.destination_source = options.destination_source; }
        if (config.destination_source.empty()) {
            config.destination_source = options.source_type == import_source_type::taobao_csv ? "taobao" : "historic";
        }
        if (!options.timeframe.empty()) {
            config.taobao_timeframe = options.timeframe;
            config.timeframe_explicit = true;
        }
        config.taobao_timeframe = market_data_paths::normalize_timeframe(config.taobao_timeframe);
        if (!options.instrument.empty()) { config.instrument = options.instrument; }
        if (!options.exchange.empty()) { config.exchange = options.exchange; }
        if (config.instrument.empty() != config.exchange.empty()) {
            throw std::invalid_argument("configure both instrument and exchange, or neither");
        }
        storage.emplace(config.target_root);
        for (const std::string source : {"x_trader_data", "historic", "taobao", "virtual_data"}) {
            if (storage->root().filename() == source) {
                throw std::invalid_argument("data_root must be the shared root (e.g. /data), not a source directory");
            }
        }
        try {
            (void)storage->source_root(config.destination_source);
        } catch (const std::invalid_argument&) {
            storage.emplace(config.target_root, std::vector<std::string>{config.destination_source});
        }
    } catch (const std::exception& error) {
        result.message = std::string("invalid import configuration: ") + error.what();
        return result;
    }
    const auto& paths = *storage;
    result.target_root = paths.source_root(config.destination_source);

    const std::filesystem::path source_path = std::filesystem::path(options.source_path);
    std::vector<std::filesystem::path> files;
    try {
        files = collect_source_files(source_path, config.recursive_scan);
    } catch (const std::filesystem::filesystem_error& error) {
        result.message = std::string("cannot scan import source: ") + error.what();
        return result;
    }
    result.scanned_files = files.size();
    if (files.empty()) {
        result.message = "no csv files found under source path";
        return result;
    }

    for (const auto& file : files) {
        import_file_report file_report;
        try {
            file_report = import_csv_file(file, config, options.mode, paths, options.source_type);
        } catch (const std::exception& ex) {
            file_report.source_file = file;
            file_report.status = "failed";
            file_report.message = std::string("unexpected exception: ") + ex.what();
        }

        result.rows_added += file_report.rows_added;
        result.rows_overwritten += file_report.rows_overwritten;
        result.rows_skipped += file_report.rows_skipped;
        result.invalid_rows += file_report.invalid_rows;

        if (file_report.status == "success") {
            ++result.success_files;
        } else if (file_report.status == "skipped") {
            ++result.skipped_files;
        } else {
            ++result.failed_files;
        }

        result.file_reports.push_back(std::move(file_report));
    }

    result.success = result.scanned_files > 0 && result.failed_files == 0;
    result.message = result.success ? "import finished" : "import finished with failures";
    result.report_path = write_report(config.report_root, options, result);
    if (result.report_path.empty()) {
        result.success = false;
        result.message += "; cannot write import report";
    }
    return result;
}

} // namespace seastar::xtrader
