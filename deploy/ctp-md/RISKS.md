# CTP 行情服务化风险坑点清单

按"会出事"的优先级排列; 每条给出触发条件与规避手段。

## 1. 前置会话 (CTP Front Session)

- **同账号同前置双开互相踢**: 每个 (前置, BrokerID, UserID) 只允许一个行情会话。第二个实例登录成功会踢掉第一个, 被踢方收 `OnFrontDisconnected(0x1004)`。本项目用 PID 文件 flock(exit 10) + state 目录 `collector.lock` 双保险; 但**换配置路径/换 state 目录可绕过**, 所以严禁用不同参数手工起第二个实例, 部署后只走 systemd。
- **行情与交易是两个独立会话**: MdApi 与 TraderApi 互不踢人, 但同一前置地址分别登录同账号也可能触发柜台限流; 本项目只链 MdApi, 不受交易会话影响。
- **订阅请求限流**: `SubscribeMarketData` 一次最多 100 个合约、单账号订阅总数有上限(通常数百)。本项目批量订阅 10 个预设合约, 无风险; 未来扩容注意分批、留意 `OnRspSubMarketData` 里 `ErrorID!=0`(逐合约权限失败, 不代表全量失败)。
- **登录请求 ID**: MdApi 登录应答的 `request_id` 可能为 0 或 1, 现有代码已兼容; 不要把这个宽松逻辑搬到 TraderApi。

## 2. 长时间空闲断流

- **前置空闲回收**: 部分期货公司前置对无数据流/无心跳的行情连接有超时回收(数十分钟到数小时不等), 且非交易时段(午休、15:30 后、周末、节前)断开/维护频繁。表现: 无任何事件直接 `OnFrontDisconnected`。
- **本方案的断线语义是"退出即重启"**: 现有 session 收到 disconnected 即抛错 → daemon exit 2 → `Restart=on-failure` 5 秒拉起, 新会话重新走 登录→订阅, 天然规避 CTP 断线重连后必须重登录重订阅的复杂状态机。代价: 断线到重启间隙的 tick 丢失, 属可接受; 需要补数据的场景另行落库对账。
- **静默断流**(链路死了但 TCP 没断): 无 disconnect 事件。由每分钟 health 的 tick 停滞检测兜底, 且只在"本会话已收到过 tick + 处于 tick 窗口"时判定, 避免午休/夜盘无行情品种误杀。

## 3. 交易日历/调休坑

- **期货市场周末永远不开市**: 调休上班的周六/周日(如 2026-02-14、02-28、05-09、09-20、10-10)股市补班、期货休市。日历脚本对 weekday>=5 一律判非交易日, 与官方公告逐条核对过 2026 全年。
- **节前最后交易日无夜盘**: 规则 = "下一交易日与当日间隔 >3 天则该晚无夜盘"(周五→周一隔 3 天有夜盘; 隔 4 天及以上无)。已用 2026 年 7 个官方"不进行夜盘交易"公告验证: 2025-12-31、02-13、04-03、04-30、06-18、09-24、09-30 全部命中。
- **ActionDay 跨日**: 21:00 后的 tick 属于下一交易日; 周五夜盘 tick 的交易日是下周一。现有落盘按 `ActionDay` 分文件, 天然正确; 但**任何按本地日历日做统计/切文件的自研逻辑都要改用 ActionDay**。
- **日历年度维护**: 交易所每年 12 月中旬发布下一年安排。`coverage_until` 到期后脚本 **fail-closed**(拒绝启动而非错日启动), `ctp-md-calendar-check.timer` 每周一早提醒。漏更新的后果是停采而非错采——可接受, 但别把它拖过元旦假期。
- **临时调整**: 罕见极端情况下交易所临时休市/加班, 用 JSON 里 `extra_holidays` / `extra_trading_days` 覆盖, 不要改代码。

## 4. 权限类

