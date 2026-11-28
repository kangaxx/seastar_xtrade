// 分钟线期货数据 -> 日线期货数据 转换库（独立 SO，不依赖 seastar）。
//
// 数据口径（已按 rm9999 实际数据验证）：
//   - 输入时间戳是“自然日 + 时间”，不是交易日；
//   - 夜盘（21:00-23:00/23:30）在时间戳上属于当晚自然日，但属于下一个交易日；
//   - 因此默认把夜盘归入“该自然日之后的下一个交易日”，与日盘合并成一根日线。
//
// 输出列：date,open,high,low,close,volume,open_interest,money,source_symbol
//   date = 交易日（YYYY-MM-DD）
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 夜盘归属模式。
enum xt_m2d_night_mode {
    // 自动：本库按国内商品期货惯例处理为“夜盘归下一交易日”。
    XT_M2D_NIGHT_AUTO_NEXT_DAY = 0,
    // 夜盘归下一交易日（CZCE/DCE/SHFE 默认）。
    XT_M2D_NIGHT_NEXT_DAY = 1,
    // 夜盘归时间戳当日（用于已经按交易日打标签的数据源）。
    XT_M2D_NIGHT_SAME_DAY = 2,
};

typedef struct xt_m2d_options {
    const char* input_path;   // 输入分钟线 CSV，必填
    const char* output_path;  // 输出日线 CSV，必填
    const char* symbol;       // 可选：source_symbol 精确过滤，NULL/"" 表示全部
    const char* start_date;   // 可选：交易日下限 YYYY-MM-DD
    const char* end_date;     // 可选：交易日上限 YYYY-MM-DD
    int night_mode;           // xt_m2d_night_mode，0 表示默认“夜盘归下一交易日”
} xt_m2d_options;

typedef struct xt_m2d_stats {
    long long input_rows;             // 读入的数据行（不含表头）
    long long output_rows;            // 输出的日线条数
    long long skipped_rows;           // 非法/被过滤的行
    long long unassigned_night_rows;  // 找不到下一交易日的夜盘行
    char error[512];                  // 失败原因（UTF-8）
} xt_m2d_stats;

// 返回 0 成功，非 0 失败；stats 可为 NULL。
int xt_m2d_convert(const xt_m2d_options* options, xt_m2d_stats* stats);

// 版本号，便于排查调用方加载的是哪一版 SO。
const char* xt_m2d_version(void);

#ifdef __cplusplus
}
#endif
