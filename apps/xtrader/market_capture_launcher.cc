// exec-style foreground launch; never passes credentials to a shell or argv.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include "market_capture_menu.hh"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <vector>

extern char** environ;

namespace seastar::xtrader::cli {
namespace {

std::filesystem::path default_path(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    return value && *value ? value : fallback;
}

struct selected_market_files {
    std::filesystem::path config = default_path("XTRADER_MARKET_CONFIG", "/etc/xtrader/market.enc");
    std::filesystem::path key = default_path("XTRADER_MARKET_KEY_FILE", "/etc/xtrader/market.key");
};

selected_market_files& current_files() {
    static selected_market_files files;
    return files;
}

int launch(const char* name, std::vector<std::string> arguments) {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path() / name;
    arguments.insert(arguments.begin(), executable.string());
    std::vector<char*> argv;
    for (auto& argument : arguments) { argv.push_back(argument.data()); }
    argv.push_back(nullptr);
    posix_spawnattr_t attributes;
    auto result = ::posix_spawnattr_init(&attributes);
    if (result != 0) { throw std::system_error(result, std::generic_category(), "prepare market process"); }
    sigset_t empty;
    sigset_t defaults;
    ::sigemptyset(&empty);
    ::sigemptyset(&defaults);
    ::sigaddset(&defaults, SIGINT);
    ::sigaddset(&defaults, SIGTERM);
    result = ::posix_spawnattr_setsigmask(&attributes, &empty);
    if (!result) { result = ::posix_spawnattr_setsigdefault(&attributes, &defaults); }
    if (!result) {
        result = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    }
    pid_t child = -1;
    if (!result) { result = ::posix_spawn(&child, executable.c_str(), nullptr, &attributes, argv.data(), environ); }
    ::posix_spawnattr_destroy(&attributes);
    if (result) {
        throw std::system_error(result, std::generic_category(), "start sibling market utility (build it first)");
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) { throw std::system_error(errno, std::generic_category(), "wait for market process"); }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

} // namespace

void open_market_capture_menu() {
    const auto& files = current_files();
    run_market_capture_menu(files.config, files.key, std::cin, std::cout, std::cerr,
        [] (const auto& config, const auto& key) {
            return launch("xtrader_market_capture", {"--config", config.string(), "--key-file", key.string()});
        });
}

void open_initial_ctp_key_menu() {
    auto& files = current_files();
    run_initial_ctp_key_menu(files.config, files.key, std::cin, std::cout, std::cerr,
        [] (const auto& config, const auto& key) {
            return launch("xtrader_market_config", {"--initialize", config.string(), key.string()});
        });
}

void open_market_config_selection() {
    auto& files = current_files();
    select_market_config(files.config, files.key, std::cin, std::cout, std::cerr);
}

void open_legacy_import_menu() {
    auto& files = current_files();
    run_legacy_import_menu(files.config, files.key, std::cin, std::cout, std::cerr,
        [] (const auto& config, const auto& key) {
            return launch("xtrader_market_config", {"--import-legacy", config.string(), key.string()});
        });
}

} // namespace seastar::xtrader::cli
