# Seastar xtrader 调试说明

本文档用于在 Ubuntu 虚拟机中对 Seastar 的 xtrader 目标进行编译、运行与调试。

功能补齐、六阶段状态与替换验收以[开发计划修订版](xtrader_development_plan.md)为准。
当前 CLI/demo 不是完整交易入口。四个独立模式程序目前只提供帮助并拒绝运行，
CTP 网关启动也被禁止；不能因编译或 `--help` 检查通过就认为可以进行交易。

**2026-11-10 当前约定：**VBox Linux 未启动，暂不执行本文中的连接、同步、
编译或测试命令。B5–B8 本轮新增代码/工具均未运行验证；第 9 节仅列出后续操作。

## 1. 前提

- 虚拟机系统：Ubuntu 22.04+
- 仓库路径：`/home/vboxuser/workspace/seastar`
- 推荐通过 SSH 别名连接：`xtrader-vbox`

连通性检查：

```bash
ssh xtrader-vbox "echo VM_OK; uname -a; whoami"
```

## 2. 一次性环境准备

安装依赖（首次）：

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; sudo ./install-dependencies.sh'"
```

检查关键工具：

```bash
ssh xtrader-vbox "bash -lc 'python3 --version; cmake --version | head -n 1; ninja --version; gdb --version | head -n 1'"
```

## 3. 配置与编译

### 3.1 配置（首次或依赖变化后）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk'"
```

### 3.2 编译 xtrader

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ninja -C build/debug app_xtrader_cli app_xtrader_demo -j2'"
```

### 3.3 检查产物

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ls -lh build/debug/apps/xtrader/xtrader_cli build/debug/apps/xtrader/xtrader_demo'"
```

### 3.4 一键脚本（Debug / Release）

这两份是 LD/阿里云 Linux 服务器使用的完整 xtrader 构建脚本。
将**当前完整工作区**同步到服务器后，在仓库根目录手动运行；本轮新增文件尚未提交，
仅同步两份脚本或拉取旧提交不能获得新增源码。脚本本身不执行上传、部署或服务切换。
根目录另有旧 `build_debug.sh` / `build_release.sh`，它们仅构建 CLI，
不是本节覆盖 B5–B8 组件的构建入口。

```bash
bash ./build_xtrader.sh
bash ./build_xtrader_demo_release.sh
```

两个脚本分别配置 Debug / Release，编译 CLI、demo、四个禁用中的独立入口和
`xtrader_phase6_probe`。B5 行情/格式/本地模拟及 B6 恢复协议源码通过 demo 的 CMake
源列表编译，不需要手工逐个添加 `.cc` 到脚本。
还会对 B7/B8 的 `tools/xtrader_acceptance.py` 及其用例执行 Python 语法检查，
不运行它们、不产生 `.pyc`。
**默认只编译及检查 Python 语法，不启动产物、不执行回归或长稳，也不连接柜台。**
Release 产物均位于 `build/release/apps/xtrader/`，不会覆盖 `build/debug` 中的产物。

默认 `JOBS=1`，适合低内存云服务器；确认内存充足后可显式提高。
需要原有的 `--help` 冒烟检查时显式启用（仍不运行回归）：

```bash
RUN_HELP_CHECKS=1 bash ./build_xtrader_demo_release.sh
```

两者均支持 `STALL_INTERVAL_SECONDS`（默认 60 秒）：阶段无输出时提示仍在运行，不会自动终止编译。
Debug 完整日志保存在 `build/logs/build_xtrader_*.log`，
Release 完整日志保存在 `build/logs/build_xtrader_demo_release_*.log`。
构建命令或日志写入失败均返回非零，不将日志管道错误当作成功。
依赖准备要求与 Debug 相同；检测到 Conda 时只给出警告，不自动退出或清理环境。

## 4. 运行方法

### 4.1 CLI 入口（推荐先测）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_cli --help | head -n 30'"
```

### 4.2 Demo 入口

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_demo --help | head -n 30'"
```

### 4.3 独立模式入口：仅占位，不能交易

| 程序 | 预留用途 | 当前状态 |
|---|---|---|
| `xtrader_replay` | 历史回放/纯模拟 | 未实现，拒绝运行 |
| `xtrader_paper` | 实时行情与本地模拟撮合 | 未实现，拒绝运行 |
| `xtrader_ctp_sim` | CTP 柜台模拟盘 | 未实现，拒绝运行 |
| `xtrader_live` | 实盘 | 未实现且默认禁用 |

