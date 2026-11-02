# Phase 3: CTP 行情/交易桥接实现

## 概述

本文档定义 Phase 3 的开发规格，实现 CTP API 与 Seastar Reactor 的桥接。

## 参考文档

- `STEP-03-ctp-reactor-bridge-solution-1.md` - 架构设计文档

## Phase 3 任务清单

### 3.1 CTP SPI 适配器实现

**目标**：实现 `MdSpi` 和 `TraderSpi` 的 Seastar 封装。

**新增文件**：
- `include/seastar/xtrader/ctp_spi_adapter.hh`
- `src/xtrader/ctp_spi_adapter.cc`

**关键接口**：
```cpp
class MdSpiAdapter : public CThostFtdcMdSpi {
    explicit MdSpiAdapter(ctp_gateway* gateway);
    // CTP SPI callbacks -> push to md_ring_buffer
};

class TraderSpiAdapter : public CThostFtdcTraderSpi {
    explicit TraderSpiAdapter(ctp_gateway* gateway);
    // CTP SPI callbacks -> push to trader_ring_buffer
};
```

### 3.2 domain 类型与 CTP 结构映射

**目标**：实现 `convert_from_ctp()` 函数族。

**修改文件**：
- `include/seastar/xtrader/domain_types.hh`
- `src/xtrader/domain_types.cc`

**映射函数**：
```cpp
domain::market_data convert_from_ctp(const CThostFtdcDepthMarketDataField*);
domain::trade_report convert_from_ctp(const CThostFtdcTradeField*);
domain::order convert_from_ctp(const CThostFtdcOrderField*);
```

### 3.3 Pending orders 管理

**目标**：维护挂单状态映射，支持撤单链路。

**新增结构**：
```cpp
struct pending_order {
    domain::order_request request;
    std::string order_ref;
    std::chrono::steady_clock::time_point submit_time;
    domain::order_status status;
};

class pending_order_manager {
    std::unordered_map<std::string, pending_order> _orders;
    future<domain::order_status> submit(const domain::order_request& req);
    future<> cancel(const std::string& order_sys_id);
};
```

### 3.4 账户资金初始化

**目标**：从 CTP 查询账户信息并初始化 `trading_account`。

**接口**：
```cpp
future<> sync_account_from_ctp();
```

### 3.5 编译验证

**目标**：确保所有新代码可编译通过。

---

## CTP 库路径

```
api/ctp/v6.7.2/linux64/
├── libthostmduserapi_se.so
├── libthosttraderapi_se.so
├── ThostFtdcMdApi.h
├── ThostFtdcTraderApi.h
└── ThostFtdcUserApiStruct.h
```

---

## 验收标准

1. CTP SPI 回调可正确 push 到对应 ring buffer
2. domain 类型与 CTP 结构转换正确
3. pending_orders 可追踪订单状态
4. 账户初始化从 CTP 查询成功
5. 编译通过
