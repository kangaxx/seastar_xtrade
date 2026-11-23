// Session language is inherited by sibling utilities, never written into account/data files.
// Source: bilingual menu request, 2026-11-23. Signature: GitHub Copilot (gpt-6.1-sol).
#pragma once

#include <cerrno>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace seastar::xtrader::cli {

enum class ui_language { english, chinese };

inline ui_language parse_ui_language(const std::string& value) {
    if (value.empty() || value == "en") { return ui_language::english; }
    if (value == "zh") { return ui_language::chinese; }
    throw std::invalid_argument("XTRADER_LANGUAGE must be en or zh / XTRADER_LANGUAGE 必须是 en 或 zh");
}

inline ui_language& current_ui_language() {
    static ui_language language = [] {
        const auto value = std::getenv("XTRADER_LANGUAGE");
        return parse_ui_language(value ? value : "");
    }();
    return language;
}

inline const char* ui_text(const char* english, const char* chinese) {
    return current_ui_language() == ui_language::chinese ? chinese : english;
}

inline void set_ui_language(ui_language language) {
    auto& current = current_ui_language();
    const char* value = language == ui_language::chinese ? "zh" : "en";
#ifdef _WIN32
    const auto error = ::_putenv_s("XTRADER_LANGUAGE", value);
    if (error) { throw std::system_error(error, std::generic_category(), "set UI language"); }
#else
    if (::setenv("XTRADER_LANGUAGE", value, 1) != 0) {
        throw std::system_error(errno, std::generic_category(), "set UI language");
    }
#endif
    current = language;
}

inline void run_language_menu(std::istream& input, std::ostream& output, std::ostream& errors) {
    std::string line;
    while (true) {
        output << "\n[Language / 语言]\n1) English\n2) 简体中文\n"
               << ui_text("u) Back (Enter keeps current)\nSelect: ",
                          "u) 返回（回车保持当前语言）\n选择：") << std::flush;
        if (!std::getline(input, line) || line.empty() || line == "u" || line == "back") { return; }
        try {
            if (line == "1" || line == "en" || line == "EN") {
                set_ui_language(ui_language::english);
            } else if (line == "2" || line == "zh" || line == "ZH") {
                set_ui_language(ui_language::chinese);
            } else {
                errors << ui_text("[ERROR] Choose 1/English, 2/中文 or u.\n",
                                  "[ERROR] 请选择 1/English、2/中文，或 u 返回。\n");
                continue;
            }
            output << ui_text("[OK] Language: English\n", "[OK] 当前语言：简体中文\n");
            return;
        } catch (const std::system_error& error) {
            errors << ui_text("[ERROR] Cannot change language: ", "[ERROR] 无法切换语言：")
                   << error.what() << '\n';
            return;
        }
    }
}

} // namespace seastar::xtrader::cli
