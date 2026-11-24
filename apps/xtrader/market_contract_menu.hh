// Offline product lookup and guided contract entry; never guesses listed or main contracts.
// Source: CLI product guidance request, 2026-11-24. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include "cli_language.hh"
#include <seastar/xtrader/market_capture_config.hh>

#include <algorithm>
#include <charconv>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seastar::xtrader::cli {
namespace contract_menu_detail {

struct exchange_entry {
    const char* code;
    const char* english;
    const char* chinese;
};

inline constexpr exchange_entry EXCHANGES[] = {
    {"SHFE", "Shanghai Futures Exchange", "上海期货交易所（上期所）"},
    {"DCE", "Dalian Commodity Exchange", "大连商品交易所（大商所）"},
    {"CZCE", "Zhengzhou Commodity Exchange", "郑州商品交易所（郑商所）"},
    {"CFFEX", "China Financial Futures Exchange", "中国金融期货交易所（中金所）"},
    {"INE", "Shanghai International Energy Exchange", "上海国际能源交易中心（能源中心）"},
    {"GFEX", "Guangzhou Futures Exchange", "广州期货交易所（广期所）"},
};

struct product_entry {
    const char* exchange;
    const char* code;
    const char* english;
    const char* chinese;
};

inline constexpr product_entry PRODUCTS[] = {
    {"SHFE", "rb", "Rebar", "螺纹钢"},
    {"SHFE", "ag", "Silver", "白银"},
    {"SHFE", "au", "Gold", "黄金"},
    {"SHFE", "cu", "Copper", "铜"},
    {"SHFE", "al", "Aluminum", "铝"},
    {"SHFE", "zn", "Zinc", "锌"},
    {"SHFE", "pb", "Lead", "铅"},
    {"SHFE", "ni", "Nickel", "镍"},
    {"SHFE", "sn", "Tin", "锡"},
    {"SHFE", "ss", "Stainless steel", "不锈钢"},
    {"SHFE", "hc", "Hot rolled coil", "热轧卷板"},
    {"SHFE", "wr", "Wire rod", "线材"},
    {"SHFE", "ru", "Natural rubber", "天然橡胶"},
    {"SHFE", "bu", "Bitumen", "石油沥青"},
    {"SHFE", "fu", "Fuel oil", "燃料油"},
    {"SHFE", "sp", "Wood pulp", "纸浆"},
    {"SHFE", "ao", "Alumina", "氧化铝"},
    {"DCE", "m", "Soybean meal", "豆粕"},
    {"DCE", "i", "Iron ore", "铁矿石"},
    {"DCE", "j", "Coke", "焦炭"},
    {"DCE", "jm", "Coking coal", "焦煤"},
    {"DCE", "a", "No.1 soybean", "豆一"},
    {"DCE", "b", "No.2 soybean", "豆二"},
    {"DCE", "y", "Soybean oil", "豆油"},
    {"DCE", "p", "Palm oil", "棕榈油"},
    {"DCE", "c", "Corn", "玉米"},
    {"DCE", "cs", "Corn starch", "玉米淀粉"},
    {"DCE", "l", "LLDPE", "聚乙烯"},
    {"DCE", "v", "PVC", "聚氯乙烯"},
    {"DCE", "pp", "Polypropylene", "聚丙烯"},
    {"DCE", "eg", "Ethylene glycol", "乙二醇"},
    {"DCE", "eb", "Styrene", "苯乙烯"},
    {"DCE", "pg", "LPG", "液化石油气"},
    {"DCE", "jd", "Egg", "鸡蛋"},
    {"DCE", "lh", "Live hog", "生猪"},
    {"DCE", "rr", "Milled rice", "粳米"},
    {"CZCE", "SA", "Soda ash", "纯碱"},
    {"CZCE", "SR", "White sugar", "白糖"},
    {"CZCE", "TA", "PTA", "精对苯二甲酸"},
    {"CZCE", "MA", "Methanol", "甲醇"},
    {"CZCE", "FG", "Glass", "玻璃"},
    {"CZCE", "CF", "Cotton", "棉花"},
    {"CZCE", "CY", "Cotton yarn", "棉纱"},
    {"CZCE", "RM", "Rapeseed meal", "菜粕"},
    {"CZCE", "OI", "Rapeseed oil", "菜油"},
    {"CZCE", "AP", "Apple", "苹果"},
    {"CZCE", "CJ", "Dried jujube", "红枣"},
    {"CZCE", "UR", "Urea", "尿素"},
    {"CZCE", "PF", "Polyester staple fiber", "短纤"},
    {"CZCE", "SF", "Ferrosilicon", "硅铁"},
    {"CZCE", "SM", "Silicomanganese", "锰硅"},
    {"CZCE", "PK", "Peanut", "花生"},
    {"CZCE", "PX", "Paraxylene", "对二甲苯"},
    {"CZCE", "SH", "Caustic soda", "烧碱"},
    {"CFFEX", "IF", "CSI 300 index", "沪深300股指"},
    {"CFFEX", "IH", "SSE 50 index", "上证50股指"},
    {"CFFEX", "IC", "CSI 500 index", "中证500股指"},
    {"CFFEX", "IM", "CSI 1000 index", "中证1000股指"},
    {"CFFEX", "TS", "2-year government bond", "2年期国债"},
    {"CFFEX", "TF", "5-year government bond", "5年期国债"},
    {"CFFEX", "T", "10-year government bond", "10年期国债"},
    {"CFFEX", "TL", "30-year government bond", "30年期国债"},
    {"INE", "sc", "Crude oil", "原油"},
    {"INE", "lu", "Low sulfur fuel oil", "低硫燃料油"},
    {"INE", "nr", "TSR 20 rubber", "20号胶"},
    {"INE", "bc", "International copper", "国际铜"},
    {"INE", "ec", "Containerized freight (Europe)", "集运指数（欧线）"},
    {"GFEX", "lc", "Lithium carbonate", "碳酸锂"},
    {"GFEX", "si", "Industrial silicon", "工业硅"},
};

inline std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [] (unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    return text;
}

inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return {}; }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::optional<std::size_t> listed_number(const std::string& text, std::size_t count) {
    std::size_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && number && number <= count) {
        return number - 1;
    }
    return std::nullopt;
}

