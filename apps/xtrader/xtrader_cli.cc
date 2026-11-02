/*
 * X-Trader CLI - Main entry point for X-Trader Seastar trading engine
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/xtrader/data_importer.hh>
#include <seastar/xtrader/historical_data_manager.hh>
#include <seastar/xtrader/redis_sync_client.hh>
#include <seastar/xtrader/version.hh>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string to_lower(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [] (unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

// ============================================================
// Config: 环境配置根目录 & 加密常量
// ============================================================
const std::string CONFIG_ROOT = "/etc/xtrader";
const std::vector<std::string> DEFAULT_ENVS = {"guangfa01", "simu01"};
const size_t MASTER_KEY_SIZE = 32;      // 256-bit
const size_t SALT_SIZE = 16;            // 128-bit per-field
const std::string ENC_PREFIX = "ENCv1:"; // 密文标记前缀

// ============================================================
// 编译期 Pepper：编译时注入，不同构建环境可替换
//   - 换 Pepper 会导致所有已加密数据不可读（密钥轮换效果）
//   - 不应提交到公开仓库
// ============================================================
const std::string COMPILE_TIME_PEPPER = "X-Trader::v0.1.2::salt-";

// ============================================================
// 判断字段是否需要加密（按字段名识别敏感字段）
// ============================================================
bool is_sensitive_field(const std::string& key) {
    std::string lower = to_lower(key);
    return lower.find("password")  != std::string::npos
        || lower.find("secret")   != std::string::npos
        || lower.find("investor") != std::string::npos
        || lower.find("auth")     != std::string::npos
        || lower.find("token")    != std::string::npos;
}

// ============================================================
// Base64 编解码（C++ 标准库实现，无外部依赖）
// ============================================================
const std::string BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16);
        if (i + 1 < len) chunk |= (static_cast<uint32_t>(data[i + 1]) << 8);
        if (i + 2 < len) chunk |= static_cast<uint32_t>(data[i + 2]);
        result += BASE64_CHARS[(chunk >> 18) & 0x3F];
        result += BASE64_CHARS[(chunk >> 12) & 0x3F];
        result += (i + 1 < len) ? BASE64_CHARS[(chunk >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? BASE64_CHARS[chunk & 0x3F] : '=';
    }
    return result;
}

std::vector<uint8_t> base64_decode(const std::string& input) {
    std::vector<uint8_t> result;
    auto index_of = [] (char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    size_t i = 0;
    while (i < input.size()) {
        int a = (i < input.size()) ? index_of(input[i++]) : -1;
        int b = (i < input.size()) ? index_of(input[i++]) : -1;
        int c = (i < input.size()) ? index_of(input[i++]) : -1;
        int d = (i < input.size()) ? index_of(input[i++]) : -1;
        if (a == -1) break;
        result.push_back(static_cast<uint8_t>((a << 2) | ((b & 0x30) >> 4)));
        if (b == -1 || input[i-3] == '=') break;
        result.push_back(static_cast<uint8_t>(((b & 0x0F) << 4) | ((c & 0x3C) >> 2)));
        if (c == -1 || input[i-2] == '=') break;
        result.push_back(static_cast<uint8_t>(((c & 0x03) << 6) | d));
    }
    return result;
}

// ============================================================
// 简单的 FNV-1a 64-bit hash（用于密钥派生混合）
// ============================================================
uint64_t fnv1a_64(const uint8_t* data, size_t len, uint64_t seed = 0xcbf29ce484222325ULL) {
    const uint64_t PRIME = 0x100000001b3ULL;
    uint64_t hash = seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= PRIME;
    }
    return hash;
}

// ============================================================
// HMAC-style 迭代 hash：从 seed 材料派生出 256-bit 密钥
//   - 使用 FNV-1a + 计数器迭代，模拟 KDF 行为
//   - 输入: seed_bytes + pepper + counter
// ============================================================
std::vector<uint8_t> derive_key_256(const std::vector<uint8_t>& seed, const std::string& pepper) {
    std::vector<uint8_t> key(MASTER_KEY_SIZE);
    std::vector<uint8_t> pepper_bytes(pepper.begin(), pepper.end());
    size_t offset = 0;

    for (uint32_t round = 0; round < 4 && offset < MASTER_KEY_SIZE; ++round) {
        // 每轮 hash: seed + pepper + round_byte
        uint64_t h = fnv1a_64(seed.data(), seed.size(), 0x6c62272e07bb0142ULL);
        h = fnv1a_64(pepper_bytes.data(), pepper_bytes.size(), h);
        h = fnv1a_64(reinterpret_cast<const uint8_t*>(&round), sizeof(round), h);

        // 再 hash 一遍加大扩散
        h = fnv1a_64(reinterpret_cast<const uint8_t*>(&h), sizeof(h),
                     0x62b821756295c58dULL);

        // 将 64-bit 填充到 key
        for (int i = 0; i < 8 && offset < MASTER_KEY_SIZE; ++i, ++offset) {
            key[offset] = static_cast<uint8_t>((h >> (i * 8)) & 0xFF);
        }
    }
    return key;
}

// ============================================================
// 读取 /etc/machine-id（系统唯一标识符）
//   返回 32 字符 Hex -> 16 字节二进制
// ============================================================
std::vector<uint8_t> read_machine_id() {
    std::vector<uint8_t> mid;
    std::ifstream file("/etc/machine-id");
    if (!file.is_open()) {
        // 备选：/var/lib/dbus/machine-id
        file.open("/var/lib/dbus/machine-id");
    }
    if (!file.is_open()) return mid;

    std::string hex;
    std::getline(file, hex);
    // 去掉末尾换行和空白
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.back()))) {
        hex.pop_back();
    }
    if (hex.size() < 32) return mid;

    mid.reserve(16);
    for (size_t i = 0; i + 1 < hex.size() && mid.size() < 16; i += 2) {
        unsigned int byte;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> byte;
        mid.push_back(static_cast<uint8_t>(byte));
    }
    return mid;
}

// ============================================================
// 主密钥派生（无文件存储方案）
//
//   密钥来源优先级：
//     1) 环境变量 XTRADER_KEY（Hex 64 字符）→ 直接作为 256-bit key
//     2) /etc/machine-id   + 编译期 Pepper → derive_key_256()
//     3) 兜底：random_device 随机（仅限临时使用，每次启动 key 不同）
//
//   安全性：
//     - machine-id 绑定：密钥与机器绑定，拷贝配置文件到其他机器无法解密
//     - Pepper 绑定：不同构建的 Pepper 不同，无法跨版本解密
//     - 无密钥文件：不存在 .master_key 被窃取的风险
//     - 环境变量：容器/CI 场景可用 XTRADER_KEY 显式注入
// ============================================================
std::vector<uint8_t> load_master_key() {
    // 优先级 1：环境变量
    const char* env_key = std::getenv("XTRADER_KEY");
    if (env_key && std::strlen(env_key) == MASTER_KEY_SIZE * 2) {
        std::vector<uint8_t> key(MASTER_KEY_SIZE);
        for (size_t i = 0; i < MASTER_KEY_SIZE; ++i) {
            unsigned int byte;
            std::stringstream ss;
            ss << std::hex << env_key[i * 2] << env_key[i * 2 + 1];
            ss >> byte;
            key[i] = static_cast<uint8_t>(byte);
        }
        return key;
    }

    // 优先级 2：machine-id + pepper 派生
    auto machine_id = read_machine_id();
    if (!machine_id.empty()) {
        return derive_key_256(machine_id, COMPILE_TIME_PEPPER);
    }

    // 优先级 3：兜底随机生成（仅本次运行有效，重启后无法解密旧配置）
    std::cerr << "[WARN] No XTRADER_KEY set and no /etc/machine-id found.\n";
    std::cerr << "[WARN] Using ephemeral random key -- encrypted configs from\n";
    std::cerr << "[WARN] previous runs will NOT be decryptable!\n";

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::vector<uint8_t> key(MASTER_KEY_SIZE);
    for (size_t i = 0; i < MASTER_KEY_SIZE; ++i) {
        key[i] = dist(gen);
    }
    return key;
}

// ============================================================
// 生成随机 salt
// ============================================================
std::vector<uint8_t> generate_salt() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::vector<uint8_t> salt(SALT_SIZE);
    for (size_t i = 0; i < SALT_SIZE; ++i) salt[i] = dist(gen);
    return salt;
}

// ============================================================
// XOR 流加密 / 解密（对称操作）
//   - 对每个字节: result[i] = plain[i] ^ master_key[i % MK] ^ salt[i % SS]
//   - 解密也走同一函数（XOR 自反）
// ============================================================
std::vector<uint8_t> xor_cipher(
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& master_key,
    const std::vector<uint8_t>& salt)
{
    std::vector<uint8_t> result(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t k = master_key[i % master_key.size()];
        uint8_t s = salt[i % salt.size()];
        result[i] = data[i] ^ k ^ s;
    }
    return result;
}

// ============================================================
// 加密明文值 → "ENCv1:<base64_salt>:<base64_ciphertext>"
// ============================================================
std::string encrypt_value(const std::string& plaintext) {
    if (plaintext.empty()) return plaintext;

    auto master_key = load_master_key();
    auto salt = generate_salt();

    std::vector<uint8_t> plain_bytes(plaintext.begin(), plaintext.end());
    auto cipher_bytes = xor_cipher(plain_bytes, master_key, salt);

    return ENC_PREFIX
         + base64_encode(salt.data(), salt.size())
         + ":"
         + base64_encode(cipher_bytes.data(), cipher_bytes.size());
}

// ============================================================
// 解密 "ENCv1:<base64_salt>:<base64_ciphertext>" → 明文
//   - 不以 ENC_PREFIX 开头的值直接返回原值
// ============================================================
std::string decrypt_value(const std::string& encoded) {
    if (encoded.size() < ENC_PREFIX.size()
        || encoded.substr(0, ENC_PREFIX.size()) != ENC_PREFIX) {
        return encoded; // 未加密，透传
    }

    std::string payload = encoded.substr(ENC_PREFIX.size());
    size_t sep = payload.find(':');
    if (sep == std::string::npos) return encoded;

    std::string salt_b64 = payload.substr(0, sep);
    std::string cipher_b64 = payload.substr(sep + 1);

    auto salt = base64_decode(salt_b64);
    auto cipher_bytes = base64_decode(cipher_b64);

    auto master_key = load_master_key();
    auto plain_bytes = xor_cipher(cipher_bytes, master_key, salt);

    return std::string(plain_bytes.begin(), plain_bytes.end());
}

// ============================================================
// 读取 key=value 配置文件，返回 map（自动解密）
// ============================================================
std::map<std::string, std::string> read_config_file(const std::filesystem::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    if (!file.is_open()) return values;

    std::string line;
    while (std::getline(file, line)) {
        // trim
        size_t begin = 0;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
        size_t end = line.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1]))) --end;
        std::string trimmed = line.substr(begin, end - begin);

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        size_t pos = trimmed.find('=');
        if (pos == std::string::npos) continue;

        std::string key = trimmed.substr(0, pos);
        std::string val = trimmed.substr(pos + 1);
        // trim key & val
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        size_t vstart = 0;
        while (vstart < val.size() && std::isspace(static_cast<unsigned char>(val[vstart]))) ++vstart;
        val = val.substr(vstart);

        // 自动解密
        values[key] = decrypt_value(val);
    }
    return values;
}

// ============================================================
// 写入 key=value 配置文件（敏感字段自动加密）
//   - 非敏感字段：明文直接写入
//   - 敏感字段 + 非空：加密后写入
// ============================================================
bool write_config_file(const std::filesystem::path& path, const std::map<std::string, std::string>& values) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::cerr << "[ERROR] Failed to create directory: " << path.parent_path() << std::endl;
        return false;
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to write config: " << path << std::endl;
        return false;
    }

    // 写入配置头注释
    file << "# X-Trader Account Configuration\n";
    file << "# Environment: " << path.parent_path().filename().string() << "\n";
    file << "# Generated by X-Trader CLI\n";
    file << "# Sensitive fields are encrypted with ENCv1 scheme\n\n";

    for (const auto& kv : values) {
        // 已加密的值跳过二次加密，空值跳过
        if (kv.second.empty() || kv.second.compare(0, ENC_PREFIX.size(), ENC_PREFIX) == 0) {
            file << kv.first << "=" << kv.second << "\n";
        } else if (is_sensitive_field(kv.first)) {
            file << kv.first << "=" << encrypt_value(kv.second) << "\n";
        } else {
            file << kv.first << "=" << kv.second << "\n";
        }
    }
    return true;
}

// ============================================================
// 扫描 /etc/xtrader/ 下的所有环境目录（含 xtrader.ini 才计入）
// ============================================================
std::vector<std::filesystem::path> scan_env_dirs() {
    std::vector<std::filesystem::path> envs;
    std::error_code ec;
    if (!std::filesystem::is_directory(CONFIG_ROOT, ec)) return envs;

    for (const auto& entry : std::filesystem::directory_iterator(CONFIG_ROOT, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        auto ini_path = entry.path() / "xtrader.ini";
        if (std::filesystem::is_regular_file(ini_path, ec)) {
            envs.push_back(entry.path());
        }
    }
    return envs;
}

// ============================================================
// 交互式创建单个环境配置（逐个问答账号信息）
// ============================================================
void create_config_interactive(const std::string& env_name, const std::string& account_env) {
    std::cout << "\n========== Creating [" << env_name << "] ==========" << std::endl;
    if (account_env == "real") {
        std::cout << "  (广发01 实盘环境)" << std::endl;
    } else {
        std::cout << "  (模拟账号环境)" << std::endl;
    }
    std::cout << "请依次输入以下配置信息（直接回车使用默认值）：" << std::endl;

    std::map<std::string, std::string> cfg;
    cfg["account_env"] = account_env;

    struct field_info {
        std::string key;
        std::string prompt;
        std::string default_val;
    };

    std::vector<field_info> fields = {
        {"broker_id",    "期货公司代码 (broker_id)",     "9999"},
        {"investor_id",  "投资者账号 (investor_id)",     ""},
        {"password",     "密码 (password)",              ""},
        {"app_id",       "AppID",                       "simnow_client_test"},
        {"auth_code",    "认证码 (auth_code)",           "0000000000000000"},
        {"md_front",     "行情前置地址 (md_front)",      "tcp://180.168.146.187:10131"},
        {"trade_front",  "交易前置地址 (trade_front)",   "tcp://180.168.146.187:10130"},
    };

    for (const auto& f : fields) {
        std::string dflt = f.default_val;
        if (account_env == "real") {
            // 实盘默认值覆盖
            if (f.key == "app_id")      dflt = "";
            if (f.key == "auth_code")   dflt = "";
            if (f.key == "md_front")    dflt = "";
            if (f.key == "trade_front") dflt = "";
        }
        std::cout << "  " << f.prompt;
        if (!dflt.empty()) {
            std::cout << " [" << dflt << "]";
        }
        std::cout << ": ";

        std::string input;
        std::getline(std::cin, input);
        cfg[f.key] = input.empty() ? dflt : input;
    }

    auto ini_path = std::filesystem::path(CONFIG_ROOT) / env_name / "xtrader.ini";
    if (write_config_file(ini_path, cfg)) {
        std::cout << "[OK] Created: " << ini_path << std::endl;
    }
}

// ============================================================
// 检查默认环境（guangfa01 + simu01），缺失时提示创建
// ============================================================
void ensure_default_envs() {
    for (const auto& env_name : DEFAULT_ENVS) {
        auto ini_path = std::filesystem::path(CONFIG_ROOT) / env_name / "xtrader.ini";
        if (std::filesystem::is_regular_file(ini_path)) continue;

        std::string account_env = (env_name == "guangfa01") ? "real" : "simu";
        std::string label = (env_name == "guangfa01") ? "广发01实盘" : "模拟账号";

        std::cout << "\n[" << env_name << "] 环境配置不存在: " << ini_path << std::endl;
        std::cout << "是否创建 " << label << " 配置文件？(y/n) [n]: ";

        std::string input;
        std::getline(std::cin, input);
        input = to_lower(input);

        if (input == "y" || input == "yes") {
            create_config_interactive(env_name, account_env);
        } else {
            std::cout << "[SKIP] 跳过 " << env_name << " 配置创建" << std::endl;
        }
    }
}

// ============================================================
// 打印单个环境配置摘要
// ============================================================
void print_env_summary(const std::filesystem::path& env_dir) {
    auto ini = env_dir / "xtrader.ini";
    auto cfg = read_config_file(ini);
    std::string broker = cfg.count("broker_id") ? cfg["broker_id"] : "-";
    std::string investor = cfg.count("investor_id") ? cfg["investor_id"] : "-";
    std::string env = cfg.count("account_env") ? cfg["account_env"] : "-";
    std::string env_tag = (env == "real") ? "REAL" : "SIMU";

    std::cout << "  [" << env_tag << "] " << env_dir.filename().string()
              << "  |  broker: " << broker
              << "  |  investor: " << investor << std::endl;
}

// ============================================================
// show_config_info（重构版）
// ============================================================
void show_config_info() {
    std::cout << "\n========== Configuration Info ==========" << std::endl;
    std::cout << "Version:    " << seastar::xtrader::version << std::endl;
    std::cout << "Build:      " << seastar::xtrader::build_type << std::endl;
    std::cout << "Git Commit: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "Git Branch: " << seastar::xtrader::git_branch << std::endl;
    std::cout << "Build Time: " << seastar::xtrader::build_timestamp << std::endl;
    std::cout << "========================================" << std::endl;

    // 扫描 /etc/xtrader/ 下所有环境
    auto envs = scan_env_dirs();

    std::cout << "\n--- Account Environments ---" << std::endl;
    std::cout << "Config Root: " << CONFIG_ROOT << std::endl;

    if (envs.empty()) {
        std::cout << "(no environments found)" << std::endl;
    } else {
        for (const auto& env_dir : envs) {
            print_env_summary(env_dir);
        }
    }
    std::cout << "Total: " << envs.size() << " environment(s)" << std::endl;

    // 检查默认环境，缺失则提示创建
    ensure_default_envs();

    // 数据目录
    std::cout << "\n--- Data Directories ---" << std::endl;
    std::cout << "Default Data Root: /data/x_trader_data" << std::endl;
    std::cout << "========================================" << std::endl;
}

void run_heartbeat_test() {
    std::cout << "\n[Heartbeat Test]" << std::endl;
    auto start = std::chrono::steady_clock::now();
    seastar::xtrader::redis_sync_client redis;
    if (!redis.connect("127.0.0.1", 6379)) {
        std::cout << "[FAIL] Cannot connect to Redis" << std::endl;
        return;
    }
    auto latency = std::chrono::steady_clock::now() - start;
    std::cout << "[OK] Connected to Redis in "
              << std::chrono::duration<double, std::milli>(latency).count()
              << " ms" << std::endl;

    // Test SET/GET
    bool set_ok = redis.set("xtrader:heartbeat:test", "ok");
    if (set_ok) {
        std::string val = redis.get("xtrader:heartbeat:test");
        std::cout << "[OK] SET/GET test: " << val << std::endl;
    } else {
        std::cout << "[FAIL] SET failed" << std::endl;
    }

    redis.disconnect();
}

void run_root_menu() {
    std::string data_root = "/data/x_trader_data";
    std::string import_source_path;
    seastar::xtrader::import_source_type import_source_type = seastar::xtrader::import_source_type::taobao_csv;
    seastar::xtrader::import_mode import_mode = seastar::xtrader::import_mode::incremental;
    std::string import_config_path;

    while (true) {
        std::cout << "\n========== X-Trader CLI ==========" << std::endl;
        std::cout << "1) Live" << std::endl;
        std::cout << "2) Replay" << std::endl;
        std::cout << "3) Import" << std::endl;
        std::cout << "4) Data Brief" << std::endl;
        std::cout << "5) Config Info" << std::endl;
        std::cout << "6) Heartbeat Test" << std::endl;
        std::cout << "q) Quit" << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << "Select: ";

        std::string input;
        std::getline(std::cin, input);
        input = to_lower(input);

        if (input == "1") {
            std::cout << "[TODO] Live mode" << std::endl;
        } else if (input == "2") {
            std::cout << "[TODO] Replay mode" << std::endl;
        } else if (input == "3") {
            // Import menu
            while (true) {
                std::cout << "\n[Import Menu]" << std::endl;
                std::cout << "  source_path: " << (import_source_path.empty() ? "(empty)" : import_source_path) << std::endl;
                std::cout << "  source_type: " << (import_source_type == seastar::xtrader::import_source_type::taobao_csv ? "taobao_csv" : "ctp_clean_csv") << std::endl;
                std::cout << "  mode: " << (import_mode == seastar::xtrader::import_mode::incremental ? "incremental" : "overwrite") << std::endl;
                std::cout << "  config_path: " << (import_config_path.empty() ? "(default)" : import_config_path) << std::endl;
                std::cout << "  1) Set source path" << std::endl;
                std::cout << "  2) Set source type" << std::endl;
                std::cout << "  3) Set import mode" << std::endl;
                std::cout << "  4) Set config path" << std::endl;
                std::cout << "  5) Run import" << std::endl;
                std::cout << "  u) Up" << std::endl;
                std::cout << "Select: ";

                std::getline(std::cin, input);
                input = to_lower(input);

                if (input == "1") {
                    std::cout << "Enter source path: ";
                    std::getline(std::cin, import_source_path);
                    std::cout << "source_path set to " << import_source_path << std::endl;
                } else if (input == "2") {
                    std::cout << "Source types:\n  1) taobao_csv\n  2) ctp_clean_csv\nSelect: ";
                    std::getline(std::cin, input);
                    if (input == "1" || input == "taobao" || input == "taobao_csv") {
                        import_source_type = seastar::xtrader::import_source_type::taobao_csv;
                        std::cout << "source_type set to taobao_csv" << std::endl;
                    } else if (input == "2" || input == "ctp" || input == "ctp_clean_csv") {
                        import_source_type = seastar::xtrader::import_source_type::ctp_clean_csv;
                        std::cout << "source_type set to ctp_clean_csv" << std::endl;
                    } else {
                        std::cerr << "[ERROR] Unknown source type" << std::endl;
                    }
                } else if (input == "3") {
                    std::cout << "Import modes:\n  1) incremental\n  2) overwrite\nSelect: ";
                    std::getline(std::cin, input);
                    if (input == "1" || input == "incremental" || input == "inc") {
                        import_mode = seastar::xtrader::import_mode::incremental;
                        std::cout << "import_mode set to incremental" << std::endl;
                    } else if (input == "2" || input == "overwrite" || input == "full") {
                        import_mode = seastar::xtrader::import_mode::overwrite;
                        std::cout << "import_mode set to overwrite" << std::endl;
                    } else {
                        std::cerr << "[ERROR] Unknown import mode" << std::endl;
                    }
                } else if (input == "4") {
                    std::cout << "Enter config path (empty for default): ";
                    std::getline(std::cin, import_config_path);
                    if (import_config_path.empty()) {
                        std::cout << "config_path reset to default" << std::endl;
                    } else {
                        std::cout << "config_path set to " << import_config_path << std::endl;
                    }
                } else if (input == "5") {
                    if (import_source_path.empty()) {
                        std::cerr << "[ERROR] source_path not set" << std::endl;
                        continue;
                    }
                    if (!std::filesystem::exists(import_source_path)) {
                        std::cerr << "[ERROR] source_path does not exist: " << import_source_path << std::endl;
                        continue;
                    }

                    seastar::xtrader::import_options opts;
                    opts.source_path = import_source_path;
                    opts.source_type = import_source_type;
                    opts.mode = import_mode;
                    if (!import_config_path.empty()) {
                        opts.config_path = import_config_path;
                    }

                    std::cout << "Starting import..." << std::endl;
                    seastar::xtrader::data_importer importer;
                    auto result = importer.execute(opts);

                    std::cout << "[OK] Import " << (result.success ? "succeeded" : "failed") << std::endl;
                    std::cout << "  Files: " << result.scanned_files << " scanned, "
                              << result.success_files << " success, "
                              << result.skipped_files << " skipped, "
                              << result.failed_files << " failed" << std::endl;
                    std::cout << "  Rows: " << result.rows_added << " added, "
                              << result.rows_overwritten << " overwritten, "
                              << result.rows_skipped << " skipped" << std::endl;
                    if (!result.message.empty()) {
                        std::cout << "  Message: " << result.message << std::endl;
                    }
                } else if (input == "u" || input == "back" || input == "up") {
                    break;
                } else {
                    std::cerr << "[ERROR] Unknown command" << std::endl;
                }
            }
        } else if (input == "4") {
            // Data brief
            std::cout << "\n[Data Brief]" << std::endl;
            seastar::xtrader::historical_data_manager manager(data_root);
            std::string warning;
            auto datasets = manager.scan_datasets(&warning);
            if (!warning.empty()) {
                std::cout << "[WARN] " << warning << std::endl;
            }
            std::cout << "Found " << datasets.size() << " datasets:" << std::endl;
            for (const auto& ds : datasets) {
                std::cout << "  " << ds.exchange << "/" << ds.symbol
                          << " " << ds.timeframe
                          << " (" << ds.row_count << " records)" << std::endl;
            }
        } else if (input == "5") {
            show_config_info();
        } else if (input == "6") {
            run_heartbeat_test();
        } else if (input == "q" || input == "quit" || input == "exit") {
            std::cout << "Goodbye!" << std::endl;
            break;
        } else {
            std::cout << "Invalid option" << std::endl;
        }
    }
}

} // namespace

int main(int ac, char** av) {
    std::cout << "========================================" << std::endl;
    std::cout << "X-Trader Seastar Trading Engine v"
              << seastar::xtrader::version << std::endl;
    std::cout << "Build: " << seastar::xtrader::build_type
              << " | Git: " << seastar::xtrader::git_commit << std::endl;
    std::cout << "========================================" << std::endl;

    seastar::app_template app;

    return app.run(ac, av, [] {
        return seastar::async([] {
            run_root_menu();
        });
    });
}
