/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace seastar::xtrader::test {

// B3: isolated synthetic historical datasets for replay integration tests.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
class replay_test_directory {
public:
    replay_test_directory() {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            _path = std::filesystem::temp_directory_path() /
                ("xtrader-replay-input-" + std::to_string(timestamp) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(_path, ec)) {
                return;
            }
            if (ec && ec != std::errc::file_exists) {
                throw std::runtime_error("cannot create replay test directory: " + ec.message());
            }
        }
        throw std::runtime_error("cannot allocate unique replay test directory");
    }

    replay_test_directory(const replay_test_directory&) = delete;
    replay_test_directory& operator=(const replay_test_directory&) = delete;

    ~replay_test_directory() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
        if (ec) {
            std::cerr << "Cannot remove replay test directory: " << ec.message() << std::endl;
        }
    }

    const std::filesystem::path& path() const noexcept { return _path; }

private:
    std::filesystem::path _path;
};

inline void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
    if (!file) {
        throw std::runtime_error("cannot write replay test file: " + path.string());
    }
}

inline std::filesystem::path write_dataset(const std::filesystem::path& root, const std::string& exchange,
                                           const std::string& rows, const std::string& symbol = "rb9999") {
    const auto directory = root / exchange / symbol / "1m";
    std::filesystem::create_directories(directory);
    write_text(directory / "dataset-manifest.conf",
        "symbol=" + symbol + "\nexchange=" + exchange + "\ntimeframe=1m\nactive_file=bars.csv\n");
    const auto csv = directory / "bars.csv";
    write_text(csv, "date,open,high,low,close,volume,open_interest\n" + rows);
    return csv;
}

} // namespace seastar::xtrader::test
