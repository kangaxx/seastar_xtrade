# Seastar xtrader 调试说明

本文档用于在 Ubuntu 虚拟机中对 Seastar 的 xtrader 目标进行编译、运行与调试。

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

在 Linux 项目根目录运行：

```bash
bash ./build_xtrader.sh
bash ./build_xtrader_demo_release.sh
```

两个脚本分别配置 Debug / Release，均编译 CLI 和 demo，并运行各自的 `--help` 检查。
Release 产物位于 `build/release/apps/xtrader/xtrader_cli` 和
`build/release/apps/xtrader/xtrader_demo`，不会覆盖 `build/debug` 中的产物。

默认并行数为 `JOBS=2`，低内存机器可使用：

```bash
JOBS=1 bash ./build_xtrader_demo_release.sh
```

两者均支持 `STALL_INTERVAL_SECONDS`（默认 60 秒）：阶段无输出时提示仍在运行，不会自动终止编译。
Release 完整日志保存在 `build/logs/build_xtrader_demo_release_*.log`。
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
