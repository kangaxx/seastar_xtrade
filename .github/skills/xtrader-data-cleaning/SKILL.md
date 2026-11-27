# xtrader-data-cleaning

## Description

X-Trader 行情数据（CTP tick）落盘口径的权威记录，以及已知脏数据问题的清单。
任何数据清洗、补齐、合并、回测取数工作，都必须先按本文档的口径理解数据，再动手处理。

适用场景：
- 需要读取/合并 `/data/x_trader_data` 下的历史 tick 数据；
- 需要判断某个交易日的数据是否完整、是否可用；
- 开发数据清洗、校验、补齐功能前的口径确认。

不适用场景：
- CTP 采集器本身的编译与部署（见 `xtrader-build` skill）；
- 实时行情链路的运行状态诊断（见 `docs/xtrader_debug_guide.md`）。

## 一、落盘口径（本系统的事实约定）

### 1.1 路径与命名

```
/data/x_trader_data/futures/<EXCHANGE>/<INSTRUMENT>/tick/<YYYYMMDD>.csv
/data/x_trader_data/futures/<EXCHANGE>/<INSTRUMENT>/tick/<YYYYMMDD>.csv.lock
```

- 文件名中的 `<YYYYMMDD>` **取自 CTP 行情回报的 `ActionDay` 字段**（CSV 第 7 列），
  不是本地时间，也不是 `TradingDay`（第 8 列）。
- 每个合约每个交易日一个文件，采集进程持有对应 `.lock` 文件，跨进程互斥。
- 示例（2026-11-27 实测）：`/data/x_trader_data/futures/DCE/c2701/tick/20261127.csv`。

### 1.2 CSV schema（42 列，无引号，逗号分隔，首行为表头）

| 列 | 字段 | 说明 |
|---|---|---|
| 1 | `schema_version` | 当前为 `1` |
| 2 | `session_id` | 采集会话 ID（每次进程启动生成新值，可用于识别重启） |
| 3 | `sequence` | 会话内递增序号 |
| 4 | `received_unix_ms` | 本地收到该 tick 的 Unix 毫秒时间戳 |
| 5 | `exchange` | 交易所：`DCE` / `CZCE` / `SHFE` |
| 6 | `instrument` | 合约代码，如 `c2701`、`MA701` |
| 7 | `action_day` | **CTP `ActionDay`（决定落盘文件名）** |
| 8 | `trading_day` | **CTP `TradingDay`（真实归属交易日，清洗必须用它）** |
| 9 | `update_time` | 交易所时间 `HH:MM:SS` |
| 10 | `update_millisec` | 交易所毫秒 |
| 11 | `last_price` | 最新价 |
| 12 | `volume` | 累计成交量 |
| 13 | `turnover` | 累计成交额 |
| 14 | `open_interest` | 持仓量 |
| 15–22 | `open_price` / `highest_price` / `lowest_price` / `pre_close_price` / `pre_settlement_price` / `pre_open_interest` / `upper_limit_price` / `lower_limit_price` | 日内与结算参考价 |
| 23–42 | `bid_price1..5`、`bid_volume1..5`、`ask_price1..5`、`ask_volume1..5` | 五档盘口，顺序为 bid1, bid_vol1, ask1, ask_vol1, …, ask5, ask_vol5（第 42 列为 `ask_volume5`） |

### 1.3 三家交易所的 `ActionDay` 语义差异（重要）

同一晚夜盘（2026-11-26 21:00 → 11-27 02:30）实测：

| 交易所 | 夜盘 21:00 行的 `action_day` / `trading_day` | 落盘文件 | 结果 |
|---|---|---|---|
| DCE | `20261127` / `20261127` | `20261127.csv` | 夜盘归次日，符合交易习惯 |
| CZCE | `20261126` / `20261126` | `20261126.csv` | 夜盘归**当日** |
| SHFE | 23:59 行 `20261126` / `20261127`；00:00 行 `20261127` / `20261127` | 21:00–23:59 在 `20261126.csv`，00:00–02:30 在 `20261127.csv` | **按午夜切成两个文件** |

结论（清洗工作的第一条铁律）：

> **文件名不等于交易日。** 同一交易日的夜盘数据可能分散在相邻两个文件里，
> 也可能落在“看起来更早”的那一天的文件里。
> 归组、合并、对齐一律以 CSV 第 8 列 `trading_day` 为准，禁止用文件名推断交易日。

### 1.4 已知的会话与覆盖特征

- `session_id` 每次采集进程启动都会变；一个文件里出现多个 `session_id` 属正常现象
  （跨夜盘、盘中重启都会产生）。
- 采集重启后的第一帧往往是 CTP 推送的**缓存快照**，其 `update_time` 是上一根 tick 的时间，
  不代表新数据（见第二节第 1 条）。

