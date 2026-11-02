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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>

namespace seastar::xtrader {
namespace {

struct internal_import_config {
    std::filesystem::path target_root = "/data/x_trader_data";
    std::filesystem::path report_root = "./exports/replay/import_reports";
    bool recursive_scan = true;
    std::string taobao_data_domain = "dominant";
    std::string taobao_timeframe = "1m";
};

struct standard_row {
    std::string datetime;
    std::string open;
    std::string high;
    std::string low;
    std::string close;
    std::string volume;
    std::string open_interest;
    std::string money;
    std::string source_symbol;
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

std::vector<std::string> split_csv_line(const std::string& line, char separator) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (std::getline(stream, token, separator)) {
        tokens.push_back(trim_copy(token));
    }
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
    return default_value;
}

bool parse_taobao_datetime(const std::string& value, std::string& normalized) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.c_str(), "%d/%d/%d %d:%d", &year, &month, &day, &hour, &minute) != 5) {
        if (std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
            return false;
        }
    }

    std::ostringstream output;
    output << std::setfill('0')
           << std::setw(4) << year << '-'
           << std::setw(2) << month << '-'
           << std::setw(2) << day << ' '
           << std::setw(2) << hour << ':'
           << std::setw(2) << minute << ':'
           << std::setw(2) << second;
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
    return true;
}