- **行情权限是账号级开通**: 前置能连不代表能订阅; 某些品种/期权需要单独开通。订阅失败时 `OnRspSubMarketData` 返回逐合约错误, daemon 会把订阅失败当致命错误退出重启——如果某合约长期无权限, 会陷入 systemd 的重启退避循环(StartLimitBurst 5 次后停止), 健康检查也会参与重启直到限流。**先确认 10 个预设合约都有权限**, 生产环境无权限合约应从配置中移除。
- **程序化交易报备**: 各交易所要求程序化接入报备(含行情采集)。未报备可能被柜台风控限流或断连, 上线前找期货公司确认。
- **SimNow 与生产前置不同**: 测试环境与生产的前置地址、账号、流控策略都不同, 别把 SimNow 结论直接搬到生产。
- **运行用户权限**: daemon 以专用系统用户 `xtrader` 运行, 需要: `/data/x_trader_data` 写权限、`/var/lib/xtrader`(state/flow)、`/var/log/xtrader`、`/run/xtrader`(pid, tmpfs)。systemd 的 `RuntimeDirectory`/`StateDirectory`/`LogsDirectory` 自动建目录并归属正确, 但 `/data/x_trader_data` 必须手工授权(install 步骤里已给命令)。

## 5. so 库依赖

- **SE 版 so**: 项目链的是 `libthostmduserapi_se.so`(免注册/无授权文件版)。若换成非 SE 版(`libthostmduserapi.so`), 通常还需要 `.dat` 授权文件和对应 `so` 版本, 且部分柜台要求 `CreateFtdcMdApi` 传 flow 路径——不要混用版本。
- **glibc 兼容**: CTP so 是闭源二进制, 6.7.2 需要较新 glibc。升级系统/换发行版后先 `ldd libthostmduserapi_se.so` 看缺不缺库, 缺 `libssl`/`libcrypto` 时行情登录会静默失败或 `CreateFtdcMdApi` 返回空。
- **flow 目录**: CTP 内部写 flow 文件记录会话状态, 本项目把它放在 state 目录下的 `flow/`(0700 属主校验)。目录不可写时 API 可能直接崩或回调异常, 权限被改坏是第一排查点。
- **rpath**: CMake 已对可执行文件写 `BUILD_RPATH` 指向 CTP 目录; 若把二进制拷到 `/opt/xtrader/bin` 单独跑, 需 `LD_LIBRARY_PATH` 或把 so 装进系统库目录并 `ldconfig`。部署后 `ldd /opt/xtrader/bin/xtrader_market_capture` 应是全绿。
- **SDK 升级无 ABI 保证**: CTP 每个大版本(6.3/6.5/6.7...)结构体字段有增删, 升级必须整包换(so + 头文件)并重新编译, 且升级前要对照 `error.xml` 重新核对错误码语义。

## 6. 工程/部署特有坑

- **exit 10 不重启**: pid 锁冲突必须映射 `RestartPreventExitStatus=10`, 否则两个实例互相 restart 风暴。改 unit 文件时别删这行。
- **systemd 沙箱 vs 数据路径**: `ProtectSystem=strict` 下除 `ReadWritePaths=/data/x_trader_data` 外全部只读; 若未来把数据目录移到别处, unit 和 `market_data_paths` 默认值要同步改, 否则进程直接 EACCES 退出 2。
- **时区**: timer 用 `OnCalendar=... Asia/Shanghai` 显式时区, 服务进程 `Environment=TZ=Asia/Shanghai`。机器换时区不影响调度; 中国无夏令时, 不存在 DST 跳变问题。但**机器时间漂移**会同时搞乱 timer 触发和 tick 时间戳, 确保 chrony/NTP 常驻。
- **夜盘品种差异**: 上期所贵金属/原油/有色夜盘到 01:00~02:30。23:05 停会截断这些品种。若订阅品种里有它们: `NIGHT_PRESENT_WINDOW/NIGHT_TICK_WINDOW` 改成跨午夜区间(如 `20:30-02:35`), 启用 `ctp-md-stop-night-late.timer` 并停用 23:05 的那个。窗口解析器原生支持跨午夜。
- **journald 容量**: 心跳 1 条/分钟 + health 日志, 长期运行量大。建议 `/etc/systemd/journald.conf.d/ctp-md.conf` 设 `SystemMaxUse=1G`、`MaxRetentionSec=30day`; 文件日志 `/var/log/xtrader/` 建议配 logrotate(每天轮转、保留 14 份)。
- **优雅停靠两层兜底**: SIGTERM → 主动 `RegisterSpi(nullptr)+Release()+flush 落盘`; 若 Release 卡死, 第二个 SIGTERM 或 systemd `TimeoutStopSec=45` 会强杀。stop 过程中若丢弃未落盘 tick, 会话会报错并以 exit 2 结束——journal 里出现该错误说明有数据缺口, 要查。
- **SELinux/AppArmor**: 开启强制模式的分发版可能拦截 `/run/xtrader` 写入或 CTP so 的匿名映射。报 `AVC denied` 时按分发版加白, 不要直接关 SELinux。
