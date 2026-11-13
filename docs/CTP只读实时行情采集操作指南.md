# CTP 只读实时行情采集操作指南

日期：2026-11-13。范围：菜单入口 → AES-256-GCM 配置 → CTP MdApi 登录/订阅 → 行情显示 → Tick CSV。

**交付状态：代码与离线用例已加入，尚待 LD 编译、离线用例执行及真实前置验证。
不能把本指南当成已取得真实行情的验收证明。**

## 1. 能力和隔离边界

- CLI 主菜单 `9) Read-only CTP market capture` 启动独立的 `xtrader_market_capture` 进程。
- 采集器只链接仓库 CTP 6.7.2 的 `libthostmduserapi_se.so`，不链接交易库，不实例化交易网关、
  交易策略、资金账户或下单接口。旧交易网关及 replay/paper/ctp_sim/live 的禁用保护不变。
- 使用 TCP 单播行情连接；不照搬旧项目的 UDP/组播初始化参数。
- 配置明确指定交易所和具体合约，不自动选择主力合约，不把 `rb9999`、`RB9999.XSGE`
  当成 CTP 可订阅合约。螺纹钢、白银使用实际有效的 `rb...`、`ag...` 合约，交易所填 `SHFE`。
- 按配置逐一确认订阅应答；只有全部确认后才显示 `subscribed`。
  等待第一条 Tick、未开市或没有行情更新，不等于已有实时价格。
- SDK 回调仅复制到有界队列；工作进程处理登录、订阅、CSV 和显示。
  不在 Seastar reactor 或 SDK 回调里执行磁盘写入。

参考旧项目的 `src/trade-core/market/ctp_market.cpp` 的连接、登录和订阅顺序；
未复制其首 Tick 丢弃行为、旧目录、XOR 配置或交易启动流程。

## 2. 编译和离线检查

在 LD 的完整源码目录执行，继续使用原有脚本：

```bash
bash build_xtrader_demo_release.sh
# Debug 可改用：bash build_xtrader.sh
```

新增依赖是 OpenSSL Crypto 开发包；CTP 头文件和行情 `.so` 已在 `api/ctp/v6.7.2/linux64/`。
如果 CMake 报找不到 OpenSSL，应由管理员按 LD 发行版准备开发包
（例如 Debian/Ubuntu 的 `libssl-dev`，或 RHEL 系的 `openssl-devel`），再重试；
脚本不会自动安装，也不会启动采集或连接账号。

Release 新增产物：

```text
build/release/apps/xtrader/xtrader_market_capture
build/release/apps/xtrader/xtrader_market_config
```

独立离线检查不链接真实 CTP 库，不连接服务器，不使用生产 `/data`：

```bash
bash scripts/test_xtrader_market_capture.sh
```

其覆盖：菜单明确确认、加密配置往返、错误密钥/篡改/明文/文件权限拒绝、合约订阅、
首 Tick、多合约、跨日期、停止前队列排空、追加/单写者锁、断线/登录/订阅失败、
队列溢出及输出路径边界。它使用假 MdApi，不能验证真实账号、网络、前置兼容性或行情权限。

## 3. 准备目录和账号信息

以将来运行 CLI/采集器的同一个 Linux 用户操作；不要用 root 生成密钥后改用普通用户读取。
配置和密钥要求当前用户拥有、没有符号链接/硬链接、文件权限不向组或其他用户开放（建议 `0600`）。

```bash
umask 077
install -d -m 700 "$HOME/.config/xtrader"
install -d -m 700 "$HOME/.local/share/xtrader-keys"
```

`/data` 必须存在并可访问，`/data/x_trader_data` 必须能由采集账号写入。
若来源目录尚不存在，可让管理员为该账号创建；**不要递归更改已有采集数据的所有权或权限**。
程序不会把无权写入的数据自动保存到当前目录。

向期货公司/行情服务商确认：

