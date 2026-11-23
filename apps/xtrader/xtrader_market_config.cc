// Interactive config creation and explicit legacy INI import.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture_config.hh>
#include <seastar/xtrader/version.hh>
#include "cli_language.hh"

#include <iostream>
#include <openssl/crypto.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace {

using seastar::xtrader::cli::ui_text;

struct secret_text {
    std::string value;
    ~secret_text() { OPENSSL_cleanse(value.data(), value.size()); }
};

std::string prompt(const char* label) {
    std::cout << label << ": " << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) { throw std::runtime_error(ui_text("configuration input cancelled", "已取消配置输入")); }
    return value;
}

std::string password_prompt(const char* label = ui_text("CTP password (hidden)", "CTP 密码（隐藏输入）")) {
    if (!::isatty(STDIN_FILENO)) {
        throw std::runtime_error(ui_text("password entry requires a terminal; input redirection is not supported",
                                        "密码输入需要交互终端，不支持输入重定向"));
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
        throw std::runtime_error(ui_text("legacy import and plaintext review require an interactive terminal",
                                        "旧版导入和明文核对需要交互终端"));
    }
    const auto source = prompt(ui_text("Legacy X-Trader INI path (readable regular file)", "旧版 X-Trader INI 路径（可读取的普通文件）"));
    secret_text secret{password_prompt(ui_text("Legacy XTRADER_INI_SECRET (hidden)", "旧版 XTRADER_INI_SECRET（隐藏输入）"))};
    auto contracts = seastar::xtrader::parse_market_capture_contracts(
        prompt(ui_text("Concrete contracts, comma separated (EXCHANGE:instrument; no spaces)",
                       "具体合约，逗号分隔（EXCHANGE:instrument；不含空格）")));
    auto config = seastar::xtrader::import_legacy_market_config(source, secret.value, std::move(contracts));
    std::cout << ui_text("[WARN] Legacy XOR has no authentication. A wrong secret can produce plausible text.\n"
                         "The next step displays credentials on this terminal, including passwords. "
                         "Terminal scrollback or recording may retain them.\n",
                         "[WARN] 旧版 XOR 无认证校验，错误口令也可能生成看似正常的文本。\n"
                         "下一步会在终端显示凭据（含密码），终端回滚或录屏可能保存这些内容。\n");
    if (prompt(ui_text("Type SHOW to display the plaintext once", "输入 SHOW，仅显示一次明文")) != "SHOW") {
        throw std::runtime_error(ui_text("legacy import cancelled before plaintext review", "已取消旧版导入，未进行明文核对"));
    }
    const auto& trader = *config.trader;
    std::cout << ui_text("\n[Plaintext legacy configuration: displayed once]\n", "\n[旧版配置明文：仅显示一次]\n")
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
    if (prompt(ui_text("After independently checking the fields, type SAVE to encrypt and save",
                       "自行核对字段后，输入 SAVE 加密并保存")) != "SAVE") {
        throw std::runtime_error(ui_text("legacy import cancelled; no encrypted configuration created",
                                        "已取消旧版导入，未创建加密配置"));
    }
    seastar::xtrader::initialize_market_capture_config(config, output, key);
    std::cout << ui_text("[OK] Legacy configuration saved with authenticated encryption; trading remains disabled.\n",
                         "[OK] 已通过认证加密保存旧版配置，交易仍已禁用。\n");
}

void import_plaintext_config(const char* source, const char* output, const char* key) {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        throw std::runtime_error(ui_text("plaintext INI import requires an interactive terminal", "明文 INI 导入需要交互终端"));
    }
    const auto config = seastar::xtrader::import_plaintext_market_config(source);
    seastar::xtrader::initialize_market_capture_config(config, output, key);
    std::cout << ui_text("[OK] Plaintext INI imported into an encrypted configuration; credentials were not displayed.\n",
                         "[OK] 已将明文 INI 导入加密配置，未显示凭据。\n")
              << ui_text("[INFO] Encrypted configuration: ", "[INFO] 加密配置：") << output << '\n'
              << ui_text("[INFO] No contract was imported; choose concrete contracts when starting capture.\n",
                         "[INFO] 未导入合约，请在启动采集时选择具体合约。\n")
              << ui_text("[INFO] Trading remains disabled.\n", "[INFO] 交易仍已禁用。\n");
}

void create_config(const char* output, const char* key, bool initialize = false) {
    seastar::xtrader::market_capture_config config;
    config.market_front = prompt(ui_text("CTP market front (tcp://hostname:port)", "CTP 行情前置（tcp://hostname:port）"));
    config.broker_id = prompt("BrokerID");
    config.user_id = prompt("UserID");
    config.password = password_prompt();
    config.contracts = seastar::xtrader::parse_market_capture_contracts(
        prompt(ui_text("Concrete contracts, comma separated (EXCHANGE:instrument; no spaces)",
                       "具体合约，逗号分隔（EXCHANGE:instrument；不含空格）")));
    if (initialize) {
        seastar::xtrader::initialize_market_capture_config(config, output, key);
    } else {
        seastar::xtrader::save_market_capture_config(config, output, key);
    }
    std::cout << ui_text("[OK] Authenticated encrypted market configuration created; no connection attempted.\n",
                         "[OK] 已创建认证加密的行情配置，未尝试连接。\n");
}

} // namespace

int main(int argc, char** argv) {
    try {
        (void)seastar::xtrader::cli::current_ui_language();
        std::cout << "X-Trader Seastar Trading Engine v"
                  << seastar::xtrader::get_full_version() << '\n';
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "xtrader_market_config --new-key KEY_FILE\n"
                         "xtrader_market_config --create ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --initialize ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --import-legacy ENCRYPTED_CONFIG KEY_FILE\n"
                         "xtrader_market_config --import-plaintext SOURCE_INI ENCRYPTED_CONFIG KEY_FILE\n"
                      << ui_text("--initialize creates missing parent directories and creates/reuses an external key.\n"
                                 "Files are owner-only and never overwritten. Password entry requires a terminal.\n",
                                 "--initialize 创建缺失的父目录，并创建或复用外部密钥。\n"
                                 "输出文件仅供所有者访问，不覆盖已有文件。密码输入需要交互终端。\n");
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--new-key") {
            seastar::xtrader::create_market_capture_key(argv[2]);
            std::cout << ui_text("[OK] Random 256-bit key created. Protect it separately from the configuration.\n",
                                 "[OK] 已生成随机 256 位密钥，请与配置分开保护。\n");
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
        throw std::invalid_argument(ui_text("invalid arguments; use --help", "参数无效，请使用 --help 查看用法"));
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 2;
    }
}
