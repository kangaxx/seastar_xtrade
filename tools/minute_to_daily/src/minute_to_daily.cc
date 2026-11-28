// 分钟线期货数据 -> 日线期货数据（独立 SO 实现）。
//
// 关键口径：输入时间戳是自然日，夜盘归下一个交易日。
// 例如 2025-01-03（周五）21:00 的夜盘，属于 2025-01-06（周一）的日线。
#include "xtrader/minute_to_daily.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// region 通用工具

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'
                             || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    return text.substr(begin);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (char ch : line) {
        if (quoted) {
            if (ch == '"') {
                quoted = false;
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

bool parse_double(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && errno == 0;
}

bool parse_count(const std::string& text, long long& value) {
    double number = 0.0;
    if (!parse_double(text, number)) {
        return false;
    }
    value = static_cast<long long>(std::llround(number));
    return true;
}

bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

// 解析 "YYYY-MM-DD HH:MM:SS"，只取自然日与小时。
bool parse_timestamp(const std::string& text, std::string& date, int& hour) {
    if (text.size() < 16) {
        return false;
    }
    static const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15};
    for (int position : digit_positions) {
        if (!is_digit(text[static_cast<size_t>(position)])) {
            return false;
        }
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':') {
        return false;
    }
    date = text.substr(0, 10);
    hour = (text[11] - '0') * 10 + (text[12] - '0');
    return hour >= 0 && hour < 24;
}

bool is_day_hour(int hour) {
    return hour >= 6 && hour < 18;
}

// endregion

// region 表头映射

struct Columns {
    int date = -1;
    int open = -1;
    int high = -1;
    int low = -1;
    int close = -1;
    int volume = -1;
    int open_interest = -1;
    int money = -1;
    int source_symbol = -1;
};

bool map_columns(const std::vector<std::string>& header, Columns& columns, std::string& error) {
    for (size_t index = 0; index < header.size(); ++index) {
        const std::string name = trim(header[index]);
        if (name == "date") columns.date = static_cast<int>(index);
        else if (name == "open") columns.open = static_cast<int>(index);
        else if (name == "high") columns.high = static_cast<int>(index);
        else if (name == "low") columns.low = static_cast<int>(index);
        else if (name == "close") columns.close = static_cast<int>(index);
        else if (name == "volume") columns.volume = static_cast<int>(index);
        else if (name == "open_interest") columns.open_interest = static_cast<int>(index);
        else if (name == "money") columns.money = static_cast<int>(index);
        else if (name == "source_symbol") columns.source_symbol = static_cast<int>(index);
    }
    if (columns.date < 0 || columns.open < 0 || columns.high < 0 || columns.low < 0
            || columns.close < 0 || columns.volume < 0) {
        error = "input header is missing required columns: date/open/high/low/close/volume";
        return false;
    }
    return true;
}

// endregion

// region 日线聚合

struct DailyBar {
    bool has_open = false;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    long long volume = 0;
    long long open_interest = 0;
    double money = 0.0;
    std::string source_symbol;
    long long bars = 0;
};

void append_bar(DailyBar& daily, double open, double high, double low, double close,
                long long volume, long long open_interest, double money,
                const std::string& source_symbol) {
    if (!daily.has_open) {
        daily.open = open;
        daily.high = high;
        daily.low = low;
        daily.has_open = true;
    } else {
        daily.high = std::max(daily.high, high);
        daily.low = std::min(daily.low, low);
    }
    daily.close = close;
    daily.volume += volume;
    daily.open_interest = open_interest;
    daily.money += money;
    if (!source_symbol.empty()) {
        daily.source_symbol = source_symbol;
    }
    ++daily.bars;
}

struct ConvertContext {
    const xt_m2d_options& options;
    xt_m2d_stats& stats;
    Columns columns;
};

bool resolve_trading_day(const ConvertContext& context, const std::string& calendar_date,
                         int hour, const std::set<std::string>& trading_dates,
                         std::string& trading_day) {
    if (is_day_hour(hour)) {
        trading_day = calendar_date;
        return trading_dates.find(calendar_date) != trading_dates.end();
    }
    // 夜盘：默认归下一交易日；数据源已按交易日打标签时可用 same-day 模式。
    if (context.options.night_mode == XT_M2D_NIGHT_SAME_DAY) {
        trading_day = calendar_date;
        return true;
    }
    auto next = trading_dates.upper_bound(calendar_date);
    if (next == trading_dates.end()) {
        ++context.stats.unassigned_night_rows;
        return false;
    }
    trading_day = *next;
    return true;
}

bool first_pass(ConvertContext& context, std::set<std::string>& trading_dates,
                std::string& error) {
    std::ifstream input(context.options.input_path);
    if (!input) {
        error = std::string("cannot open input file: ") + context.options.input_path;
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "input file is empty";
        return false;
    }
    if (!map_columns(split_csv(trim(line)), context.columns, error)) {
        return false;
    }
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const auto fields = split_csv(line);
        const int date_index = context.columns.date;
        if (date_index < 0 || static_cast<size_t>(date_index) >= fields.size()) {
            continue;
        }
        std::string date;
        int hour = 0;
        if (!parse_timestamp(fields[static_cast<size_t>(date_index)], date, hour)) {
            continue;
        }
        if (is_day_hour(hour)) {
            trading_dates.insert(date);
        }
    }
    if (trading_dates.empty()) {
        error = "no day-session rows found; cannot determine trading calendar";
        return false;
    }
    return true;
}

bool second_pass(ConvertContext& context, const std::set<std::string>& trading_dates,
                 std::map<std::string, DailyBar>& daily, std::string& error) {
    std::ifstream input(context.options.input_path);
    if (!input) {
        error = std::string("cannot open input file: ") + context.options.input_path;
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "input file is empty";
        return false;
    }

    const std::string symbol_filter = context.options.symbol ? context.options.symbol : "";
    const std::string start_date = context.options.start_date ? context.options.start_date : "";
    const std::string end_date = context.options.end_date ? context.options.end_date : "";

    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        ++context.stats.input_rows;
        const auto fields = split_csv(line);
        const Columns& columns = context.columns;

        auto field = [&fields](int index) -> std::string {
            if (index < 0 || static_cast<size_t>(index) >= fields.size()) {
                return std::string();
            }
            return trim(fields[static_cast<size_t>(index)]);
        };

        std::string date;
        int hour = 0;
        if (!parse_timestamp(field(columns.date), date, hour)) {
            ++context.stats.skipped_rows;
            continue;
        }
        const std::string source_symbol = field(columns.source_symbol);
        if (!symbol_filter.empty() && source_symbol != symbol_filter) {
            ++context.stats.skipped_rows;
            continue;
        }

        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        long long volume = 0;
        if (!parse_double(field(columns.open), open)
                || !parse_double(field(columns.high), high)
                || !parse_double(field(columns.low), low)
                || !parse_double(field(columns.close), close)
                || !parse_count(field(columns.volume), volume)) {
            ++context.stats.skipped_rows;
            continue;
        }
        long long open_interest = 0;
        if (columns.open_interest >= 0) {
            parse_count(field(columns.open_interest), open_interest);
        }
        double money = 0.0;
        if (columns.money >= 0) {
            parse_double(field(columns.money), money);
        }

        std::string trading_day;
        if (!resolve_trading_day(context, date, hour, trading_dates, trading_day)) {
            if (is_day_hour(hour)) {
                ++context.stats.skipped_rows;
            }
            continue;
        }
        if (!start_date.empty() && trading_day < start_date) {
            ++context.stats.skipped_rows;
            continue;
        }
        if (!end_date.empty() && trading_day > end_date) {
            ++context.stats.skipped_rows;
            continue;
        }

        append_bar(daily[trading_day], open, high, low, close, volume, open_interest, money,
                   source_symbol);
    }
    return true;
}