| 信息 | 配置字段/输入 |
|---|---|
| CTP 行情前置 | `market_front`，格式 `tcp://主机名或IPv4:端口`，不接受交易前置替代 |
| 经纪公司代码 | `broker_id` / BrokerID |
| 行情登录用户 | `user_id` / UserID |
| 行情登录密码 | `password`，创建时隐藏输入 |
| 合约清单 | `EXCHANGE:instrument`，多个以英文逗号分隔，不加空格 |

例如形式为 `SHFE:rb实际交割年月,SHFE:ag实际交割年月`；
这段是格式说明，不能原样输入。请从当前柜台支持的合约中选择，旧项目的过期合约不能直接照搬。
当前支持显式映射 `SHFE/DCE/CZCE/CFFEX/INE/GFEX`，合约大小写须与柜台一致，最多 64 个。

CTP 通常通过期货公司/行情服务商的前置接入，并非直接使用交易所账号登录。
此采集器只调用 MdApi 的 `ReqUserLogin`，不调用 TraderApi 的 `ReqAuthenticate`；
不需要在此配置交易前置、AppID 或 AuthCode。若服务商另外要求专用接入认证、
穿透式采集组件或不同 SDK，必须先确认要求；本实现不宣称满足未验证的柜台合规认证。

## 4. 生成新密钥与加密配置

```bash
BIN=./build/release/apps/xtrader
"$BIN/xtrader_market_config" --new-key "$HOME/.local/share/xtrader-keys/market.key"
"$BIN/xtrader_market_config" --create \
  "$HOME/.config/xtrader/market.enc" \
  "$HOME/.local/share/xtrader-keys/market.key"
```

第二条命令依次提示行情前置、BrokerID、UserID、隐藏密码及合约列表。
不需要创建明文 INI，也不应把密码放进命令行、聊天或日志。密码输入必须使用终端。
工具不覆盖已存在的文件；更换配置时使用新文件名，确认后再切换菜单路径。

新格式是 `XTMDC001` 版本标识、随机 12 字节 nonce、16 字节认证 tag 和 AES-256-GCM 密文；
版本标识作为附加认证数据。密钥是 32 字节随机二进制文件，不是普通字符串口令。
错误密钥、篡改、明文 INI 和旧 XOR 密文都会拒绝，不会降级为明文读取。

密钥与密文应分别保护；同时获得二者的人可以解密。磁盘加密不保护已被控制的运行账号，
也不改变 CTP TCP 通道本身的传输安全属性。妥善备份密钥，丢失后只能重新生成配置。

## 5. 从菜单启动

```bash
export XTRADER_MARKET_CONFIG="$HOME/.config/xtrader/market.enc"
export XTRADER_MARKET_KEY_FILE="$HOME/.local/share/xtrader-keys/market.key"
./build/release/apps/xtrader/xtrader_cli --smp 1 --memory 512M --overprovisioned
```

1. 主菜单选择 **9**。
2. 核对加密配置与密钥文件路径；也可用子菜单 1/2 修改路径。
3. 子菜单选择 **3**，输入 **START** 才真正连接前置。
4. 观察 `subscriptions=已确认数/配置数`，全部确认后显示 `subscribed`。
5. 收到行情后显示每个合约的时间、最新价、买卖一档、累计成交量和持仓量，
   并检查 `ticks_written` 增长及对应 CSV。
6. 按 **Enter** 停止：释放 SDK、排空已接受的 Tick、同步文件，返回菜单。
   SIGINT/SIGTERM/SIGHUP 同样请求停止；启动它的父进程退出也请求停止，避免菜单退出后留下孤立采集器。
   从 CLI 按 Ctrl+C 还可能触发 CLI 自身的退出，
   因此优先使用 Enter。强杀/断电不保证队列和缓存中的数据落盘。

不设置环境变量时，菜单默认使用 `/etc/xtrader/market.enc` 和 `/etc/xtrader/market.key`。
这些与旧菜单展示的 `/etc/xtrader/<环境>/xtrader.ini` 无关。
Data Brief 的来源选择不改变本采集器的真实数据输出来源。

也可直接运行同一个采集器（仍是只读，不是绕开认证）：

