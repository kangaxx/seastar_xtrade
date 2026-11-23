# Seastar 数据导入功能操作指南

## 菜单语言（2026-11-23）

主菜单 **14) Language / 语言**：`1` 选择 English，`2` 选择简体中文，`u` 或回车保持当前语言。
切换立即作用于主菜单、行情导入、数据来源、回测设置、CTP 账号选择/别名及 START/SHOW 交互。
当前会话启动的 CTP 配置工具和采集器继承语言，不改变账号、密钥或数据文件。
默认英语；可在启动前设置 `XTRADER_LANGUAGE=zh`（或 `en`），非法值会明确报错。
选择只更新当前进程及其子进程环境，不保存到磁盘，也不更改调用终端或下一次启动的语言。

菜单编号及 `START`、`SHOW`、`INITIALIZE`、`IMPORT`、`SAVE`、`u` 等命令不翻译；
路径、数据来源名、周期、账号字段标识和凭据原值不翻译。系统/CTP 返回的错误及底层诊断保留原文。
终端需能显示 UTF-8。本轮语言相关 C++ 用例和完整 CLI 编译仍待 LD 验证。

## 统一数据目录约定（2026-11-13）

用户已确定：所有数据放在 Linux 的 `/data` 下，按来源分成以下目录。
用户确认目前采集数据位于 `/data/x_trader_data`；这是部署现状说明，本次未访问服务器核实文件。

| 来源目录 | 含义 | 注意事项 |
|---|---|---|
| `/data/x_trader_data` | 本系统实时采集 | 保留现有数据；过去采集的文件仍属于本来源，不因时间变旧移入 `historic` |
| `/data/historic` | 真实历史数据，例如官方网站下载 | 必须记录实际提供方；不能仅凭目录名认定来自官方 |
| `/data/taobao` | 淘宝购买的数据 | 保存供货批次及原始材料；是否为真实行情、复权或拼接数据须分别核实 |
| `/data/virtual_data` | 虚拟生成的测试数据 | 可为期货、其他金融产品或非金融时间序列；必须显式标记生成来源 |

新增来源时，在 `/data` 下添加独立来源目录，并登记来源定义，不塞入上述不相符的类别。
来源与用途分开：真实采集数据可用于回测，但仍属于 `x_trader_data`；用真实行情进行模拟撮合，
不会把输入数据变成 `virtual_data`。CTP 模拟柜台行情是否真实也须按实际来源核实，不能根据账户环境推断。

### 类别目录下的统一子目录设计

统一路径类 `market_data_paths` 按以下布局生成及解析路径，不设独立“品种”目录。
CLI 数据浏览及回测菜单的默认根目录由此类提供。2026-11-23 起，淘宝 CSV、规范 CSV
导入和只读 CTP 采集都使用 `market_data_writer` 这一套文件后端；历史读取器直接发现标准日文件。
代码仍待 LD 编译验证，见
[CTP 只读实时行情采集操作指南](CTP只读实时行情采集操作指南.md)。
本轮不接通 CLI 的回测 Run，也不转换内存回放/旧录制格式。

```text
/data/<source>/<asset_class>/<exchange>/<instrument>/<timeframe>/
    <YYYYMMDD>.<format>
    dataset-manifest.conf
```

| 层级 | 规则与示例 |
|---|---|
| `source` | 上述四个来源名；新增来源需单独登记 |
| `asset_class` | 产品类型，例如 `futures`、`equity`、`forex`、`crypto`、`timeseries`；非期货数据不得默认填 `futures` |
| `exchange` | 标准化交易场所，例如 `SHFE`；非交易数据使用 `NONE`；未知金融交易场所不能伪装为 `NONE` |
| `instrument` | 保留完整合约/产品标识，例如 `rb2601`、`EURUSD`、`temperature_sensor_01`；不能仅靠前两位字符生成标识 |
| `timeframe` | `tick` 或正整数加 `s/m/h/d/week`，例如 `5s`、`2h`、`4h`、`2d`、`1week`；`1w` 规范为 `1week`。`native` 只保留路径支持，不进入 Tick/K 线 CSV codec |
| `YYYYMMDD` | 显式事件日历日期；类校验真实日历日期及闰年，不推断交易日 |
| `format` | 小写字母/数字扩展名，例如 `csv`、`jsonl`、`parquet`；路径支持不代表已经支持该格式的读写 |

