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

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace seastar::xtrader {

enum class import_source_type {
    taobao_csv,
    ctp_clean_csv,
};

enum class import_mode {
    incremental,
    overwrite,
};

struct import_options {
    std::string source_path;
    import_source_type source_type = import_source_type::taobao_csv;
    import_mode mode = import_mode::incremental;
    std::string config_path;
};

struct import_file_report {
    std::filesystem::path source_file;
    std::filesystem::path target_file;
    std::string dataset_symbol;
    std::string exchange;
    std::string timeframe;
    std::string status;
    std::string message;
    std::size_t rows_read = 0;
    std::size_t rows_added = 0;
    std::size_t rows_overwritten = 0;
    std::size_t rows_skipped = 0;
    std::size_t invalid_rows = 0;
    std::string earliest_timestamp;
    std::string latest_timestamp;
};

struct import_run_result {
    bool success = false;
    std::filesystem::path report_path;
    std::filesystem::path target_root;
    std::size_t scanned_files = 0;
    std::size_t success_files = 0;
    std::size_t skipped_files = 0;
    std::size_t failed_files = 0;
    std::size_t rows_added = 0;
    std::size_t rows_overwritten = 0;
    std::size_t rows_skipped = 0;
    std::size_t invalid_rows = 0;
    std::string message;
    std::vector<import_file_report> file_reports;
};

bool parse_source_type(const std::string& value, import_source_type& source_type);
bool parse_import_mode(const std::string& value, import_mode& mode);
std::string to_string(import_source_type source_type);
std::string to_string(import_mode mode);

class data_importer {
public:
    import_run_result execute(const import_options& options) const;
};

} // namespace seastar::xtrader