## 二、已知脏数据问题（清洗功能的输入清单）

### 问题 1：登录缓存快照污染上一交易日文件（待清洗）

- 现象：采集进程登录 CTP 后，CTP 会补推一条上一交易日的缓存快照，程序会把它追加到
  **`ActionDay` 对应的那个文件**——若该 `ActionDay` 是上一交易日，就会写进旧文件。
- 实测证据（2026-11-27 07:44 启动的会话，session `c64cea2ff1c960009c734c08c457…`）：
  `CZCE/MA701/tick/20261126.csv` 的**最后一行**是该会话在 `23:00:00` 写入的
  `action_day=20261126, trading_day=20261126` 快照，而 20261126 的数据本应在 11-26 夜盘结束时就封存。
- 影响：旧文件的修改时间、行数、时间范围都被撑大；按“文件末行时间”判断数据完整性会误判。
- 清洗规则建议：识别 `session_id` 与文件的“正常会话集合”不符的行，或识别
  `update_time` 早于同文件内前一有效行的异常回退行；标记为 `seed` 后剔除或归档到旁路文件。
- 状态：**未处理**，等数据清洗功能开发时一并实现。

### 问题 2：夜盘跨文件（由第二节 1.3 的口径决定，不是错误）

- 现象：SHFE 夜盘被午夜切成两个文件（`20261126.csv` + `20261127.csv`）；
  CZCE 夜盘整体落在 `20261126.csv`。
- 这不是采集错误，是 `ActionDay` 命名规则的自然结果。
- 清洗要求：按 `trading_day` 重新归组后，再做“夜盘 + 日盘”拼接，避免重复或遗漏。

### 问题 3：数据缺口（待逐个核查）

- 已知样例：`DCE/c2701/tick/` 下存在 `20261125.csv`、`20261127.csv`，**缺 `20261126.csv`**，
  而同日的 CZCE、SHFE 文件存在。说明 11-26 白天 DCE 可能未启动采集。
- 清洗要求：先产出“应有交易日 × 实际文件”的缺口报告（按交易所工作日历），再决定补齐或标记。

### 问题 4：采集中断（运维侧事实，供完整性校验参考）

- 2026-11-27 02:41–07:44 出现 CTP 断线（`code=4097`）后连续 204 次登录/订阅 30 秒超时
  （约 90 秒一轮，退避上限 60 秒），期间无行情入库；07:44 自动恢复。
- 该时段落在夜盘收盘之后，未造成行情丢失，但清洗校验时应把日志里的中断窗口
  与 tick 缺口做交叉比对，而不是只看行数。

## 三、清洗/校验的推荐步骤

1. **按 `trading_day` 归组**：跨文件合并同一交易日的全部行，不要用文件名当交易日。
2. **去种子行**：剔除登录缓存快照（问题 1 的规则），保留原始文件不动，产出清洗后的新文件/新表。
3. **校验时间序**：以 `received_unix_ms` 排序，检查 `sequence` 单调性与 `session_id` 切换点。
4. **校验缺口**：按交易所交易时段（含夜盘）生成期望时间栅格，标注缺口区间及对应日志事件。
5. **校验字段合理性**：`last_price > 0`、五档价格非零时不越界、`volume` 单调不减（同一交易日）。
6. **输出报告**：每个文件一行（行数、首末时间、缺失区间、异常行数），便于人工复核。

## 四、常用只读核查命令

```bash
# 今日各产品文件概况（行数/大小/时间跨度/最大间隔）
for f in /data/x_trader_data/futures/*/*/tick/$(date +%Y%m%d).csv; do
  awk -F, -v n="$f" 'NR>1{c++} END{printf "%-50s rows=%d\n", n, c}' "$f"
done

# 按 session 拆分统计
awk -F, 'NR>1{n[$2]++} END{for(s in n) printf "%s %d\n", substr(s,1,8), n[s]}' <file>

# 每小时行数分布（判断夜盘/日盘覆盖）
awk -F, 'NR>1{split($9,a,":"); c[a[1]]++} END{for(h in c) printf "%s %d\n", h, c[h]}' <file> | sort
```

## 五、约束

- 清洗过程**只读原始文件**，输出写入新路径；不原地修改已封存的历史文件。
- 任何“补齐/插值/丢弃”的决定都要在报告里留痕（规则、影响行数、时间区间）。
- 本 skill 记录的是**当前系统的既有事实**，口径变更（例如改为按 `trading_day` 命名）
  必须先更新本文档再改代码。

## Related Files

- `docs/data_import_guide.md`（导入器与原始材料保留策略）
- `docs/xtrader_debug_guide.md`（采集器运行与排障）
- `.github/skills/xtrader-build/SKILL.md`（编译与环境）
- 采集端源码：`src/xtrader/`（tick 落盘实现）