inline std::vector<const product_entry*> find_products(const std::string& query, std::string_view exchange = {}) {
    std::vector<const product_entry*> matches;
    const auto key = lower(query);
    for (const auto& product : PRODUCTS) {
        if (!exchange.empty() && exchange != product.exchange) { continue; }
        if (key == lower(product.code) || query == product.chinese || key == lower(product.english)) {
            return {&product};
        }
        if (key.empty() || lower(product.code).find(key) != std::string::npos
                || std::string_view(product.chinese).find(query) != std::string_view::npos
                || lower(product.english).find(key) != std::string::npos) {
            matches.push_back(&product);
        }
    }
    return matches;
}

inline void print_product(const product_entry& product, std::ostream& output) {
    output << ui_text(product.english, product.chinese) << " [" << product.code << " / " << product.exchange << ']';
}

inline const product_entry* choose_product(std::vector<const product_entry*> products, std::string_view exchange,
        std::istream& input, std::ostream& output, std::ostream& errors) {
    while (true) {
        if (products.empty()) {
            errors << ui_text("[ERROR] No matching product. Use a name/code, or EXCHANGE:instrument for unlisted products.\n",
                              "[ERROR] 未找到品种。请换名称/代码；未收录品种可用 EXCHANGE:instrument 输入。\n");
            return nullptr;
        }
        if (products.size() == 1) { return products.front(); }
        for (std::size_t index = 0; index < products.size(); ++index) {
            output << index + 1 << ") ";
            print_product(*products[index], output);
            output << '\n';
        }
        output << ui_text("Product number or search name/code (u returns): ",
                          "品种编号，或输入名称/代码搜索（u 返回）：") << std::flush;
        std::string line;
        if (!std::getline(input, line)) { return nullptr; }
        line = trim(line);
        if (lower(line) == "u") { return nullptr; }
        if (const auto index = listed_number(line, products.size())) { return products[*index]; }
        if (line.empty()) { continue; }
        const auto matches = find_products(line, exchange);
        if (matches.empty()) {
            errors << ui_text("[ERROR] No match in this list; try again or u.\n", "[ERROR] 当前范围内没有匹配项，请重试或输入 u。\n");
        } else {
            products = matches;
        }
    }
}

