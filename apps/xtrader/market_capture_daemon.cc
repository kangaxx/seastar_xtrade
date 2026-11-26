#include "market_capture_daemon.hh"
#include "market_capture_runtime.hh"
#include "xtrader_log.hh"

#include <seastar/xtrader/market_capture.hh>
#include <seastar/xtrader/version.hh>
#include <ThostFtdcMdApi.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace seastar::xtrader::cli {
namespace {

namespace xlog = seastar::xtrader::log;

using steady_clock = std::chrono::steady_clock;

constexpr int EXIT_OK = 0;
constexpr int EXIT_USAGE = 1;
constexpr int EXIT_RUNTIME = 2;
constexpr int EXIT_STALE = 3;
constexpr int EXIT_LOCKED = 10;

int64_t epoch_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

struct daemon_options {
    std::string config_path = env_or("XTRADER_MARKET_CONFIG", "/etc/xtrader/market.enc");
    std::string key_path = env_or("XTRADER_MARKET_KEY_FILE", "/etc/xtrader/market.key");
    std::string pid_path = env_or("XTRADER_CTP_MD_PID", "/run/xtrader/ctp-md.pid");
    long tick_stale_secs = 0;
    bool help = false;
};

daemon_options parse_daemon_options(int argc, char** argv) {
    daemon_options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&] (const char* flag) -> std::string {
            if (index + 1 >= argc) { throw std::invalid_argument(std::string(flag) + " requires a value"); }
            return argv[++index];
        };
        if (argument == "--daemon-tick") { continue; }
        if (argument == "--help" || argument == "-h") { options.help = true; }
        else if (argument == "--config") { options.config_path = value("--config"); }
        else if (argument == "--key-file") { options.key_path = value("--key-file"); }
        else if (argument == "--pid-file") { options.pid_path = value("--pid-file"); }
        else if (argument == "--tick-stale-secs") { options.tick_stale_secs = std::stol(value("--tick-stale-secs")); }
        else { throw std::invalid_argument("unknown daemon argument: " + argument); }
    }
    if (const char* stale = std::getenv("XTRADER_TICK_STALE_SECS"); stale && *stale) {
        options.tick_stale_secs = std::stol(stale);
    }
    return options;
}

void print_daemon_usage(std::ostream& output) {
    output << "usage: xtrader_market_capture --daemon-tick\n"
              "           [--config ENCRYPTED_CONFIG] [--key-file KEY_FILE]\n"
              "           [--pid-file PID_FILE] [--tick-stale-secs SECONDS]\n"
              "env: XTRADER_MARKET_CONFIG (default /etc/xtrader/market.enc)\n"
              "     XTRADER_MARKET_KEY_FILE (default /etc/xtrader/market.key)\n"
              "     XTRADER_CTP_MD_PID (default /run/xtrader/ctp-md.pid)\n"
              "     XTRADER_TICK_STALE_SECS (0 disables the in-process feed watchdog)\n"
              "     XTRADER_MARKET_STATE_DIR (state/flow/status directory)\n"
              "exit: 0 clean stop, 1 usage, 2 runtime failure, 3 stale feed, 10 pid lock held\n";
}

class already_running_error : public std::runtime_error {
public:
    explicit already_running_error(const std::string& message) : std::runtime_error(message) {}
};

class pid_file_lock {
public:
    explicit pid_file_lock(std::filesystem::path path) : _path(std::move(path)) {
        _fd = ::open(_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open pid file " + _path.string());
        }
        struct stat status{};
        if (::fstat(_fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
            throw std::runtime_error("pid file must be a regular file without hard links: " + _path.string());
        }
        if (::flock(_fd, LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK) {
                throw std::system_error(errno, std::generic_category(), "lock pid file " + _path.string());
            }
            throw already_running_error("pid lock held by another instance" + owner_text());
        }
        _locked = true;
        write_pid();
    }

    ~pid_file_lock() {
        // Unlink while still holding the lock, then close: a concurrent starter
        // can only see EWOULDBLOCK before this point, so no orphan-inode race.
        if (_locked) { (void)::unlink(_path.c_str()); }
        if (_fd >= 0) { ::close(_fd); }
    }

private:
    std::string owner_text() {
        char text[32]{};
        const auto count = ::pread(_fd, text, sizeof(text) - 1, 0);
        const pid_t owner = count > 0 ? static_cast<pid_t>(std::strtol(text, nullptr, 10)) : 0;
        if (owner > 1) {
            errno = 0;
            if (::kill(owner, 0) == 0 || errno == EPERM) { return " (pid " + std::to_string(owner) + ")"; }
        }
        return " (stale pid file content)";
    }

    void write_pid() {
        const auto text = std::to_string(::getpid()) + "\n";
        if (::ftruncate(_fd, 0) != 0) {
            throw std::system_error(errno, std::generic_category(), "truncate pid file " + _path.string());
        }
        if (::pwrite(_fd, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size())) {
            throw std::system_error(errno, std::generic_category(), "write pid file " + _path.string());
        }
    }

    std::filesystem::path _path;
    int _fd = -1;
    bool _locked = false;
};

// Key=value snapshot consumed by the systemd health check; atomic tmp+rename.
class status_file {
public:
    explicit status_file(std::filesystem::path path)
        : _path(std::move(path)), _temporary(_path.string() + ".tmp") {}