bool write_output(const std::string& path, const std::map<std::string, DailyBar>& daily,
                  long long& output_rows, std::string& error) {
    std::ofstream output(path);
    if (!output) {
        error = std::string("cannot open output file: ") + path;
        return false;
    }
    output << "date,open,high,low,close,volume,open_interest,money,source_symbol\n";
    for (const auto& entry : daily) {
        const DailyBar& bar = entry.second;
        if (!bar.has_open) {
            continue;
        }
        output << entry.first << ','
               << std::fixed << std::setprecision(4) << bar.open << ','
               << bar.high << ',' << bar.low << ',' << bar.close << ','
               << bar.volume << ',' << bar.open_interest << ','
               << std::setprecision(2) << bar.money << ','
               << bar.source_symbol << '\n';
        ++output_rows;
    }
    if (!output.good()) {
        error = std::string("failed while writing output file: ") + path;
        return false;
    }
    return true;
}

// endregion

bool convert(const xt_m2d_options& options, xt_m2d_stats& stats, std::string& error) {
    ConvertContext context{options, stats, Columns{}};
    std::set<std::string> trading_dates;
    if (!first_pass(context, trading_dates, error)) {
        return false;
    }

    std::map<std::string, DailyBar> daily;
    if (!second_pass(context, trading_dates, daily, error)) {
        return false;
    }
    if (daily.empty()) {
        error = "no daily bars generated; check symbol/date filters";
        return false;
    }
    return write_output(options.output_path, daily, stats.output_rows, error);
}

void set_error(xt_m2d_stats* stats, const std::string& message) {
    if (!stats) {
        return;
    }
    std::snprintf(stats->error, sizeof(stats->error), "%s", message.c_str());
}

} // namespace

// region C ABI

extern "C" int xt_m2d_convert(const xt_m2d_options* options, xt_m2d_stats* stats) {
    xt_m2d_stats local{};
    if (!stats) {
        stats = &local;
    }
    std::memset(stats, 0, sizeof(*stats));

    if (!options) {
        set_error(stats, "options is null");
        return 1;
    }
    if (!options->input_path || options->input_path[0] == '\0') {
        set_error(stats, "input_path is required");
        return 1;
    }
    if (!options->output_path || options->output_path[0] == '\0') {
        set_error(stats, "output_path is required");
        return 1;
    }
    if (options->night_mode < XT_M2D_NIGHT_AUTO_NEXT_DAY
            || options->night_mode > XT_M2D_NIGHT_SAME_DAY) {
        set_error(stats, "invalid night_mode");
        return 1;
    }

    try {
        std::string error;
        if (!convert(*options, *stats, error)) {
            set_error(stats, error);
            return 2;
        }
        return 0;
    } catch (const std::exception& exception) {
        set_error(stats, exception.what());
        return 3;
    } catch (...) {
        set_error(stats, "unknown error");
        return 3;
    }
}

extern "C" const char* xt_m2d_version(void) {
    return "1.0.0";
}

// endregion