inline bool valid_month(const std::string& digits, std::size_t width) {
    if (digits.size() != width || !std::all_of(digits.begin(), digits.end(), [] (unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) { return false; }
    const auto month = digits.substr(digits.size() - 2);
    return month >= "01" && month <= "12";
}

inline std::optional<market_capture_contract> enter_month(const product_entry& product,
        std::istream& input, std::ostream& output, std::ostream& errors) {
    output << ui_text("Selected: ", "已选品种：");
    print_product(product, output);
    output << '\n';
    std::string line;
    while (true) {
        output << ui_text("Delivery year/month YYYYMM (e.g. 202701; u returns): ",
                          "交割年月 YYYYMM（如 202701；u 返回）：") << std::flush;
        if (!std::getline(input, line)) { return std::nullopt; }
        line = trim(line);
        if (lower(line) == "u") { return std::nullopt; }
        if (!valid_month(line, 6) || line.substr(0, 2) != "20") {
            errors << ui_text("[ERROR] Use YYYYMM, year 2000-2099, month 01-12. Listed months are not checked.\n",
                              "[ERROR] 请输入 YYYYMM，年份为 2000-2099，月份为 01-12；不校验是否挂牌。\n");
            continue;
        }
        const auto offset = std::string_view(product.exchange) == "CZCE" ? 3 : 2;
        return market_capture_contract{product.exchange, std::string(product.code) + line.substr(offset)};
    }
}

inline std::vector<market_capture_contract> parse_direct(const std::string& text) {
    if (text.find(':') != std::string::npos) { return parse_market_capture_contracts(text); }
    const auto digit = text.find_first_of("0123456789");
    if (digit == std::string::npos) { return {}; }
    const auto code = lower(text.substr(0, digit));
    for (const auto& product : PRODUCTS) {
        if (code != lower(product.code)) { continue; }
        const auto width = std::string_view(product.exchange) == "CZCE" ? 3u : 4u;
        const auto month = text.substr(digit);
        if (!valid_month(month, width)) {
            throw std::invalid_argument(ui_text("use a complete contract: CZCE uses YMM, other exchanges use YYMM",
                                               "请输入完整合约：郑商所使用 YMM，其他交易所使用 YYMM"));
        }
        return {{product.exchange, std::string(product.code) + month}};
    }
    return {};
}

inline void append_contracts(std::vector<market_capture_contract>& selected,
        const std::vector<market_capture_contract>& added, std::ostream& output) {
    auto combined = selected;
    combined.insert(combined.end(), added.begin(), added.end());
    validate_market_capture_contracts(combined);
    selected = std::move(combined);
    for (const auto& contract : added) {
        output << ui_text("[OK] Added ", "[OK] 已添加 ") << contract.exchange << ':' << contract.instrument << '\n';
    }
}

} // namespace contract_menu_detail

inline std::optional<std::vector<market_capture_contract>> run_market_contract_menu(
        std::istream& input, std::ostream& output, std::ostream& errors) {
    namespace detail = contract_menu_detail;
    std::vector<market_capture_contract> selected;
    output << ui_text("\n[Contract selection - no connection]\n"
                      "Offline common-product reference, NOT a live listing or permission check.\n"
                      "Choose a listed month yourself; no main/continuous contract is inferred.\n",
                      "\n[合约选择 - 尚未连接]\n内置常用品种速查，不是实时挂牌清单，也不验证账号权限。\n"
                      "请自行确认挂牌月份；不自动选择主力或连续合约。\n");
    std::string line;
    while (true) {
        output << ui_text("\nSelected (", "\n已选（") << selected.size() << "/64):";
        for (const auto& contract : selected) { output << ' ' << contract.exchange << ':' << contract.instrument; }
        output << '\n';
        std::size_t number = 0;
        for (const auto& exchange : detail::EXCHANGES) {
            output << ++number << ") " << ui_text(exchange.english, exchange.chinese) << " [" << exchange.code << "]\n";
        }
        output << ui_text("Enter exchange number, product name/code (Silver/ag), or full contract (ag2701).\n"
                          "You may also paste EXCHANGE:instrument pairs, comma separated.\n"
                          "d) Done  r) Remove last  u) Cancel all\nSelect: ",
                          "输入交易所编号、品种名称/代码（白银/ag），或完整合约（ag2701）。\n"
                          "也可粘贴 EXCHANGE:instrument，多个以英文逗号分隔。\n"
                          "d) 完成  r) 删除上一项  u) 全部取消\n选择：") << std::flush;
        if (!std::getline(input, line)) { return std::nullopt; }
        line = detail::trim(line);
        const auto command = detail::lower(line);
        if (command == "u") { return std::nullopt; }
        if (command == "d") {
            if (!selected.empty()) { return selected; }
            errors << ui_text("[ERROR] Add at least one concrete contract first.\n", "[ERROR] 请先添加至少一个具体合约。\n");
            continue;
        }
        if (command == "r") {
            if (!selected.empty()) { selected.pop_back(); }
            else { errors << ui_text("[ERROR] No contract to remove.\n", "[ERROR] 没有可删除的合约。\n"); }
            continue;
        }
        if (line.empty()) { continue; }
        try {
            const auto direct = detail::parse_direct(line);
            if (!direct.empty()) {
                detail::append_contracts(selected, direct, output);
                continue;
            }
            const detail::product_entry* product = nullptr;
            if (const auto index = detail::listed_number(line, std::size(detail::EXCHANGES))) {
                const auto exchange = detail::EXCHANGES[*index].code;
                product = detail::choose_product(detail::find_products({}, exchange), exchange, input, output, errors);
            } else {
                product = detail::choose_product(detail::find_products(line), {}, input, output, errors);
            }
            if (product) {
                if (const auto contract = detail::enter_month(*product, input, output, errors)) {
                    detail::append_contracts(selected, {*contract}, output);
                }
            }
        } catch (const std::invalid_argument& error) {
            errors << "[ERROR] " << error.what() << '\n';
        }
    }
}

} // namespace seastar::xtrader::cli