    void write(std::string_view state, bool ready, uint64_t ticks_written,
               int64_t last_tick_ms, int64_t started_ms) {
        std::string body;
        body.reserve(256);
        body += "pid=" + std::to_string(::getpid()) + "\n";
        body += "state=" + std::string(state) + "\n";
        body += "ready=" + std::to_string(ready ? 1 : 0) + "\n";
        body += "ticks_written=" + std::to_string(ticks_written) + "\n";
        body += "last_tick_ms=" + std::to_string(last_tick_ms) + "\n";
        body += "last_activity_ms=" + std::to_string(epoch_millis()) + "\n";
        body += "started_ms=" + std::to_string(started_ms) + "\n";
        const int fd = ::open(_temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open daemon status file");
        }
        try {
            std::size_t offset = 0;
            while (offset < body.size()) {
                const auto count = ::write(fd, body.data() + offset, body.size() - offset);
                if (count < 0 && errno == EINTR) { continue; }
                if (count < 0) {
                    throw std::system_error(errno, std::generic_category(), "write daemon status file");
                }
                offset += static_cast<std::size_t>(count);
            }
            if (::fsync(fd) != 0) {
                throw std::system_error(errno, std::generic_category(), "sync daemon status file");
            }
            ::close(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
        if (::rename(_temporary.c_str(), _path.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "publish daemon status file");
        }
    }

private:
    std::filesystem::path _path;
    std::filesystem::path _temporary;
};

int run_daemon_loop(const daemon_options& options) {
    auto config = seastar::xtrader::load_market_capture_config(options.config_path, options.key_path);
    seastar::xtrader::validate_market_capture_config(config);
    if (config.contracts.empty()) {
        throw std::runtime_error("daemon mode requires configured contracts; "
                                 "run the interactive setup (xtrader_cli) first");
    }
    xlog::info("daemon config=" + options.config_path + " front=" + config.market_front
        + " contracts=" + std::to_string(config.contracts.size()));

    const seastar::xtrader::market_data_paths data_paths;
    const auto state_directory = seastar::xtrader::cli::state_directory();
    status_file status(state_directory / "daemon.status");
    const auto started_ms = epoch_millis();

    seastar::xtrader::market_capture_session session(
        std::move(config), data_paths, state_directory,
        [] (const char* flow) { return CThostFtdcMdApi::CreateFtdcMdApi(flow, false, false); });
    session.start();
    xlog::info("daemon session started; waiting for connect/login/subscribe");

    auto next_status = steady_clock::now();
    auto next_heartbeat = next_status + std::chrono::seconds(60);
    auto last_tick_time = next_status;
    uint64_t last_ticks = 0;
    int64_t last_tick_ms = 0;
    bool saw_tick = false;

    while (!stop_requested()) {
        session.poll_for(std::chrono::milliseconds(100));
        const auto now = steady_clock::now();
        const uint64_t ticks = session.ticks_written();
        if (ticks != last_ticks) {
            last_ticks = ticks;
            last_tick_time = now;
            last_tick_ms = epoch_millis();
            saw_tick = true;
        }
        if (now >= next_status) {
            status.write(session.is_ready() ? "ready" : "connecting", session.is_ready(),
                         ticks, last_tick_ms, started_ms);
            next_status = now + std::chrono::seconds(5);
        }
        if (now >= next_heartbeat) {
            xlog::info(std::string("heartbeat ready=") + (session.is_ready() ? "1" : "0")
                + " ticks_written=" + std::to_string(ticks));
            next_heartbeat = now + std::chrono::seconds(60);
        }
        if (options.tick_stale_secs > 0 && saw_tick && session.is_ready()
                && now - last_tick_time >= std::chrono::seconds(options.tick_stale_secs)) {
            xlog::error("tick feed stale for " + std::to_string(options.tick_stale_secs)
                + "s while ready; exiting for restart");
            return EXIT_STALE;
        }
    }

    status.write("stopping", session.is_ready(), session.ticks_written(), last_tick_ms, started_ms);
    session.stop();
    xlog::info("daemon stopped ticks_written=" + std::to_string(session.ticks_written()));
    return EXIT_OK;
}

} // namespace

int run_market_capture_daemon(int argc, char** argv) {
    xlog::initialize("xtrader_market_capture");
    xlog::mirror_to_stderr(true);
    daemon_options options;
    try {
        options = parse_daemon_options(argc, argv);
    } catch (const std::exception& error) {
        xlog::error(std::string("daemon argument error: ") + error.what());
        print_daemon_usage(std::cerr);
        return EXIT_USAGE;
    }
    if (options.help) {
        print_daemon_usage(std::cout);
        return EXIT_OK;
    }
    try {
        install_signals();
        xlog::info("daemon start version=" + std::string(seastar::xtrader::get_full_version()));
        pid_file_lock lock(options.pid_path);
        return run_daemon_loop(options);
    } catch (const already_running_error& error) {
        xlog::error(error.what());
        return EXIT_LOCKED;
    } catch (const std::exception& error) {
        xlog::error(std::string("daemon failed: ") + error.what());
        return EXIT_RUNTIME;
    }
}

} // namespace seastar::xtrader::cli
