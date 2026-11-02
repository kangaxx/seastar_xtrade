# Phase 2 剩余任务开发规格

## 概述

本文档定义 X-Trader Seastar 迁移计划（PLAN-20260608-000009）第二阶段剩余任务的开发规格。

## 当前状态

### 已完成
- Task 1: domain_types.hh 扩展（10/10 子任务）✅
- Task 2.1: account_delta 结构体 ✅
- Task 3.1: SPSCRingBuffer 模板类 ✅
- Task 3.2: MdSlot/TraderSlot 结构体 ✅
- Task 3.7: instrument_shard_id hash 路由函数 ✅
- Task 3.9: CMakeLists.txt 更新 ✅

### 剩余任务
- **Task 2.2-2.5**: account_shard 归并路径（4个子任务）
- **Task 3.3-3.6, 3.8**: ctp_gateway 骨架化（6个子任务）
- **Task 3.10, 4.1-4.4**: 编译验证（5个子任务）

---

## Task 2: account_shard 归并路径

### 2.2 在 runtime_sharded 中固定 account_shard_id

**目标**：引入 `account_shard_id_` 配置，支持多账户场景。

**修改文件**：
- `include/seastar/xtrader/runtime_sharded.hh`
- `src/xtrader/runtime_sharded.cc`

**实现要点**：
```cpp
// runtime_sharded.hh
class runtime_sharded {
public:
    explicit runtime_sharded(unsigned account_shard = default_account_shard);
private:
    unsigned _account_shard_id = 1;
    static constexpr unsigned default_account_shard = 1;
};
```

### 2.3 实现 trading_account::apply_delta()

**目标**：实现账户资金增量更新逻辑。

**修改文件**：
- `include/seastar/xtrader/domain_types.hh`
- `src/xtrader/domain_types.cc`

**实现要点**：
```cpp
struct account_delta {
    double balance_delta = 0;      // 资金变化
    double margin_delta = 0;       // 保证金变化
    double frozen_delta = 0;       // 冻结变化
    double commission_delta = 0;   // 手续费变化
};

struct trading_account {
    double pre_balance = 0;
    double balance = 0;
    double available = 0;
    double curr_margin = 0;
    double frozen_margin = 0;
    double commission = 0;

    void apply_delta(const account_delta& delta) noexcept;
};
```

### 2.4 实现 submit_delta_to_account()

**目标**：在 instrument shard 调用 account shard 的增量更新。

**实现要点**：
- 使用 Seastar 的 `submit_to()` 跨 shard 调用
- 保持 `account_delta` 不可变传递

### 2.5 验证并发安全

**目标**：确保多 shard 并发提交 delta 不产生竞态。

**验证方法**：
- 单测覆盖多 shard 并发场景
- 检查 `account_delta` 的线程安全传递

---

## Task 3: ctp_gateway 骨架化

### 3.3 MdSpi 回调中的 push() 骨架

**目标**：实现行情入队逻辑。

**实现要点**：
```cpp
// 在 MdSpi 回调中
void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    md_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.data = convert_from_ctp(pDepthMarketData);
    _md_ring.push(slot);  // 无锁入队
}
```

### 3.4 TraderSpi 回调中的 push() 骨架

**目标**：实现交易回报入队逻辑。

**实现要点**：
```cpp
// 在 TraderSpi 回调中
void OnRtnTrade(CThostFtdcTradeField* pTrade) {
    trader_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.type = event_type::trade;
    slot.trade_data = convert_from_ctp(pTrade);
    _trader_ring.push(slot);  // 无锁入队
}
```

### 3.5 Shard 0 高优先级 drain 任务骨架

**目标**：实现 Seastar Reactor 调度的 drain loop。

**实现要点**：
```cpp
void ctp_gateway::start_md_drain_loop() {
    // 使用 high_priority_task 调度
    _md_drain_task.set_callback([this] { drain_md_ring(); });
    // 提交到 reactor
}
```

### 3.6 drain 循环批量上限

**目标**：防止单次 drain 阻塞 Reactor。

**实现要点**：
```cpp
static constexpr size_t MAX_DRAIN_PER_TICK = 1024;
static constexpr size_t MAX_DRAIN_US = 500;  // 500us 上限

void ctp_gateway::drain_md_ring() {
    md_slot slot;
    size_t drained = 0;
    auto start = seastar::steady_clock_type::now();
    
    while (drained < MAX_DRAIN_PER_TICK) {
        auto elapsed = seastar::steady_clock_type::now() - start;
        if (elapsed.count() > MAX_DRAIN_US * 1000) {
            break;  // 超过时间预算
        }
        if (!_md_ring.pop(slot)) {
            break;  // 队列空
        }
        // 路由到目标 shard
        ++drained;
    }
}
```

### 3.8 替换 ctp_adapter.cc stub

**目标**：将 runtime_engine 的 ctp_adapter 替换为 ctp_gateway。

**修改文件**：
- `include/seastar/xtrader/runtime_engine.hh`
- `src/xtrader/runtime_engine.cc`

### 3.10 编译验证

**目标**：确保代码可编译通过。

---

## Task 4: 编译验证

### 4.1-4.4 编译与运行验证

**目标**：在 VM 中编译并运行 xtrader_demo。

**验证命令**：
```bash
ssh xtrader-vbox "cd /home/vboxuser/workspace/seastar && \
  rm -rf build/debug && \
  BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local \
  python3 configure.py --mode=debug --disable-dpdk && \
  ninja -C build/debug app_xtrader_demo -j2 && \
  ./build/debug/apps/xtrader/xtrader_demo"
```

---

## 验收标准

1. **编译通过**：`ninja -C build/debug app_xtrader_demo` 成功
2. **运行不崩溃**：`xtrader_demo` 启动并输出正确日志
3. **CTP gateway 骨架完整**：drain loop 可调度
4. **账户更新链路闭环**：account_delta 传递路径完整

---

## 关联文档

- `docs/plans/2026-06/attachments/PLAN-20260608-000009/04-runtime/phase2-tasks.md`
- `docs/plans/2026-06/attachments/PLAN-20260608-000009/03-runtime-bridge/STEP-03-ctp-reactor-bridge-solution-1.md`