目录标识限定 ASCII 字母、数字、下划线和连字符，并以字母或数字开头；
拒绝空值、路径分隔符、点组件及控制字符，不静默改名。需要编码的产品标识由后续适配器
显式处理并在描述文件保留原值。大小写保持调用方的登记值，不自动大写或截断合约。
提供方、批次和序列类型保留在元数据中，不再各增加一层目录。实际合约和连续序列使用不同的
显式产品标识，例如 `rb2601` 与 `RB_CONTINUOUS`，不得通过 `9999` 后缀自动推断拼接规则。

示例：

```text
/data/x_trader_data/futures/SHFE/rb2601/tick/20261113.csv
/data/x_trader_data/futures/SHFE/rb2601/1m/20261113.csv
/data/historic/futures/SHFE/rb2601/1d/20261113.csv
/data/taobao/futures/SHFE/RB_CONTINUOUS/1m/20261113.csv
/data/virtual_data/timeseries/NONE/temperature_sensor_01/native/20261113.jsonl
```

示例只是命名演示，不证明这些提供方、数据集或对应产品文件实际存在。
Tick 与 bar 必须分目录；从 Tick 聚合出的分钟数据保留采集批次及输入关联，不得冒称逐笔数据。
导入不移动或改写外部原始材料；原始包封存及完整来源元数据仍待实现。
统一 writer 不提供任意目的文件名接口，不允许借此绕过标准日文件路径。
同来源、同产品、同周期、同日同格式会得到同一路径；不能据此自动覆盖或合并不同批次。

### 文件与数据集描述

- 原始材料的保留与清洗转换由后续导入器负责，记录 SHA-256，不覆盖已封存文件；
  路径类不转换或写入内容；当前导入只做已支持 CSV 的规范化，不生成原始材料校验清单。
- 日文件按明确时区的事件日历日期命名。高频按日分区；日线按年或合约汇总的专用文件命名
  尚未实现，不能自行拼接路径代替共用类。导入按日使用临时文件、fsync 和原子替换；实时 Tick 使用受锁保护的追加。
- 夜盘的日历日期和交易日分别保存，不能从文件日期推断交易日。bar 必须注明时间戳表示开盘还是收盘。
- 新标准日 CSV 不需要 `active_file` manifest。读取器按同来源/资产/场所/产品/周期分组，
  排序读取该组的多个日文件，并核对 schema 和每行日期。
- 旧 `dataset-manifest.conf` 继续支持单个 `active_file`，引用必须在自身数据集目录内，
  不接受 `../` 或符号链接；状态显示 `legacy-manifest`，不伪装成新布局。
- 后续完整描述文件仍须记录布局/格式版本、来源、提供方、产品类型、场所、原始及规范化产品标识、
  序列类型、数据周期、批次、时间域/时区、交易日规则、字段单位、覆盖范围、完整性及文件校验值。
- 连续合约补充实际合约映射、换月和复权规则；聚合数据补充输入文件校验值及转换版本。
  虚拟数据补充生成器版本、参数、随机种子（不适用时明确注明）和生成输入。
- 非期货数据不能仅补齐 CSV 表头后交给期货账务/策略。产品类型、字段单位、时间语义及能力必须经适配校验；
  当前新数据从路径取得来源及资产类型，字段/schema/日期冲突时拒绝加载；
  来源目录不证明真实性，时区、时间戳开收盘语义、批次及复权信息仍需显式补充，不能声称本轮已完整实现。

来源目录不等于交易环境或授权。真实行情也可供本地模拟使用；实盘、柜台模拟、实时本地模拟和
虚拟测试的写入会话/命名空间仍须隔离，目录中不得保存交易凭据。
回放、账务报表和测试输出不应混入输入数据集；运行产物、账户快照和 WAL 的路径不属于本轮行情路径类，
后续明确各自规则，不借行情来源目录保存交易凭据或账户状态。

### 路径类接口与边界

头文件：`include/seastar/xtrader/market_data_paths.hh`；
实现：`src/xtrader/market_data_paths.cc`。

| 接口 | 用途 |
|---|---|
| `source_root(source)` | 返回登记的来源目录；省略参数时返回本系统采集目录 `x_trader_data` |
| `source_directories()` | 读取根目录下实际存在的直接子目录并排序；不递归、不创建目录、不包括普通文件或符号链接 |
| `dataset_directory(key)` | 返回来源/资产类别/交易所/合约或序列/周期目录 |
| `data_file(key, calendar_date, format)` | 返回标准日文件路径；默认扩展名 `csv` |
| `manifest_file(key)` | 返回该数据集描述文件路径 |
| `parse_data_file(path)` | 将绝对标准文件路径还原为五个目录标识、日历日期及格式 |
| `normalize_timeframe(value)` | 校验周期并统一周线别名，不从数据间距推测周期 |
| `for_data_file(path, selected_root)` | 统一读取器的布局识别；扫描范围是公共根或一个来源，不接受额外目录层级 |
| `validate_filesystem_path(path)` | 显式检查日文件/manifest 的现有路径，不允许根目录之下的符号链接重定向 |
| `validate_contained_path(root, path)` | 新旧读取统一使用的目录/链接边界检查 |

