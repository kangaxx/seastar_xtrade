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

extern char** environ;

namespace seastar::xtrader::cli {
namespace {

std::filesystem::path default_path(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    return value && *value ? value : fallback;
}

int launch(const std::filesystem::path& config, const std::filesystem::path& key) {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "xtrader_market_capture";
    std::string program = executable.string();
    std::string config_argument = config.string();
    std::string key_argument = key.string();
    char config_option[] = "--config";
    char key_option[] = "--key-file";
    char* arguments[]{program.data(), config_option, config_argument.data(), key_option, key_argument.data(), nullptr};
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
    if (!result) { result = ::posix_spawn(&child, program.c_str(), nullptr, &attributes, arguments, environ); }
    ::posix_spawnattr_destroy(&attributes);
    if (result) {
        throw std::system_error(result, std::generic_category(), "start sibling xtrader_market_capture (build it first)");
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) { throw std::system_error(errno, std::generic_category(), "wait for market process"); }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

} // namespace

void open_market_capture_menu() {
    run_market_capture_menu(default_path("XTRADER_MARKET_CONFIG", "/etc/xtrader/market.enc"),
        default_path("XTRADER_MARKET_KEY_FILE", "/etc/xtrader/market.key"),
        std::cin, std::cout, std::cerr, launch);
}

} // namespace seastar::xtrader::cli