```bash
./build/release/apps/xtrader/xtrader_market_capture \
  --config "$HOME/.config/xtrader/market.enc" \
  --key-file "$HOME/.local/share/xtrader-keys/market.key"
```

## 6. Tick 文件和日期语义

所有行情文件通过 `market_data_paths` 构造并校验：

```text
/data/x_trader_data/futures/<exchange>/<instrument>/tick/<ActionDay>.csv
```

`ActionDay` 是 SDK 提供的自然日期，`TradingDay` 是交易日，两者独立保留；
夜盘可能不同。文件按 `ActionDay` 切换，不按系统今天或交易日猜测。
SDK 若未给出有效日期/时间，会报错停止，不自行补值。
MD Tick 的 ExchangeID 可能为空，此时使用配置中明确提供的交易所映射；
若返回非空但与配置冲突，则拒绝记录。

CSV 第一行包含完整列名：版本、随机 session_id、进程内接收序号、回调收到时的 Unix 毫秒、
交易所、合约、ActionDay、TradingDay、更新时间/毫秒、最新价、累计成交量、成交额、
持仓量、开高低/昨收/昨结/昨持仓/涨跌停及买卖五档价格与数量。
CTP 的缺失价格哨兵 `DBL_MAX`、非有限浮点数写为空单元格，不伪造为 0 或有效报价。
这里保留原始累计量，不伪造首 Tick 增量。

重启会生成新 session_id 并追加当日文件，序号不是交易所序号，不能作为“行情无遗漏”的证明。
文件锁防止多个新采集器同时追加同一文件；旧采集器若不遵守锁协议，仍应由运维避免共写。
已有文件 schema 不符、结尾有不完整行、路径是链接或发生写盘失败时停止；
不会覆盖、修复或丢弃旧文件。请先保留现场、核查，再人工处理。

普通运行中约每秒进行 fsync，换日期与正常停止也同步；不是逐 Tick 断电零丢失承诺。
当前输出是独立的原始 Tick CSV，不生成旧历史 bar 读取器的 manifest，
不能据此认为 Data Brief、导入器或正式回测已支持读取这些 Tick。

## 7. 状态目录与故障处理

SDK flow 和采集器锁不是行情数据。状态目录按以下优先级选择：

1. `XTRADER_MARKET_STATE_DIR`
2. `$XDG_STATE_HOME/xtrader/market`
3. `$HOME/.local/state/xtrader/market`

要求绝对路径、当前用户所有且权限 `0700`，不允许符号链接。
同一状态目录只能运行一个采集器；不要与旧程序共用 SDK flow 目录。

| 现象 | 处理 |
|---|---|
| 找不到采集器 | 两个新程序应与 CLI 位于同一构建目录，重新运行完整构建脚本 |
| 加载 `.so` 失败 | 核查完整 SDK 文件、Linux 架构和 LD 上的动态库依赖；尚未进入账号登录 |
| 配置认证失败 | 核对配对密钥与密文，不改用旧 XOR/明文，不提交实际文件排查 |
| 权限/链接检查失败 | 使用正确运行账号，核查具体文件和目录，不递归放开权限 |
| 登录/订阅失败 | 程序只报告错误码，向服务商核对行情前置、账号权限、合约及开市时间 |
| 30 秒仍未全部订阅 | 连接、登录、订阅合计超时，检查前置/网络/柜台并显式重启 |
| 已订阅但没有 Tick | 可能未开市、无更新或无权限，不能当作采集成功 |
| 断线、SDK 错误或回调队列溢出 | 明确停止并返回非零；本版不自动重连、不补齐断线行情，检查后人工重启 |
| 输出损坏/磁盘满 | 保留已有 CSV 和错误信息；不要以删除生产数据作为默认处理方式 |

应用不打印账号、密码、密钥或原始柜台错误文本；但 SDK 自身的日志/flow 由厂商实现，
也应按私有目录保护。真实验证应先取得明确授权，确认来源服务、合约和交易时段；
本次代码交付未使用任何实际账号或连接前置。