例如，`./build/debug/apps/xtrader/xtrader_replay --help` 只显示禁用说明。
除单独的 `--help` 或 `-h` 外，所有调用均返回非零状态，不读取配置、不连接柜台。
普通 CLI 不提供交易菜单，旧的选项 1/2 也只报错。
这些限制并不等于已完成账户、前置、凭据及状态的可信环境隔离；
任何 CTP 连接、模拟柜台或实盘验证均须先完成相应开发并另行授权。

CLI 的 `Config Info` 现在只显示未验证的环境声明和匿名序号，不显示账户信息、
不解密配置，也不再创建账户配置或收集密码。未知、重复或无法识别的环境声明
不会显示为模拟盘。显示目录默认为 `/etc/xtrader`，可使用环境变量
`XTRADER_CLI_CONFIG_ROOT` 指定其他目录；这个变量只影响 CLI 的只读展示，
不构成交易模式或账户/前置的可信绑定。
已有配置文件不会被修改；旧的 `ENCv1` 是不安全的自制方案，不能视为可靠的凭据保护。
安全凭据管理及旧凭据迁移仍未实现，在此之前不要将真实凭据交给本程序。

策略运行时必须先停止并排空 `event_bus`，再停止策略管理器；
停机期间不再接受新行情。当前分发按事件及策略回调串行等待，慢回调可能造成背压，
不能据此推断生产性能。策略行情回调失败时，策略进入失败状态，事件总线拒绝新事件，
排空后返回错误；需要人工处理失败原因，不会自动重新开启。
分片运行时目前仅支持 shard 0，并须显式指定离线初始资金；
这不是柜台资金同步，更不能用作实盘账户。
生命周期管理器、订阅表及处理器引用的对象必须存活至停止 future 完成。
分片运行时的构造方式已改为 `runtime_sharded_config`，下单/成交处理器安装返回
future，调用方必须等待安装完成；旧的按 shard 数字构造和不等待安装的用法需调整。
当前成交处理器安装和成交入账会明确失败，因为新的持仓账本尚未与资金结算和回报
路由接通；不得依赖旧的单持仓簿或将这一失败当作成交已记账。
当前价格校验和市价单规则尚未完成交易所级适配，不能据此投入交易。

## 5. GDB 调试

### 5.1 调试 xtrader_cli

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; gdb ./build/debug/apps/xtrader/xtrader_cli'"
```

GDB 常用命令：

```gdb
set args --help
run
bt
info threads
thread apply all bt
quit
```

### 5.2 调试 xtrader_demo

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; gdb ./build/debug/apps/xtrader/xtrader_demo'"
```

## 6. 常见问题与处理

### 6.1 报错：找不到编译器或 cmake/ninja

现象：`configure.py` 报 `FileNotFoundError`。

原因：命令在 Windows 本机执行，而非 Ubuntu VM。

处理：确保命令通过 SSH 在 VM 中执行。

### 6.2 报错：PowerShell 引号/管道被提前解析

现象：远程命令中的 `|`、引号行为异常。

处理建议：
- 尽量将长命令拆成多条短命令；
- 必要时用 `C:\Windows\System32\OpenSSH\ssh.exe` 调用 ssh；
- 或直接登录 VM 后本地执行。

### 6.3 仅一个目标编译成功

排查顺序：
1. 单独编译失败目标：
   ```bash
   ninja -C build/debug app_xtrader_demo -j2
   ```
2. 检查产物是否生成：
   ```bash
   ls -lh build/debug/apps/xtrader/
   ```
3. 必要时清理该目标相关对象后重编。

