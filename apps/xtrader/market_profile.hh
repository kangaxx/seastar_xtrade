// Non-secret account labels and key paths accompany encrypted profiles, not key material.
// Source: user request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include "market_capture_io.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace seastar::xtrader::cli {

struct market_profile {
    std::string alias;
    std::filesystem::path key;
};

inline std::filesystem::path market_profile_file(const std::filesystem::path& config) {
    auto path = config;
    path += ".profile";
    return path;
}

inline void validate_market_profile(const market_profile& profile) {
    if (profile.alias.empty() || profile.alias.find_first_not_of(' ') == std::string::npos
            || profile.alias.size() > 128
            || std::any_of(profile.alias.begin(), profile.alias.end(), [] (unsigned char ch) {
                return ch < 32 || ch == 127;
            })) {
        throw std::invalid_argument("account alias must be 1-128 UTF-8 bytes without control characters");
    }
    if (profile.key.empty() || !profile.key.is_absolute()) {
        throw std::invalid_argument("account profile must contain an absolute key path");
    }
}

inline std::vector<std::filesystem::path> list_market_configs(const std::filesystem::path& config) {
    std::vector<std::filesystem::path> choices;
    const auto directory = config.has_parent_path() ? config.parent_path() : std::filesystem::path(".");
    if (std::filesystem::exists(directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".enc" && entry.is_regular_file()) {
                choices.push_back(entry.path());
            }
        }
    }
    std::sort(choices.begin(), choices.end());
    return choices;
}

inline std::optional<market_profile> load_market_profile(const std::filesystem::path& config) {
    const auto path = market_profile_file(config);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory
            || (!error && status.type() == std::filesystem::file_type::not_found)) {
        return std::nullopt;
    }
    if (error) { throw std::filesystem::filesystem_error("inspect account profile", path, error); }
    std::istringstream input(capture_detail::read_private_file(path, 8192));
    std::string format;
    market_profile profile;
    std::string key;
    if (!std::getline(input, format) || format != "XTM_PROFILE_1"
            || !(input >> std::quoted(profile.alias) >> std::quoted(key))
            || !(input >> std::ws).eof()) {
        throw std::runtime_error("invalid account profile: " + path.string());
    }
    profile.key = key;
    validate_market_profile(profile);
    return profile;
}

inline void save_market_profile(const std::filesystem::path& config, market_profile profile) {
    profile.key = std::filesystem::absolute(profile.key).lexically_normal();
    validate_market_profile(profile);
    if (!std::filesystem::is_regular_file(config) || !std::filesystem::is_regular_file(profile.key)
            || std::filesystem::equivalent(config, profile.key)) {
        throw std::invalid_argument("account configuration and key must be existing, separate files");
    }
    const auto path = market_profile_file(config);
    (void)load_market_profile(config);
    std::ostringstream output;
    output << "XTM_PROFILE_1\n" << std::quoted(profile.alias) << '\n'
           << std::quoted(profile.key.string()) << '\n';
    const auto contents = output.str();
    auto temporary_name = path.string() + ".XXXXXX";
    std::vector<char> temporary(temporary_name.begin(), temporary_name.end());
    temporary.push_back('\0');
    capture_detail::file_descriptor file(::mkstemp(temporary.data()));
    if (file.get() < 0) { capture_detail::io_error("create account profile"); }
    try {
        capture_detail::write_all(file.get(), contents.data(), contents.size());
        capture_detail::sync_file(file.get());
        std::filesystem::rename(temporary.data(), path);
    } catch (...) {
        if (::unlink(temporary.data()) != 0) {
            std::throw_with_nested(std::runtime_error("failed to save and clean up account profile"));
        }
        throw;
    }
}

inline std::string market_profile_label(const std::filesystem::path& config, std::ostream& errors) {
    try {
        const auto profile = load_market_profile(config);
        if (profile) { return profile->alias; }
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot read alias for " << config.string() << ": " << error.what() << '\n';
    }
    return config.stem().string();
}

inline void run_market_profile_alias_menu(
        const std::filesystem::path& config, const std::filesystem::path& key,
        std::istream& input, std::ostream& output, std::ostream& errors,
        bool remember_import = false) {
    try {
        const auto profile = load_market_profile(config);
        const auto current = profile ? profile->alias : config.stem().string();
        output << "Account alias [" << current << "] (Enter keeps current, u skips): " << std::flush;
        std::string alias;
        if (!std::getline(input, alias) || alias == "u") {
            if (!remember_import) { return; }
            alias.clear();
        }
        if (alias.empty()) { alias = current; }
        save_market_profile(config, {std::move(alias), key});
        output << "[OK] Account alias and matching key path saved; encrypted credentials unchanged.\n";
    } catch (const std::exception& error) {
        errors << "[ERROR] Cannot save account alias; configuration remains available at "
               << config.string() << ": " << error.what() << '\n';
    }
}

} // namespace seastar::xtrader::cli
