# Phase 3: CTP 行情/交易桥接任务清单

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
- [ ] 3.6.1 cmake 配置通过
- [ ] 3.6.2 ninja 编译通过
- [ ] 3.6.3 修复编译错误（如有）