internal_import_config load_import_config(const std::filesystem::path& config_path) {
    internal_import_config config;
    std::map<std::string, std::string> values;
    if (!config_path.empty() && load_key_value_config(config_path, values)) {
        auto find_value = [&values] (const std::string& key) -> std::string {
            auto it = values.find(key);
            return it == values.end() ? std::string() : it->second;
        };

        const std::string target_root = find_value("data_root");
        const std::string report_root = find_value("report_root");
        const std::string recursive_scan = find_value("recursive_scan");
        const std::string taobao_data_domain = find_value("taobao_data_domain");
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
        if (!taobao_data_domain.empty()) {
            config.taobao_data_domain = taobao_data_domain;
        }
        if (!taobao_timeframe.empty()) {
            config.taobao_timeframe = taobao_timeframe;
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

    if (!std::filesystem::is_directory(source_path, error_code)) {
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

    std::sort(files.begin(), files.end());
    return files;
}

bool parse_taobao_file_name(const std::filesystem::path& path, std::string& symbol, std::string& exchange) {
    const std::string stem = path.stem().string();
    const std::size_t separator_pos = stem.find('.');
    if (separator_pos == std::string::npos) {
        return false;
    }

    symbol = to_lower_copy(stem.substr(0, separator_pos));
    exchange = to_upper_copy(stem.substr(separator_pos + 1));
    return !symbol.empty() && !exchange.empty();
}

bool build_taobao_row(const std::vector<std::string>& tokens, standard_row& row) {
    if (tokens.size() < 9) {
        return false;
    }

    std::string normalized_datetime;
    if (!parse_taobao_datetime(tokens[0], normalized_datetime)) {
        return false;
    }

    auto parse_numeric = [] (const std::string& value) -> bool {
        try {
            (void)std::stod(value);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    if (!parse_numeric(tokens[1]) || !parse_numeric(tokens[2]) || !parse_numeric(tokens[3])
        || !parse_numeric(tokens[4]) || !parse_numeric(tokens[5]) || !parse_numeric(tokens[6])
        || !parse_numeric(tokens[7])) {
        return false;
    }

    row.datetime = normalized_datetime;
    row.open = tokens[1];
    row.high = tokens[2];
    row.low = tokens[3];
    row.close = tokens[4];
    row.volume = tokens[5];
    row.open_interest = tokens[7];
    row.money = tokens[6];
    row.source_symbol = tokens[8];
    return true;
}

bool load_existing_rows(const std::filesystem::path& target_file, std::map<std::string, standard_row>& rows) {
    std::ifstream file(target_file);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) {
        return true;
    }

    while (std::getline(file, line)) {
        const std::vector<std::string> tokens = split_csv_line(line, ',');
        if (tokens.size() < 7) {
            continue;
        }

        standard_row row;
        row.datetime = tokens[0];
        row.open = tokens[1];
        row.high = tokens[2];
        row.low = tokens[3];
        row.close = tokens[4];
        row.volume = tokens[5];
        row.open_interest = tokens[6];
        row.money = tokens.size() > 7 ? tokens[7] : "";
        row.source_symbol = tokens.size() > 8 ? tokens[8] : "";
        rows[row.datetime] = row;
    }
    return true;
}

bool write_standard_rows(const std::filesystem::path& target_file, const std::map<std::string, standard_row>& rows) {
    std::ofstream output(target_file, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output << "date,open,high,low,close,volume,open_interest,money,source_symbol\n";
    for (const auto& entry : rows) {
        const standard_row& row = entry.second;
        output << row.datetime << ','
               << row.open << ','
               << row.high << ','
               << row.low << ','
               << row.close << ','
               << row.volume << ','
               << row.open_interest << ','
               << row.money << ','
               << row.source_symbol << '\n';
    }
    return true;
}

bool write_manifest(
    const std::filesystem::path& manifest_path,
    const std::string& symbol,
    const std::string& exchange,
    const std::string& timeframe,
    const std::string& active_file,
    const std::string& source,
    const std::string& data_kind) {

    std::ofstream output(manifest_path, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output << "symbol=" << symbol << "\n";
    output << "exchange=" << exchange << "\n";
    output << "timeframe=" << timeframe << "\n";
    output << "source=" << source << "\n";
    output << "data_kind=" << data_kind << "\n";
    output << "active_file=" << active_file << "\n";
    output << "headers=true\n";
    output << "separator=,\n";
    output << "datetime=0\n";
    output << "open=1\n";
    output << "high=2\n";
    output << "low=3\n";
    output << "close=4\n";
    output << "volume=5\n";
    output << "openinterest=6\n";
    output << "money_col=7\n";
    output << "source_symbol_col=8\n";
    return true;
}

std::string current_timestamp_for_file_name() {
    auto now = std::chrono::system_clock::now();
    std::time_t current = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
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
           << std::setw(3)
           << milliseconds.count();
    return output.str();
}

std::filesystem::path write_report(
    const std::filesystem::path& report_root,
    const import_options& options,
    const import_run_result& result) {

    std::error_code error_code;
    std::filesystem::create_directories(report_root, error_code);
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

    return report_path;
}

import_file_report import_taobao_file(
    const std::filesystem::path& source_file,
    const internal_import_config& config,
    import_mode mode,
    const std::filesystem::path& target_root) {

    import_file_report report;
    report.source_file = source_file;
    report.timeframe = config.taobao_timeframe;
    report.status = "failed";

    std::string dataset_symbol;
    std::string exchange;
    if (!parse_taobao_file_name(source_file, dataset_symbol, exchange)) {
        report.message = "unsupported taobao file name pattern";
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

    const std::vector<std::string> header_tokens = split_csv_line(header_line, ',');
    const std::vector<std::string> expected_header = {
        "date", "open", "high", "low", "close", "volume", "money", "open_interest", "symbol"
    };
    if (header_tokens.size() < expected_header.size()) {
        report.message = "unexpected header column count";
        return report;
    }
    for (std::size_t i = 0; i < expected_header.size(); ++i) {
        if (to_lower_copy(header_tokens[i]) != expected_header[i]) {
            report.message = "unsupported header format";
            return report;
        }
    }

    const std::filesystem::path dataset_dir = target_root / config.taobao_data_domain / exchange / dataset_symbol / config.taobao_timeframe;
    const std::string target_file_name = dataset_symbol + "." + exchange + "." + config.taobao_timeframe + ".imported.csv";
    const std::filesystem::path target_file = dataset_dir / target_file_name;
    const std::filesystem::path manifest_file = dataset_dir / "dataset-manifest.conf";
    report.target_file = target_file;

    std::error_code error_code;
    std::filesystem::create_directories(dataset_dir, error_code);
    if (error_code) {
        report.message = "failed to create target directory";
        return report;
    }

    std::map<std::string, standard_row> merged_rows;
    load_existing_rows(target_file, merged_rows);

    std::string line;
    while (std::getline(file, line)) {
        if (trim_copy(line).empty()) {
            continue;
        }

        const std::vector<std::string> tokens = split_csv_line(line, ',');
        standard_row row;
        if (!build_taobao_row(tokens, row)) {
            ++report.invalid_rows;
            continue;
        }

        ++report.rows_read;
        if (report.earliest_timestamp.empty() || row.datetime < report.earliest_timestamp) {
            report.earliest_timestamp = row.datetime;
        }
        if (report.latest_timestamp.empty() || row.datetime > report.latest_timestamp) {
            report.latest_timestamp = row.datetime;
        }

        auto existing = merged_rows.find(row.datetime);
        if (existing == merged_rows.end()) {
            merged_rows[row.datetime] = row;
            ++report.rows_added;
            continue;
        }

        if (mode == import_mode::incremental) {
            ++report.rows_skipped;
            continue;
        }

        existing->second = row;
        ++report.rows_overwritten;
    }

    if (report.rows_read == 0 && report.invalid_rows > 0) {
        report.message = "no valid rows imported";
        return report;
    }

    if (!write_standard_rows(target_file, merged_rows)) {
        report.message = "failed to write target csv";
        return report;
    }

    if (!write_manifest(
            manifest_file,
            dataset_symbol,
            exchange,
            config.taobao_timeframe,
            target_file_name,
            "taobao_csv",
            config.taobao_data_domain)) {
        report.message = "failed to write dataset manifest";
        return report;
    }

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

    if (options.source_type == import_source_type::ctp_clean_csv) {
        result.message = "ctp_clean_csv is not implemented yet";
        return result;
    }

    const std::filesystem::path config_path = resolve_config_path(options);
    const internal_import_config config = load_import_config(config_path);
    result.target_root = std::filesystem::absolute(config.target_root);

    const std::filesystem::path source_path = std::filesystem::path(options.source_path);
    const std::vector<std::filesystem::path> files = collect_source_files(source_path, config.recursive_scan);
    result.scanned_files = files.size();
    if (files.empty()) {
        result.message = "no csv files found under source path";
        return result;
    }

    for (const auto& file : files) {
        import_file_report file_report;
        try {
            file_report = import_taobao_file(file, config, options.mode, result.target_root);
        } catch (const std::exception& ex) {
            file_report.source_file = file;
            file_report.status = "failed";
            file_report.message = std::string("unexpected exception: ") + ex.what();
        } catch (...) {
            file_report.source_file = file;
            file_report.status = "failed";
            file_report.message = "unexpected unknown exception";
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

    result.success = (result.success_files > 0 || (result.scanned_files > 0 && result.failed_files == 0));
    result.message = result.success ? "import finished" : "import finished with failures";
    result.report_path = write_report(config.report_root, options, result);
    return result;
}

} // namespace seastar::xtrader
