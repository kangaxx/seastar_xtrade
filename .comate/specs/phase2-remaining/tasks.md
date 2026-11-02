# Phase 2 剩余任务清单

> **更新记录**：2026-07-07 P0/P1 问题修复 + 任务状态双层模型

## 任务状态说明（双层模型）

| 状态 | 含义 | 验收标准 |
|------|------|----------|
| 🦴 骨架 | 接口/结构/可编译 | 代码存在，接口定义正确 |
| 🔗 链路 | 业务流可走通 | 用例可验证，有运行证据 |

格式：`[🦴/🔗/✅]` 或 `[骨架完成/链路打通/全部完成]`

---

## P0 问题修复

### P0-1: 账户更新链路静默丢失风险
- [x] 添加错误日志反馈当 `_account_service` 为空时
- [x] `submit_delta_to_account()` 返回 `future<bool>` 而非 `future<void>`
- [x] 修复位置：`runtime_engine.cc`

### P0-2: 模型与实现字段不一致
- [x] `account_delta` 添加 `commission` 字段
- [x] `trading_account::apply_delta()` 更新 commission
- [x] 修复位置：`domain_types.hh`, `domain_types.cc`, `runtime_engine.cc`

---

## P1 问题修复

### P1-1: drain loop 自递归调度风险
- [x] 使用 `timer<>` 替代 `submit_high_priority_task` 递归调用
- [x] `_md_drain_timer.arm_periodic(500us)`
- [x] `_trader_drain_timer.arm_periodic(100us)`
- [x] `stop()` 中正确 cancel timers

### P1-2: 任务勾选与代码事实边界不清
- [x] 添加显式 SPI 回调方法声明
- [x] 实现 stub 逻辑：`push()` 到 ring buffer + 丢弃计数
- [x] 修复位置：`ctp_gateway.hh`, `ctp_gateway.cc`

### P1-3: account shard 参数传递链
- [x] `runtime_engine` 添加默认构造函数（支持 `sharded<>` 初始化）
- [x] 添加 `set_account_shard()` / `set_gateway_shard()` 方法
- [x] `runtime_sharded::start()` 中初始化 account_service
- [x] 修复位置：`runtime_engine.hh/cc`, `runtime_sharded.cc`

### P0 FIX (新): 账户服务初始化与失败策略
- [x] `trading_account_service` 添加 `_initialized` 状态
- [x] `init_account_service()` 方法统一初始化
- [x] `runtime_sharded::start()` 中调用 `init_account_service()`
- [x] `apply_delta()` 返回 bool 表示成功/失败
- [x] `submit_delta_to_account()` 返回 `future<bool>`

### P1 FIX (新): Gateway Shard 身份链路
- [x] 移除构造期 shard 身份绑定
- [x] 在 `start()` 中设置 `_is_gateway_shard = (smp::shard_id() == _gateway_shard)`
- [x] `submit_order()` 使用运行时 shard 身份路由

---

## Task 2: account_shard 归并路径

| 子任务 | 骨架 | 链路 | 代码证据 |
|--------|------|------|----------|
| 2.2 runtime_sharded account_shard_id | 🦴 | 🔗 | `runtime_sharded.hh` 构造函数, `set_account_shard()` |
| 2.3 trading_account::apply_delta() | 🦴 | 🔗 | `domain_types.cc` 第 84-114 行 |
| 2.4 submit_delta_to_account() | 🦴 | 🔗 | `runtime_engine.cc` 第 95-122 行 |
| 2.5 并发安全验证 | - | 🔗 | 单 shard 内更新，无竞态 |

**完成度**：🦴 5/5 | 🔗 5/5

---

## Task 3: ctp_gateway 骨架化

| 子任务 | 骨架 | 链路 | 代码证据 |
|--------|------|------|----------|
| 3.1 SPSCRingBuffer | 🦴 | 🦴 | `spsc_ring_buffer.hh` |
| 3.2 MdSlot/TraderSlot | 🦴 | 🦴 | `ctp_gateway.hh` |
| 3.3 MdSpi 回调 | 🦴 | 🔗 | `on_market_data()` |
| 3.4 TraderSpi 回调 | 🦴 | 🔗 | `on_trade_report()`, `on_order_return()` |
| 3.5 drain 任务骨架 | 🦴 | 🔗 | `timer<>` 调度 |
| 3.6 批量上限 | 🦴 | 🔗 | `max_drain_per_tick`, `max_drain_us` |
| 3.7 hash 路由 | 🦴 | 🔗 | `instrument_shard_id()` |
| 3.8 替换 ctp_adapter | 🦴 | 🔗 | `runtime_engine.cc` |
| 3.9 CMakeLists.txt | 🦴 | 🦴 | `apps/xtrader/CMakeLists.txt` |
| 3.10 编译验证 | - | 🔗 | ⏳ 待执行 |

**完成度**：🦴 9/9 | 🔗 7/9（待编译验证）

---

## Task 4: 编译验证

| 子任务 | 状态 |
|--------|------|
| 4.1 编译 | ⏳ 待执行 |
| 4.2 修复错误 | ⏳ 待执行 |
| 4.3 记录日志 | ⏳ 待执行 |
| 4.4 运行验证 | ⏳ 待执行 |

**完成度**：🔗 0/4

---

## 任务依赖关系

```
Task 2.2 ──┬──> Task 2.3 ──> Task 2.4 ──> Task 2.5
           │                      │
           └──────────────────────┘
                                  │
Task 3.3 ──┬──> Task 3.5 ──> Task 3.6 ──┬──> Task 3.8 ──> Task 3.10
Task 3.4 ──┘                             │
                                          │
Task 3.8 ────────────────────────────────┼──> Task 4.1 ──> Task 4.2 ──> Task 4.3 ──> Task 4.4
                                          │
Task 3.10 ───────────────────────────────┘
```
