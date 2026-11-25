// Minimal append-only runtime logger for the xtrader command-line utilities.
// Writes /var/log/xtrader/<program>.log and never throws or blocks the menu UI.
// Source: user request, 2026-11-24. Signature: GitHub Copilot (Auto).
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace seastar::xtrader::log {

namespace detail {

struct state {
    std::mutex guard;
    std::FILE* stream = nullptr;
    std::string program;
    std::string path;
    bool initialized = false;
    bool warned = false;
    bool mirror = false;
};

inline state& state_instance() {
    static state instance;
    return instance;
}

// Tests may point XTRADER_LOG_DIR at a scratch directory; production uses /var/log/xtrader.
inline std::string log_directory() {
    if (const char* override_directory = std::getenv("XTRADER_LOG_DIR"); override_directory && *override_directory) {
        return override_directory;
    }
    return "/var/log/xtrader";
}

inline bool logging_disabled() {
    const char* disable = std::getenv("XTRADER_LOG_DISABLE");
    return disable != nullptr && std::strcmp(disable, "1") == 0;
}

// Creates the log directory once; an existing directory is not an error.
inline bool ensure_directory(const std::string& directory) {
    if (::mkdir(directory.c_str(), 0755) == 0) { return true; }
    return errno == EEXIST;
}

inline std::string timestamp() {
    struct timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    struct tm local{};
    ::localtime_r(&now.tv_sec, &local);
    char date[32];
    ::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &local);
    // %z keeps the server timezone readable without relying on glibc's tm_gmtoff extension.
    char zone[8] = "+0000";
    (void)::strftime(zone, sizeof zone, "%z", &local);
    char text[64];
    std::snprintf(text, sizeof text, "%s.%03ld %s", date, now.tv_nsec / 1000000, zone);
    return text;
}

inline void write_line(state& current, const char* level, std::string_view message) {
    std::lock_guard<std::mutex> lock(current.guard);
    if (current.stream == nullptr && !current.mirror) {
        if (!current.warned) {
            current.warned = true;
            std::fprintf(stderr, "[WARN] runtime log file unavailable; continuing with console output only\n");
        }
        return;
    }
    const auto line = timestamp() + " [" + level + "] pid=" + std::to_string(::getpid())
        + " " + std::string(message) + "\n";
    if (current.stream != nullptr) {
        (void)std::fwrite(line.data(), 1, line.size(), current.stream);
        (void)std::fflush(current.stream);
    }
    if (current.mirror) {
        // Daemon mode mirrors every line to stderr so systemd journald captures it.
        (void)std::fwrite(line.data(), 1, line.size(), stderr);
        (void)std::fflush(stderr);
    }
}

} // namespace detail

// Call once per process before the first log line; later calls are ignored.
// Failure to create or open the file only disables file logging, never the program.
inline void initialize(std::string_view program) {
    auto& current = detail::state_instance();
    std::lock_guard<std::mutex> lock(current.guard);
    if (current.initialized) { return; }
    current.initialized = true;
    if (detail::logging_disabled()) { return; }
    const auto directory = detail::log_directory();
    if (!detail::ensure_directory(directory)) {
        current.warned = true;
        std::fprintf(stderr, "[WARN] cannot create log directory %s: %s\n", directory.c_str(), std::strerror(errno));
        return;
    }
    current.program = std::string(program);
    current.path = directory + "/" + current.program + ".log";
    current.stream = std::fopen(current.path.c_str(), "ae");
    if (current.stream == nullptr) {
        current.warned = true;
        std::fprintf(stderr, "[WARN] cannot open log file %s: %s\n", current.path.c_str(), std::strerror(errno));
        return;
    }
    // Line buffered: a killed collector still keeps every line written before the signal.
    (void)std::setvbuf(current.stream, nullptr, _IOLBF, 0);
}

// Current log file path, or empty when file logging is disabled or unavailable.
inline std::string file_path() {
    auto& current = detail::state_instance();
    std::lock_guard<std::mutex> lock(current.guard);
    return current.path;
}

// Mirror every log line to stderr (journald capture); affects lines written
// after the call and never disables file logging.
inline void mirror_to_stderr(bool enable) {
    auto& current = detail::state_instance();
    std::lock_guard<std::mutex> lock(current.guard);
    current.mirror = enable;
}

inline void info(std::string_view message) { detail::write_line(detail::state_instance(), "INFO", message); }
inline void warning(std::string_view message) { detail::write_line(detail::state_instance(), "WARN", message); }
inline void error(std::string_view message) { detail::write_line(detail::state_instance(), "ERROR", message); }

} // namespace seastar::xtrader::log