构造、生成及解析只做字符串/路径校验，不创建目录，不读写文件。无效参数抛出
`std::invalid_argument`，文件系统检查失败抛出 `std::filesystem::filesystem_error`。
生产默认根目录为 `/data`；测试可显式提供隔离的绝对根目录，不能使用整个文件系统根。
原始路径中的 `.`/`..` 被拒绝，不通过规范化隐藏越界；相邻名称目录也不能通过前缀匹配混入根目录。

`validate_filesystem_path` 会访问文件系统，应由启动流程或 I/O worker 显式调用，不在 reactor
热路径中同步调用。该检查允许数据根本身是受控挂载/链接，但拒绝其下已存在的链接；
它不消除检查与打开之间的竞态。后续文件后端仍需使用平台适合的受控打开/原子发布措施。
本类不认证来源真实性、不批准实盘、不判断某个金融产品是否受当前策略/账务支持。

### CLI 数据来源菜单

CLI 启动时通过 `market_data_paths::source_root()` 取得默认采集目录，
不在 CLI 内重新写死 `/data/x_trader_data`。主菜单显示当前来源及完整目录。

1. 选择 `7) Select data source`，列出 `/data` 下实际存在的直接子目录名称及编号，
   标记默认目录和当前选择；不是只显示四个预设名称。
2. 输入编号切换，按 Enter 保持当前选择，输入 `u` 返回。输入无效编号会明确报错，
   不接受手工路径跳转；目录无法列出时显示错误，保持原选择，不自动切换其他来源。
3. 返回后选择 `4) Data Brief`，将所选目录作为构造参数传给 `historical_data_manager`；
   `5) Config Info` 同时显示默认和当前目录。

选择仅在当前 CLI 会话中生效，不写配置、不创建缺失目录、不迁移文件。
默认采集目录缺失时也不会自动改选其他来源。来源目录的存在不证明数据质量或真实性。
现有非标准名称目录也可以作为只读浏览根目录被选择；目录发现不会自动将它登记为
标准日文件来源，`market_data_key` 的名称校验及显式注册规则保持不变。
此处的来源选择仅影响 Data Brief，不自动覆盖回测子菜单的独立选择，
也不改变导入菜单的源文件/目标目录或录制 sink 配置。

### 回测菜单链（Run 返回“施工中”）

主菜单新增 `8) Backtest setup (Run is under construction)`。这是准备参数和演示交互的入口，
不是可执行交易回测的正式入口，独立 `xtrader_replay` 程序仍禁用。

| 子菜单 | 行为 |
|---|---|
| `1) Select data source` | 复用来源选择菜单；进入回测菜单时独立默认本系统采集目录，不继承 Data Brief 的临时选择 |
| `2) Select product type` | 按所选来源下的实际一级目录筛选；选中后纳入该目录下的数据文件，也可选全部 |
| `3) Select data files` | 可不经过类型筛选直接选择一个或多个文件；输入如 `1,3` 或 `1 3`，按输入顺序保存选择 |
| `4) Select strategy` | 当前仅提供 `Virtual` 占位项，不创建实际策略对象，不产生交易信号 |
| `5) Run` | 检查已选择文件和策略，组装 `backtest_config` 后调用占位函数；中文显示“施工中”，英文显示“Under construction” |

产品类型筛选只按实际目录分组，不证明其金融产品类型；旧布局的 `dominant` 等目录也只是分组，
不能据此认定数据是真实期货。来源/类型改变后，不混用旧文件选择：
切换来源清空类型和文件；重新选择类型则用该分组的实际文件替换旧选择。
取消选择保持原配置，缺目录、空数据或无效/重复编号明确提示，不自动填充示例文件。

文件列表递归发现当前分组下的 `.csv`、`.jsonl`、`.parquet` 和 `.zip` 普通文件，
排除符号链接、manifest 及 `.partial` 临时文件。这只是菜单候选类型，不表示执行器已经支持这些格式。
Run 前再次检查所选来源和文件存在；选择仅保存在本次回测菜单会话内，不修改配置文件或数据。