## 7. 推荐日常流程（最短）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ninja -C build/debug app_xtrader_cli app_xtrader_demo -j2; ./build/debug/apps/xtrader/xtrader_cli --help | head -n 20'"
```

如果依赖或配置发生变化，先补跑 `configure.py --mode=debug --disable-dpdk`。

## 8. 第六阶段：指标、门禁与离线验证工具

本节对应第六阶段的**代码与工具**，不表示该阶段已完成实盘验收。
工具不读取账户配置、不连接 CTP/Redis、不发送交易请求，也不操作云服务器。

### 8.1 构建与回归

在 Linux 项目根目录执行（需要现有 C++20 编译器，默认 `g++`）：

```bash
bash ops/phase6/build_and_check.sh
```

该命令直接构建真实的 `performance_metrics` / `health_check` 源文件，执行指标与门禁回归，
再生成 `build/phase6/xtrader_phase6_probe`。不依赖 Seastar 全量构建或 CTP 动态库。
测试覆盖直方图边界、分位数上界、分组件错误率、队列深度、滚动丢弃窗口、
空数据拒绝通过、阈值边界、重置和并发读写。编译器可通过 `CXX` 指定为一个可执行文件路径。

已有 Seastar 构建目录时，也可以使用原构建系统：

```bash
ninja -C build/debug test_unit_xtrader_phase6_metrics test_unit_xtrader_phase4_minimal app_xtrader_demo app_xtrader_phase6_probe
ctest --test-dir build/debug -R 'Seastar.unit.xtrader_(phase6_metrics|phase4_minimal)' --output-on-failure
```

### 8.2 合成负载与长时间观测

```bash
bash ops/phase6/run_probe.sh --seconds 10 --rate 1000 --mode mixed
bash ops/phase6/run_probe.sh --seconds 10 --rate 1000 --mode market_data
bash ops/phase6/run_probe.sh --seconds 10 --rate 1000 --mode order
```

探针使用标准库的有界队列和生产/消费线程，实际调用指标和健康检查代码。
`order` / `trade` 仅为合成事件类型，不生成委托；这不是 Seastar reactor 或 CTP 的吞吐测试。
`--rate` 是目标投递速率，实际投递量受调度与采集开销影响，必须结合 `attempted` 和
`elapsed_seconds` 解读，不得把目标速率写成实测吞吐量。

按需要显式执行 8h / 24h **离线**长稳观测：

```bash
bash ops/phase6/run_probe.sh --seconds 28800 --rate 1000 --mode mixed
bash ops/phase6/run_probe.sh --seconds 86400 --rate 1000 --mode mixed
```

每秒输出 JSONL：各组件的样本数、错误率、累计/最近一分钟丢弃数、当前/峰值队列深度、
平均/最大延迟与 P50/P95/P99 上界；Linux 下还包含进程峰值 RSS 和当前文件描述符数量。
记录存入唯一的 `build/phase6/evidence_*` 目录，含命令、主机、UTC 时间、Git 提交、
工作区修改状态、探针二进制 SHA256、标准错误及退出码。失败结果同样保留，不生成虚构通过证据。

退出码：`0` 仅表示本次合成负载门禁通过，`1` 表示门禁失败，`2` 表示输入或运行错误。
任一周期失败会保留失败状态；结束时仍有队列积压也会失败。
生产结束后的排空窗口默认 5 秒，可用 `--drain-seconds` 调整；
窗口结束时正在执行的单个模拟处理仍会结束，注入延迟最多额外 1 秒（另受操作系统调度影响）。
`pending` 为未处理事件，最终满足 `attempted = consumed + dropped + pending`。

失败路径示例（应返回非零，不要当作环境故障）：

```bash
bash ops/phase6/run_probe.sh --seconds 2 --error-every 1 --mode mixed
bash ops/phase6/run_probe.sh --seconds 2 --rate 10000 --capacity 2 --delay-us 10000 --max-drops 0
```

### 8.3 指标与门禁的精确定义

- P50/P95/P99 来自固定 65 桶 log2 直方图，是保守上界而非精确分位数：
  正数 `x` 所在桶的上界为 `2^bit_width(x)-1`，再以观测最大值截断；内存不会随运行时间增长。
- 延迟、错误率和峰值队列是本次进程启动/`reset()` 后的累计口径。
  错误率为该组件的 `errors / (successes + errors)`，不再拿全局错误除以其他组件事件数。
  延迟样本与处理结果分别记录；一次事件分发可触发多个策略回调。
- 丢弃门禁使用按秒分桶的保守 60～61 秒窗口，不是累计丢弃数冒充“每分钟”。
- 默认平均延迟上限 1000μs、P99 上界 5000μs、错误率 0.001、当前队列深度 65536；
  行情/分发丢弃上限 100/窗口，订单与成交默认零丢弃。均为工具默认值，**不是已验证的生产 SLA**。
- 探针支持 `--max-avg-us`、`--max-p99-us`、`--max-drops`、`--max-queue`；
  还会检查整个运行期间的峰值队列，避免排空后掩盖超限。订单/成交零丢弃约束不被 `--max-drops` 放宽。
- 无延迟样本时状态为 `unknown`，不能按健康通过；已有丢弃/积压超限不会被空样本掩盖。
  此健康报告不是 CTP 登录/连接就绪探针，也不提供交易数据的新鲜度保证。
- 指标实例为进程级，使用互斥锁保证 CTP 回调线程与 reactor 的读写安全；延迟与计数存储有界，
  不在指标锁内执行 I/O。采集本身有开销，仍需在目标 Linux reactor 环境度量，不能据此宣称低延迟达标。
- 当前队列深度是该组件最近观测的队列，不是所有 shard 队列的总和；订单/成交共用一个网关队列，
  二者深度不能相加。多分片部署的分片级指标和聚合策略仍需在真实运行接线时收口。

### 8.4 业务埋点与尚未验收的范围

现有 CTP 网关已记录回调入队失败、回调到 reactor 出队的延迟、队列深度、
本地报单调用耗时/结果及 drain 耗时；event bus 已记录入队积压、丢弃、
队列等待耗时和策略回调成功/失败。报单耗时不是柜台确认或成交延迟，
行情出队成功也不代表后续策略处理完成；缺少成交处理器会计入错误。
上述埋点不会替代前序阶段尚未接通的订阅、行情业务分发和真实交易入口。

第六阶段完整验收仍需在前序链路可运行后提供：

1. 至少两套目标环境的真实行情/订单混合负载结果与阈值确认。
2. 真实运行链路的 8h/24h 长稳、断线重连、重新订阅、资源回收与异常积压恢复记录。
3. 相同输入下新旧系统的订单、成交、持仓、资金及夜盘/跨日语义对照，无未解释的严重偏差。
4. 经审批的灰度范围、停止/回退条件、恢复旧系统步骤与至少一次演练证据。

这些项目不是合成探针的通过项。当前工具不自动切流、杀进程、回退交易服务，
不能仅凭合成 JSONL 或 `--help` 成功将整个第六阶段标为完成。

## 9. B5–B8 内部组件与离线验收工具（待统一验证）

### 9.1 后续最小验证清单

以下命令供 VBox 恢复、用户允许统一验证之后使用，**本轮没有执行**。
现有 B1–B4 用例仍须一并复核，不能用这三个新增目标代替前置回归。

```bash
ninja -C build/debug test_unit_xtrader_market_pipeline test_unit_xtrader_recovery_coordinator
ctest --test-dir build/debug -R '^Seastar.unit.xtrader_(market_pipeline|recovery_coordinator|acceptance)$' --output-on-failure
python3 -B tests/unit/xtrader_acceptance_test.py
```

最后一条是标准库工具用例的独立运行方式，与 CTest 中 `xtrader_acceptance` 二选一，
不必重复执行。CMake 找不到 Python3 时会明确提示未注册该用例。
新代码也加入 demo 的编译列表，但没有安装正式运行链路或启用任何交易入口。

`paper_market_session` 的调用顺序为构造时安装异步 Tick/最终 bar sink →
等待 `on_tick` → 在返回结果后调用本地下单/撤单 → 等待后续 `on_tick` 的成交结果 →
等待 `stop()` 后销毁。Tick sink 还须保存该 Tick 携带的 `completed_bar`；
最终 bar sink 只接收仍未完成的尾部 bar。禁止 sink 内调用同步文件或 hiredis I/O，
禁止 sink 回调自身等待 `stop()`（会等待自身导致死锁）。
sink 的超时与强制取消尚未实现；未完成的 sink 会阻止 `stop()` 完成，不能在此之前
销毁 session。处理延迟门禁仅在 sink 返回后检查，不能代替 I/O 超时。
当前没有内置行情连接、策略回调分发、落盘/Redis 传输或重启恢复能力。

JSON v1 保存交易所/合约、真实交易日标签、显式时间域、源序号、时段边界、最新价、
最优盘口及盘口量、累计量、可缺失的增量量、持仓量和缺口标记。分钟 CSV v1 表头：

```text
schema_version,exchange,instrument,trading_day,time_domain,begin_ms,end_ms,open,high,low,close,observed_volume,open_interest,is_complete
```

这里的 `observed_volume` 不是保证完整的成交量，时间戳不是自动转换的 UTC；
不兼容旧历史 CSV 的 `date,open,high,low,close,volume,open_interest`。
消费者适配、隔离目录、Redis 保留长度和写入失败处置仍需明确实现后接线。

### 9.2 证据清单

工具只读本地文件，不执行测试、连接服务或开启交易。可在安装 Python3 的机器运行：

```bash
python3 -B tools/xtrader_acceptance.py template > manifest.json
python3 -B tools/xtrader_acceptance.py gate manifest.json
```

模板默认阻塞。`wbs` 必须包含开发计划的全部 27 项，`modes` 包含
`replay`、`paper`、`ctp_sim`、`live_disabled`；其中 `live_disabled` 表示实盘相关的
**禁用/隔离/离线桩验证**，不表示允许真实委托。每项填入真实 `status`、
`evidence` 引用列表及 `review` 审核记录。审批记录只能据实填写，模板不是批准。

`artifacts` 将引用 ID 映射为 `{"path":"相对文件路径","sha256":"64位小写摘要"}`。
文件必须非空、位于 manifest 所在目录内，不允许绝对路径或符号链接逃逸；
不要将凭据、原始账户身份或秘密配置放入证据包。
`implementation_revision` 应记录提交及未提交变更指纹，不能只填旧基线提交。

`environments` 至少两项不同 `id`，分别填写真实业务链路、8h/24h 时长（秒）、
丢失订单/成交数、容量/恢复是否通过、资源趋势是否审核、已批准阈值证据引用和审核记录。
`comparison`、`single_writer_rehearsal`、`rollback_rehearsal` 必须分别有证据；
`open_blockers` 非空时永远阻塞。清单不测量指标、不验证审批人的身份，也不能仅凭
两个不同环境标签证明真的运行过两套环境；报告内容仍须人工复核。

### 9.3 新旧逐笔 JSONL 对照契约

```bash
python3 -B tools/xtrader_acceptance.py compare legacy.jsonl seastar.jsonl
```

两份导出必须使用相同输入、参数和归一化规则。工具不直接解析旧系统私有日志，
**新旧导出适配器仍待实现**。每行 JSON 必须且只能包含：
`schema_version`（整数 1）、`scope`、`sequence`（从 1 连续递增）、
`event_time_ns`（公共时间域内单调非负整数观测时间）、`kind`、`key`、`values`。
`key` 是导出适配器建立的稳定业务关联标识，不可用互不对应的原生订单号直接比较。
顺序、时间和所有字段都参与精确比较，工具不会自动重排或忽略差异。

`scope` 必须含非空字符串 `dataset_id`、`parameter_set_id`、`trading_day`（YYYYMMDD）、
`account_alias`、`strategy_id`、`money_unit`、`price_unit`、`time_domain`。
除交易日允许单调推进外，其余范围在同一导出内固定；多账户/策略须分别导出。
价格以相同合约定义的整数 tick 表示，金额以 `money_unit` 指定的整数单位表示，
真实合约参数及时间域换算须随证据归档。

| kind | values 必需字段 |
|---|---|
| order | exchange、instrument、side、offset、hedge、type、limit_price_ticks、volume、filled、status |
| trade | exchange、instrument、side、offset、hedge、order_key、price_ticks、volume、commission_units、margin_change_units、close_profit_units、is_approximate、fill_source |
| position | exchange、instrument、hedge、long_today、long_yesterday、short_today、short_yesterday、frozen_long_today、frozen_long_yesterday、frozen_short_today、frozen_short_yesterday |
| account | balance、available、margin、frozen_margin、frozen_commission、commission、close_profit |

方向使用 `buy/sell`，开平使用 `open/close/close_today/close_yesterday`，
套保标识使用 `speculation/hedge/arbitrage`；委托类型为 `limit/market/fak/fok`，
市价单 `limit_price_ticks` 可为 0。成交来源为 `ctp_report/simulated_best_quote/simulated_bar_open`，
模拟成交的 `is_approximate` 必须为 `true`。数值字段均为整数，近似标记为布尔，
其余字段为非空字符串。状态使用领域类型的英文枚举值。
相同交易日/交易所/成交 `key` 重复出现会拒绝；订单状态更新可以重复使用同一业务 `key`。
四类记录均须出现，空文件或缺类不算完整替换对照。

两个检查子命令的退出码：`0` 表示证据库存完整或导出完全一致，`1` 表示阻塞/差异，
`2` 表示输入或文件错误。输出始终包含 `live_authorized=false`。只保留前 20 个差异行号，
但计数覆盖全部差异，不回显账户金额。库存完整、对照相等都不是实盘准入或人工签核。
