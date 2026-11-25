# CTP 行情采集服务化部署手册 (systemd + timer)

分支: `daemon-mode-m0m1`。改动点:

| 文件 | 说明 |
|---|---|
| `apps/xtrader/market_capture_daemon.{hh,cc}` | 新增: `--daemon-tick` 无交互常驻入口、PID 文件单实例锁、心跳状态文件、可选进程内 stale 看门狗 |
| `apps/xtrader/market_capture_runtime.{hh,cc}` | 新增: 信号/状态目录共享逻辑(从交互入口抽出,行为不变; 重复 stop 信号强制退出) |
| `apps/xtrader/xtrader_market_capture.cc` | 最小改动: `--daemon-tick` 分发, 菜单逻辑原样保留 |
| `apps/xtrader/xtrader_log.hh` | 新增 `mirror_to_stderr()`: daemon 模式每行日志镜像到 stderr 供 journald 采集 |
| `apps/xtrader/CMakeLists.txt` | `app_xtrader_market_capture` 增加两个新源文件 |
| `deploy/ctp-md/*` | 交易日历、调度脚本、systemd 单元、安装脚本 |

## 1. 构建 (Linux, 项目现有构建树)

```bash
ninja -C build/release app_xtrader_market_capture   # 或项目既有的构建目标
# 验证两种模式都在:
build/release/apps/xtrader/xtrader_market_capture --help
build/release/apps/xtrader/xtrader_market_capture --daemon-tick --help
```

## 2. 安装二进制与凭据

```bash
sudo install -d -m 0755 /opt/xtrader/bin
sudo install -m 0755 build/release/apps/xtrader/xtrader_market_capture /opt/xtrader/bin/
sudo install -m 0640 -o xtrader -g xtrader market.enc /etc/xtrader/market.enc
sudo install -m 0640 -o xtrader -g xtrader market.key /etc/xtrader/market.key
# 数据目录权限(daemon 以 xtrader 用户运行):
sudo install -d -m 0750 -o xtrader -g xtrader /data/x_trader_data
```

## 3. 安装调度层

```bash
cd deploy/ctp-md && sudo ./install.sh
```

## 4. 手工冒烟测试(必做, 一次即可)

```bash
# 前台直跑, 观察日志(绕过 systemd):
sudo -u xtrader env TZ=Asia/Shanghai XTRADER_MARKET_STATE_DIR=/var/lib/xtrader/market \
    /opt/xtrader/bin/xtrader_market_capture --daemon-tick \
    --config /etc/xtrader/market.enc --key-file /etc/xtrader/market.key
# 另一终端: 应看到 ready=1、ticks 增长、60s 心跳
cat /var/lib/xtrader/market/daemon.status

# 单实例验证: 再起第二个实例应立即失败并报 pid:
#   "pid lock held by another instance (pid N)"  →  exit 10

# systemd 路径:
sudo systemctl start ctp-md.service
journalctl -u ctp-md -f
sudo systemctl stop ctp-md.service          # 验证 SIGTERM 优雅停(日志出现 "daemon stopped")
cat /var/lib/xtrader/market/daemon.status   # 停止后不存在(或为 stopping 快照)
```

## 5. 交易日历自测(可直接验证 2026 年公告的每个边界)

```bash
CAL=/usr/local/libexec/ctp-md/ctp_trading_calendar.py
$CAL is-trading-day 2026-02-13        # trading
$CAL is-trading-day 2026-02-16        # non-trading (春节)
$CAL is-trading-day 2026-09-20        # non-trading (周日, 调休上班日也不开市)
$CAL has-night-session 2026-04-03     # no (清明前夜, 官方公告一致)
$CAL has-night-session 2026-04-07     # has (节后恢复夜盘)
$CAL has-night-session 2026-02-06     # has (普通周五)
$CAL next-trading-day 2026-02-13      # 2026-02-24
$CAL check-coverage --warn-days 365   # 覆盖期提醒
```

## 6. 日常运维

```bash
systemctl list-timers 'ctp-md*'                       # 各 timer 下次触发
sudo /usr/local/libexec/ctp-md/ctp_session_ctl.sh status   # 服务状态+journal 尾部
journalctl -u ctp-md -S today -f                      # 主服务日志
journalctl -u 'ctp-md-ctl@*.service' --since today    # 调度动作日志
```

- 服务**不要** `systemctl enable`(开机常驻): 由 timer + health 按交易时段拉起; 只有想连周末也采集时才 enable。
- 断线自愈两层: CTP 断连事件使 daemon 以 exit 2 退出 → `Restart=on-failure` 秒级拉起; 卡死/静默断流由每分钟 health 兜底(限流: 每小时最多 5 次重启)。
- 手动干预: `systemctl stop ctp-md.service` 后, 若处于交易时段, 下一分钟 health 会自动拉起 —— 要彻底停用先 `systemctl disable --now ctp-md-health.timer`。

## 7. 每年 12 月的必做动作

交易所 12 月中旬发布下一年休市安排(证监会统一通知)。届时更新
`/etc/xtrader/trading_calendar.json`(节假日区间 + `coverage_until` 延展到次年 12-31)。
不更新则日历到期后调度**fail-closed 拒绝启动**(不会在错误日期采集),
`ctp-md-calendar-check.timer` 每周一早 08:10 提前 30 天开始在 journal 告警。
