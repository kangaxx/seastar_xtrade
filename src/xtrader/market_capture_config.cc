// AES-256-GCM envelope; deliberately rejects legacy XOR/plaintext configurations.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#include <seastar/xtrader/market_capture_config.hh>
#include <seastar/xtrader/market_data_paths.hh>
#include "market_capture_io.hh"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>

namespace seastar::xtrader {
namespace {

constexpr std::string_view MAGIC = "XTMDC001";
constexpr std::size_t NONCE_SIZE = 12;
constexpr std::size_t TAG_SIZE = 16;
constexpr std::size_t MAX_CONFIG_SIZE = 65536;

struct secret_text {
    std::string value;
    ~secret_text() { OPENSSL_cleanse(value.data(), value.size()); }
};

void require_crypto(bool condition) {
    if (!condition) {
        throw std::runtime_error("market configuration cryptographic operation failed");
    }
}

void require_text(const std::string& text, std::size_t maximum, const char* field, bool spaces = false) {
    if (text.empty() || text.size() > maximum
            || !std::all_of(text.begin(), text.end(), [spaces] (unsigned char ch) {
                return ch >= (spaces ? 32 : 33) && ch <= 126;
            })) {
        throw std::invalid_argument(std::string("invalid market configuration field: ") + field);
    }
}

secret_text read_key(const std::filesystem::path& path) {
    secret_text key{capture_detail::read_private_file(path, 32)};
    if (key.value.size() != 32) {
        throw std::runtime_error("market encryption key must contain exactly 32 raw bytes");
    }
    return key;
}

std::string serialize(const market_capture_config& config) {
    std::string text = "broker_id=" + config.broker_id + "\nuser_id=" + config.user_id
        + "\npassword=" + config.password + "\nmarket_front=" + config.market_front + "\ncontracts=";
    for (const auto& contract : config.contracts) {
        text += contract.exchange + ":" + contract.instrument + ",";
    }
    text.pop_back();
    return text + "\n";
}

market_capture_config parse(const std::string& text) {
    std::map<std::string, std::string_view> fields;
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        const auto separator = line.find('=');
        if (separator == std::string::npos
                || !fields.emplace(std::string(line.substr(0, separator)), line.substr(separator + 1)).second) {
            throw std::invalid_argument("invalid or duplicate encrypted market configuration field");
        }
        remaining = newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
    }
    if (fields.size() != 5 || !fields.contains("broker_id") || !fields.contains("user_id")
            || !fields.contains("password") || !fields.contains("market_front") || !fields.contains("contracts")) {
        throw std::invalid_argument("encrypted market configuration requires exactly five supported fields");
    }
    market_capture_config config;
    config.broker_id = fields.at("broker_id");
    config.user_id = fields.at("user_id");
    config.password = fields.at("password");
    config.market_front = fields.at("market_front");
    config.contracts = parse_market_capture_contracts(std::string(fields.at("contracts")));
    validate_market_capture_config(config);
    return config;
}

using cipher_context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

cipher_context new_context() {
    cipher_context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    require_crypto(static_cast<bool>(context));
    return context;
}

} // namespace

market_capture_config::~market_capture_config() {
    for (auto* text : {&broker_id, &user_id, &password}) {
        OPENSSL_cleanse(text->data(), text->size());
    }
}

std::vector<market_capture_contract> parse_market_capture_contracts(const std::string& text) {
    if (text.empty() || text.back() == ',') {
        throw std::invalid_argument("contracts must be nonempty EXCHANGE:instrument pairs");
    }
    std::vector<market_capture_contract> contracts;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ',')) {
        const auto separator = item.find(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == item.size()) {
            throw std::invalid_argument("contracts must be EXCHANGE:instrument pairs");
        }
        contracts.push_back({item.substr(0, separator), item.substr(separator + 1)});
    }
    return contracts;
}

