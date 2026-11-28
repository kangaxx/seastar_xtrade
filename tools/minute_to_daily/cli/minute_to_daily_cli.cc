// 分钟线 -> 日线转换库的命令行测试入口。
#include "xtrader/minute_to_daily.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void print_usage(const char* program) {
    std::printf(
        "usage: %s --input <minute.csv> --output <daily.csv> [options]\n"
        "  --symbol <source_symbol>   only keep this source_symbol\n"
        "  --start <YYYY-MM-DD>       trading-day lower bound\n"
        "  --end <YYYY-MM-DD>         trading-day upper bound\n"
        "  --same-day                 treat night session as the same calendar day\n"
        "  --version                  print library version\n",
        program);
}

bool take_value(int argc, char** argv, int& index, const char* name, const char*& value) {
    if (std::strcmp(argv[index], name) != 0) {
        return false;
    }
    if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return false;
    }
    value = argv[++index];
    return true;
}

} // namespace

int main(int argc, char** argv) {
    xt_m2d_options options{};
    options.night_mode = XT_M2D_NIGHT_NEXT_DAY;

    const char* input = nullptr;
    const char* output = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--version") == 0) {
            std::printf("%s\n", xt_m2d_version());
            return 0;
        }
        if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        const char* value = nullptr;
        if (take_value(argc, argv, index, "--input", value)) {
            input = value;
        } else if (take_value(argc, argv, index, "--output", value)) {
            output = value;
        } else if (take_value(argc, argv, index, "--symbol", value)) {
            options.symbol = value;
        } else if (take_value(argc, argv, index, "--start", value)) {
            options.start_date = value;
        } else if (take_value(argc, argv, index, "--end", value)) {
            options.end_date = value;
        } else if (std::strcmp(argv[index], "--same-day") == 0) {
            options.night_mode = XT_M2D_NIGHT_SAME_DAY;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!input || !output) {
        print_usage(argv[0]);
        return 2;
    }
    options.input_path = input;
    options.output_path = output;

    xt_m2d_stats stats{};
    const int result = xt_m2d_convert(&options, &stats);
    std::printf("input_rows=%lld output_rows=%lld skipped_rows=%lld unassigned_night_rows=%lld\n",
                stats.input_rows, stats.output_rows, stats.skipped_rows,
                stats.unassigned_night_rows);
    if (result != 0) {
        std::fprintf(stderr, "convert failed (%d): %s\n", result,
                     stats.error[0] ? stats.error : "unknown error");
        return result;
    }
    std::printf("ok: %s\n", output);
    return 0;
}
