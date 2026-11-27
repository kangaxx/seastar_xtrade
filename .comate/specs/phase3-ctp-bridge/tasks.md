# Phase 3: CTP 行情/交易桥接任务清单

> 2026-11-09 修订说明：以下勾选保留为历史编码记录，不代表完整功能验收。
> 实际订阅/行情下游、启动查询同步、订单回报路由、线程所有权及重连对账仍需收口，
> 以[开发计划修订版](../../../docs/xtrader_development_plan.md)的 R3-01 至 R3-05 为当前执行清单。

## 3.1 CTP SPI 适配器实现
- [x] 3.1.1 创建 `ctp_spi_adapter.hh`
- [x] 3.1.2 创建 `MdSpiAdapter` 类（继承 CThostFtdcMdSpi）
- [x] 3.1.3 创建 `TraderSpiAdapter` 类（继承 CThostFtdcTraderSpi）
- [x] 3.1.4 实现 OnRtnDepthMarketData -> md_ring_buffer_.push()
- [x] 3.1.5 实现 OnRtnTrade -> trader_ring_buffer_.push()
- [x] 3.1.6 实现 OnRtnOrder -> trader_ring_buffer_.push()

## 3.2 domain 类型与 CTP 结构映射
- [x] 3.2.1 实现 `convert_from_ctp(market_data)` 内联版本
- [x] 3.2.2 实现 `convert_from_ctp(trade_report)` 内联版本
- [x] 3.2.3 实现 `convert_from_ctp(order)` 内联版本
- [x] 3.2.4 实现 offset_flag 转换函数

## 3.3 Pending orders 管理
- [x] 3.3.1 创建 `pending_order` 结构
- [x] 3.3.2 创建 `pending_order_manager` 类
- [x] 3.3.3 实现 `submit_order()` 并记录 pending
- [x] 3.3.4 实现 `cancel_order()` 并维护状态
- [x] 3.3.5 实现超时检测和状态更新

## 3.4 账户资金初始化
- [x] 3.4.1 实现 `sync_account_from_ctp()`
- [x] 3.4.2 处理 OnRspQryTradingAccount 回调
- [x] 3.4.3 初始化 trading_account_service

## 3.5 CTP API 生命周期管理
- [x] 3.5.1 实现 MdApi/TraderApi 实例化
- [x] 3.5.2 实现连接/登录流程
- [x] 3.5.3 实现断开/登出流程
- [x] 3.5.4 添加配置验证

## 3.6 编译验证
- [x] 3.6.1 cmake 配置通过
- [x] 3.6.2 ninja 编译通过
- [x] 3.6.3 修复编译错误（如有）

> 2026-11-27 编译验证记录（VBox `xtrader-vbox`，Ubuntu 22.04，4 vCPU / 5 GB 内存）
>
> - 代码基线：`daemon-mode-m0m1` @ `a6e017d`（由 Windows 本地库整树同步至
>   `/home/vboxuser/workspace/seastar_xtrade`，`git status` 干净，抽样 md5 与本地一致）。
> - 配置：`BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local`，`--mode=release`，沿用既有
>   `build/release`（含 238 MB `libseastar.a` 与 106 个 seastar 目标文件，无需全量重编）。
>   配置输出 `Build files have been written to: .../build/release`，退出码 0。
> - `ninja -C build/release -j2 app_xtrader_cli`：**22/22 通过**，0 error；
>   产物 `xtrader_cli` 107 MB、`xtrader_market_capture` 7.4 MB、`xtrader_market_config` 3.4 MB。
> - `ninja -C build/release -j2 app_xtrader_demo`：**38/38 通过**，0 error；
>   产物 `xtrader_demo` 142 MB（`ELF 64-bit LSB pie executable`）。
> - 运行验证：三个可执行文件 `--help` 均返回 0，版本行输出
>   `X-Trader Seastar Trading Engine v0.1.3.unknown.release` 且 `Git: a6e017d` 正确。
> - 构建日志：`/home/vboxuser/build_logs/vm_cli_build.log`、
>   `/home/vboxuser/build_logs/vm_demo_build.log`。
> - 已知无害告警：GnuTLS 3.7.3 低于要求的 3.7.4（OpenSSL 3.0.2 可用）；
>   `/var/log/xtrader` 无写权限（普通用户运行，非 root）；
>   `release-build-info/xtrader-build-info.txt` 未生成（版本号显示 `unknown`）。
> - 说明：本记录仅证明「可配置、可编译、可启动、`--help` 可用」，
>   **不构成功能验收**（未连接 CTP 柜台、未订阅行情、未发单）。