`backtest_config.data_root` 的默认值已改由路径类生成；菜单 Run 时显式写入所选 `data_root`、
`data_files`、`product_type`、`strategy_id`。其中后三项是本轮新增的菜单准备参数，
**现有 `backtest_driver` 还没有使用它们进行指定文件加载或策略执行**。
Run 不创建驱动、不解码数据、不启动 event bus/撮合/账务、不生成收益报表，
也不把“施工中”显示成回测成功；真正执行链路后续另行开发。
导入器仍用独立 `import.conf` 配置外部输入和报告，但行情目的路径已经改为统一布局。

### 独立编译与用例（LD 上由用户执行）

用户已回传 2026-11-13 的首版路径类独立编译及用例通过日志；该结果不覆盖本轮新增的目录发现和 CLI 菜单。
本机编译环境仍缺标准库头文件及 SDK，**本轮 C++ 编译和执行结果待 LD 回传**。
脚本现在分别编译路径类、CLI 来源菜单及回测准备菜单用例，不依赖完整 Seastar 构建、
CTP 或 Redis，不运行其余回归，不启动完整 CLI。

在 LD 的仓库根目录运行：

```bash
bash scripts/test_xtrader_market_data_paths.sh
```

需已有支持 C++20 的 GCC/Clang；默认 `g++`，可以显式选择：

```bash
CXX=clang++ bash scripts/test_xtrader_market_data_paths.sh
```

覆盖四来源路径、无品种层、生成/解析一致、非期货标识、非法日期/闰年、未知来源、
空标识/分隔符/点组件/越界路径，以及 Linux 下的跨来源、根外及悬空符号链接。
文件格式名只验证命名，不执行 CSV/Parquet 解码。测试使用 `/tmp` 下独占的临时目录，
不访问 `/data`；结束清理自身文件，失败返回非零退出码。
成功应出现 `[OK] market_data_paths compilation and standalone cases passed.`。
新增菜单用例覆盖默认来源、实际目录发现、四类及新来源选择、非法编号重试、取消/EOF、
缺失/空目录以及文件/符号链接不参与选择，成功应出现
`[OK] CLI data-source discovery and selection cases passed.`。
回测菜单用例覆盖默认/其他来源、类型全选、单文件/多文件、Virtual 选择、
无效输入、切换时清理旧选择、空/缺失数据及占位 Run 的参数传递，
成功应出现 `[OK] Backtest source/product/files/strategy selection and placeholder Run passed.`。

完整 CLI 已在 CMake 中增加路径类实现的链接；菜单独立用例通过不等于完整 CLI 构建通过。
已有 Release 构建配置时，可单独编译 CLI，不运行交易程序或回归：

```bash
cmake --build build/release --target app_xtrader_cli --parallel 1
```

上述路径/菜单脚本不覆盖 2026-11-23 新增的 writer/导入/读取修改。
本轮新增独立回归脚本，需在 LD 运行：

```bash
bash scripts/test_xtrader_market_storage.sh
bash scripts/test_xtrader_market_capture.sh
```

前者覆盖多来源、不同周期、多日分区、增量/覆盖、真实 Tick serializer 到导入再读取、
旧 manifest、错误 schema/日期、损坏尾行、锁冲突及链接边界；只链接 OpenSSL，
不链接 CTP 动态库、不连接市场服务器、不访问生产 `/data`。本轮未在 Windows 编译或执行 C++ 用例。

### 当前兼容边界与实施顺序

**新写入已经统一；没有自动迁移、删除或覆盖旧汇总文件。**
旧 `<source>/<domain>/<exchange>/<symbol>/<timeframe>/*.imported.csv` 通过受边界限制的
manifest 兼容读取，不能根据旧目录名认定其真实来源或资产类型。
新日文件与旧 manifest 同时混入一个周期目录时明确报错，不暗中丢掉其中一套输入。
跨来源有同场所/产品/周期的数据时必须选择来源，查找歧义会报错。期货回测驱动拒绝新布局下
非 `futures` 数据及 Tick 文件；原始 Tick 可通过 `load_ticks()` 读取，但本轮没有实现 Tick 转 K 线。

共享 writer 使用 `0750` 创建目录、`0640` 创建数据和锁文件（受 umask 影响），不递归 chmod/chown。
数据根的父目录须已存在。实时追加保留 schema 和完整尾行检查；导入先验证整个输入文件，
再按日合并已有数据，已有损坏文件会报错，不静默清洗。
原子性按日文件提供，不是跨全部日文件的事务；后续日期写入失败时，已经发布的日期可能保留，
本次导入会返回失败，应保留报告并核查，不自动回滚生产数据。

## 功能概述
Seastar 的数据导入功能主要用于将历史数据（如 CSV 文件）导入到系统中，以支持回测和实盘分析。该功能支持多种数据源类型和导入模式，用户可以通过命令行界面（CLI）进行交互式操作。

## 数据导入的主要步骤