void validate_market_capture_config(const market_capture_config& config) {
    require_text(config.broker_id, 10, "broker_id");
    require_text(config.user_id, 15, "user_id");
    require_text(config.password, 40, "password", true);
    require_text(config.market_front, 255, "market_front");
    const auto separator = config.market_front.rfind(':');
    const auto host = config.market_front.size() >= 6
        ? config.market_front.substr(6, separator > 6 ? separator - 6 : 0) : std::string{};
    unsigned port = 0;
    if (!config.market_front.starts_with("tcp://") || separator == std::string::npos || host.empty()
            || !std::all_of(host.begin(), host.end(), [] (unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
            })) {
        throw std::invalid_argument("market_front must be tcp://hostname:port (IPv4 or DNS)");
    }
    const auto begin = config.market_front.data() + separator + 1;
    const auto end = config.market_front.data() + config.market_front.size();
    const auto parsed = std::from_chars(begin, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535) {
        throw std::invalid_argument("market_front requires a valid TCP port");
    }
    if (config.contracts.empty() || config.contracts.size() > 64) {
        throw std::invalid_argument("configure between 1 and 64 concrete futures contracts");
    }
    const std::set<std::string> exchanges{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
    std::set<std::string> instruments;
    const market_data_paths paths;
    for (const auto& contract : config.contracts) {
        if (!exchanges.contains(contract.exchange) || contract.instrument.size() > 80
                || !instruments.insert(contract.instrument).second) {
            throw std::invalid_argument("invalid exchange, duplicate instrument or instrument too long");
        }
        (void)paths.dataset_directory({"x_trader_data", "futures", contract.exchange, contract.instrument, "tick"});
    }
}

void create_market_capture_key(const std::filesystem::path& path) {
    secret_text key{std::string(32, '\0')};
    require_crypto(RAND_bytes(reinterpret_cast<unsigned char*>(key.value.data()), 32) == 1);
    capture_detail::create_private_file(path, key.value);
}

void save_market_capture_config(const market_capture_config& config,
                               const std::filesystem::path& encrypted_file,
                               const std::filesystem::path& key_file) {
    validate_market_capture_config(config);
    auto key = read_key(key_file);
    secret_text plaintext{serialize(config)};
    auto context = new_context();
    std::array<unsigned char, NONCE_SIZE> nonce{};
    require_crypto(RAND_bytes(nonce.data(), nonce.size()) == 1);
    require_crypto(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char*>(key.value.data()), nonce.data()) == 1);
    int count = 0;
    require_crypto(EVP_EncryptUpdate(context.get(), nullptr, &count,
        reinterpret_cast<const unsigned char*>(MAGIC.data()), MAGIC.size()) == 1);
    std::string ciphertext(plaintext.value.size() + 16, '\0');
    require_crypto(EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &count,
        reinterpret_cast<const unsigned char*>(plaintext.value.data()), plaintext.value.size()) == 1);
    const auto size = count;
    require_crypto(EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + size, &count) == 1);
    ciphertext.resize(static_cast<std::size_t>(size + count));
    std::array<unsigned char, TAG_SIZE> tag{};
    require_crypto(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag.data()) == 1);
    std::string envelope(MAGIC);
    envelope.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    envelope.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    envelope += ciphertext;
    capture_detail::create_private_file(encrypted_file, envelope);
}

market_capture_config load_market_capture_config(const std::filesystem::path& encrypted_file,
                                                 const std::filesystem::path& key_file) {
    auto key = read_key(key_file);
    auto envelope = capture_detail::read_private_file(encrypted_file, MAX_CONFIG_SIZE);
    const auto header_size = MAGIC.size() + NONCE_SIZE + TAG_SIZE;
    if (envelope.size() <= header_size || !envelope.starts_with(MAGIC)) {
        throw std::runtime_error("unsupported encrypted market configuration; plaintext and legacy XOR are rejected");
    }
    auto context = new_context();
    auto* bytes = reinterpret_cast<unsigned char*>(envelope.data());
    require_crypto(EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char*>(key.value.data()), bytes + MAGIC.size()) == 1);
    int count = 0;
    require_crypto(EVP_DecryptUpdate(context.get(), nullptr, &count, bytes, MAGIC.size()) == 1);
    secret_text plaintext{std::string(envelope.size(), '\0')};
    require_crypto(EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plaintext.value.data()), &count,
        bytes + header_size, envelope.size() - header_size) == 1);
    const auto size = count;
    require_crypto(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
        bytes + MAGIC.size() + NONCE_SIZE) == 1);
    if (EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(plaintext.value.data()) + size, &count) != 1) {
        throw std::runtime_error("market configuration authentication failed (wrong key or modified ciphertext)");
    }
    plaintext.value.resize(static_cast<std::size_t>(size + count));
    return parse(plaintext.value);
}

} // namespace seastar::xtrader
