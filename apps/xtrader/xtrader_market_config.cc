// Interactive config creation and explicit legacy INI import.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture_config.hh>

#include <iostream>
#include <openssl/crypto.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace {

struct secret_text {
    std::string value;
    ~secret_text() { OPENSSL_cleanse(value.data(), value.size()); }
};

std::string prompt(const char* label) {
    std::cout << label << ": " << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) { throw std::runtime_error("configuration input cancelled"); }
    return value;
}

std::string password_prompt(const char* label = "CTP password (hidden)") {
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
        auto value = prompt(label);
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

void import_config(const char* output, const char* key) {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        throw std::runtime_error("legacy import and plaintext review require an interactive terminal");
    }
    const auto source = prompt("Legacy X-Trader INI path (owner-only file)");
    secret_text secret{password_prompt("Legacy XTRADER_INI_SECRET (hidden)")};
    auto contracts = seastar::xtrader::parse_market_capture_contracts(
        prompt("Concrete contracts, comma separated (EXCHANGE:instrument; no spaces)"));
    auto config = seastar::xtrader::import_legacy_market_config(source, secret.value, std::move(contracts));
    std::cout << "[WARN] Legacy XOR has no authentication. A wrong secret can produce plausible text.\n"
                 "The next step displays credentials on this terminal, including passwords. "
                 "Terminal scrollback or recording may retain them.\n";
    if (prompt("Type SHOW to display the plaintext once") != "SHOW") {
        throw std::runtime_error("legacy import cancelled before plaintext review");
    }
    const auto& trader = *config.trader;
    std::cout << "\n[Plaintext legacy configuration: displayed once]\n"
              << "market.market_front: " << config.market_front
              << "\nmarket.broker_id: " << config.broker_id
              << "\nmarket.user_id: " << config.user_id
              << "\nmarket.password: " << config.password
              << "\ntrader.trade_front: " << trader.trade_front
              << "\ntrader.broker_id: " << trader.broker_id
              << "\ntrader.user_id: " << trader.user_id
              << "\ntrader.password: " << trader.password
              << "\ntrader.app_id: " << trader.app_id
              << "\ntrader.auth_code: " << trader.auth_code
              << "\ntrader.user_product_Info: " << trader.user_product_info << "\n" << std::flush;
    if (prompt("After independently checking the fields, type SAVE to encrypt and save") != "SAVE") {
        throw std::runtime_error("legacy import cancelled; no encrypted configuration created");
    }
    seastar::xtrader::initialize_market_capture_config(config, output, key);
    std::cout << "[OK] Legacy configuration saved with authenticated encryption; trading remains disabled.\n";
}

void import_plaintext_config(const char* source, const char* output, const char* key) {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        throw std::runtime_error("plaintext INI import requires an interactive terminal");
    }
    const auto config = seastar::xtrader::import_plaintext_market_config(source);
    seastar::xtrader::initialize_market_capture_config(config, output, key);
    std::cout << "[OK] Plaintext INI imported into an encrypted configuration; credentials were not displayed.\n"
              << "[INFO] Encrypted configuration: " << output << '\n'
              << "[INFO] No contract was imported; choose concrete contracts when starting capture.\n"
              << "[INFO] Trading remains disabled.\n";
}

void create_config(const char* output, const char* key, bool initialize = false) {
    seastar::xtrader::market_capture_config config;
    config.market_front = prompt("CTP market front (tcp://hostname:port)");
    config.broker_id = prompt("BrokerID");
    config.user_id = prompt("UserID");
    config.password = password_prompt();
    config.contracts = seastar::xtrader::parse_market_capture_contracts(
        prompt("Concrete contracts, comma separated (EXCHANGE:instrument; no spaces)"));
    if (initialize) {
        seastar::xtrader::initialize_market_capture_config(config, output, key);
    } else {
        seastar::xtrader::save_market_capture_config(config, output, key);
    }
    std::cout << "[OK] Authenticated encrypted market configuration created; no connection attempted.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "xtrader_market_config --new-key KEY_FILE\n"
                         "xtrader_market_config --create ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --initialize ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --import-legacy ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --import-plaintext SOURCE_INI ENCRYPTED_CONFIG KEY_FILE\n"
                         "--initialize creates missing parent directories and creates/reuses an external key.\n"
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
        if (argc == 4 && std::string(argv[1]) == "--initialize") {
            create_config(argv[2], argv[3], true);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--import-legacy") {
            import_config(argv[2], argv[3]);
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--import-plaintext") {
            import_plaintext_config(argv[2], argv[3], argv[4]);
            return 0;
        }
        throw std::invalid_argument("invalid arguments; use --help");
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 2;
    }
}