### 1. 设置导入参数
在 CLI 中，用户可以通过以下选项设置导入参数：
- **source path**: 数据源路径，支持文件或目录。
- **source type**: 输入适配器。`taobao_csv` 接受原有淘宝九列表头；`ctp_clean_csv`
  接受本系统完整原始 Tick 表头或规范 K 线 CSV（七列基础字段或九列完整字段），不是任意厂商格式的自动识别器。
- **import mode**: 导入模式，支持 `incremental`（增量导入）和 `overwrite`（覆盖导入）。
- **config path**: 明确选择的文件优先，其次 `XTRADER_IMPORT_CONFIG`，默认 `/xtrader_conf/import.conf`。
  显式选择却无法读取时报错，不使用默认配置伪装成功。
- **6) Set timeframe**: 显式选择 `tick`、`5s`、`2h`、`1d`、`1week` 等。
- **7) Set destination source**: 选择默认来源或合法自定义来源名，不接受任意路径。

### 2. 执行导入
设置完成后，选择 `Execute import` 开始导入。系统会扫描指定路径下的文件，并根据配置执行导入操作。

### 3. 查看导入结果
导入完成后，系统会输出以下信息：
- **导入状态**: 成功或失败。
- **扫描文件数**: 总扫描文件数、成功文件数、跳过文件数、失败文件数。
- **数据行统计**: 新增行数、覆盖行数、跳过行数、无效行数。
- **详细报告**: 每个文件的导入状态和统计信息。

## 配置文件说明
配置文件采用 `key=value` 格式，支持以下字段：
- `data_root`: 统一根目录，默认 `/data`，**不能再配置成 `/data/x_trader_data` 等来源目录**。
- `destination_source`: 目的来源；淘宝适配器默认 `taobao`，规范 CSV 适配器默认 `historic`；
  可显式设为 `x_trader_data`、`virtual_data` 或合法自定义来源，不改变 CSV 的 Tick/K 线类型。
- `asset_class`: 资产类别，淘宝期货适配默认 `futures`；其他资产必须显式填写。
- `instrument`、`exchange`: 一起设置时允许任意输入文件名；否则文件名必须是
  `<instrument>.<exchange>[.<timeframe>].csv`。产品标识保留大小写。
- `report_root`: 导入报告目录。
- `recursive_scan`: 是否递归扫描子目录。
- `timeframe`: 正整数周期或 `tick`；仍接受旧 `taobao_timeframe` 字段。
  未显式设置时先用文件名周期，K 线缺省仍为 `1m`，完整 Tick 表头缺省为 `tick`；
  缺省只是兼容旧行为，**不会推断真实周期**。显式周期与文件名或 schema 冲突时拒绝导入。
- `taobao_data_domain`: 已废弃；旧配置含此字段会报错，请改用来源和资产类别，不把 `dominant` 当资产类别。

示例配置：

```ini
data_root=/data
destination_source=taobao
asset_class=futures
timeframe=5s
report_root=/root/xtrader_import_reports
recursive_scan=true
```

结果示例：`/data/taobao/futures/SHFE/rb2601/5s/20261123.csv`。
K 线表头统一为 `date,open,high,low,close,volume,open_interest,money,source_symbol`，
时间保存为 `YYYY-MM-DD HH:MM:SS`（包括秒）；Tick 完整保留原始 schema、会话、接收序号、
ActionDay、TradingDay 及五档字段。日线/周线也按各行明确的时间戳日期分文件，不擅自转换日期或重新聚合。

## 示例操作
以下是一个完整的导入示例：
1. 启动 CLI 并进入导入菜单。
2. 输入外部文件或目录完整路径，例如 `/root/share_path/rb2601.SHFE.csv`；原始路径不受目的目录布局限制。
3. 设置数据源类型为 `taobao_csv`。
4. 设置导入模式为 `incremental`。
5. 设置周期为 `5s`、目的来源为 `taobao`，执行导入，再在 Data Brief 选择 `taobao` 来源查看。

## 注意事项
- 确保数据源路径和配置文件路径正确。
- `incremental` 跳过已有行标识；`overwrite` 仅替换输入中同标识的行，不删除未涉及日期/行。
  K 线标识是规范时间戳；Tick 标识保留事件时间、session_id 及 sequence，不按价格或秒钟去重。
- 任一无效输入行会拒绝该文件，不把缺列、非法日期、NaN、OHLC 错误或 schema 冲突静默丢掉。
- 导入报告会生成在指定目录下，可用于后续分析。

## 后续计划
- 增加更多数据源类型支持。
- 优化导入性能和错误处理。
- 提供更详细的导入日志和报告。