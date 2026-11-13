// Interactive config creation: no plaintext file, command-line password or legacy XOR.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture_config.hh>

#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <termios.h>
#include <unistd.h>

namespace {

std::string prompt(const char* label) {
    std::cout << label << ": " << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) { throw std::runtime_error("configuration input cancelled"); }
    return value;
}

std::string password_prompt() {
    if (!::isatty(STDIN_FILENO)) {
        throw std::runtime_error("password entry requires a terminal; input redirection is not supported");
    }
    struct termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) {
        throw std::system_error(errno, std::generic_category(), "read terminal settings");
    }
    auto hidden = original;
    hidden.c_lflag &= ~ECHO;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        throw std::system_error(errno, std::generic_category(), "disable password echo");
    }
    try {
        auto value = prompt("CTP password (hidden)");
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original) != 0) {
            throw std::system_error(errno, std::generic_category(), "restore terminal echo");
        }
        std::cout << '\n';
        return value;
    } catch (...) {
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original) != 0) {
            std::throw_with_nested(std::runtime_error("failed to restore terminal echo; run stty echo"));
        }
        throw;
    }
}

void create_config(const char* output, const char* key) {
    seastar::xtrader::market_capture_config config;
    config.market_front = prompt("CTP market front (tcp://hostname:port)");
    config.broker_id = prompt("BrokerID");
    config.user_id = prompt("UserID");
    config.password = password_prompt();
    config.contracts = seastar::xtrader::parse_market_capture_contracts(
        prompt("Concrete contracts, comma separated (EXCHANGE:instrument; no spaces)"));
    seastar::xtrader::save_market_capture_config(config, output, key);
    std::cout << "[OK] Authenticated encrypted market configuration created; no connection attempted.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "xtrader_market_config --new-key KEY_FILE\n"
                         "xtrader_market_config --create ENCRYPTED_CONFIG KEY_FILE\n"
                         "Files are owner-only and never overwritten. Password entry requires a terminal.\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--new-key") {
            seastar::xtrader::create_market_capture_key(argv[2]);
            std::cout << "[OK] Random 256-bit key created. Protect it separately from the configuration.\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--create") {
            create_config(argv[2], argv[3]);
            return 0;
        }
        throw std::invalid_argument("invalid arguments; use --help");
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 2;
    }
}
