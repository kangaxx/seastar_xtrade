# CTP Demo 最小编译修复及后续风险

日期：2026-11-05  
来源：CTP Demo 编译问题分析及最小修复要求  
记录：GitHub Copilot

## 本次修改范围

- `include/seastar/xtrader/ctp_gateway.hh`：补充 SDK 登录请求结构 `CThostFtdcReqUserLoginField` 的前置声明。
- `include/seastar/xtrader/ctp_spi_adapter.hh`：移除两个 SPI 适配器析构函数上的 `override`，保留具体类型的 `std::unique_ptr` 所有权。
- `include/seastar/xtrader/domain_types.hh`：在 `order_request` 末尾增加 `order_type type = order_type::limit`，使现有未指定类型的请求继续采用限价语义，避免改变原有成员的聚合初始化顺序。

本次不修改回调线程模型、退出流程、订单校验、序列化或实盘接入逻辑。以下风险仍待处理；编译通过不表示风险已经关闭。

## 风险清单

| 编号 | 级别 | 涉及对象 | 风险及影响 | 后续处理与验收要求 |
|---|---|---|---|---|
| CTP-R01 | 中 | `MdSpiAdapter` 生命周期 | SDK 基类没有虚析构函数，不能通过 `CThostFtdcMdSpi*` 删除适配器。适配器持有网关裸指针，若 API 回调尚未停止就销毁适配器或网关，可能发生悬空访问。移除 `override` 只解决编译错误。 | 保持具体类型所有权；依据 SDK 生命周期契约确认停止、释放 API 与销毁适配器的顺序。覆盖正常停止、连接未完成即停止、断线及重复停止场景，确认销毁后不再收到回调。 |
| CTP-R02 | 中 | `TraderSpiAdapter` 生命周期及交易回报 | 同样不能通过 `CThostFtdcTraderSpi*` 删除适配器。停止过程中仍可能有委托、成交、撤单或资金查询回调；过早销毁可能导致悬空访问，未处理完的回报还可能造成订单、持仓或资金状态不一致。 | 确认回调停止与对象销毁顺序；明确停机时待处理回报、未完成请求和未完成委托的处理及重连对账策略。覆盖带未完成请求停止、成交与停止并发、断线重连场景。 |
| CTP-R03 | 中高 | `order_request::type` 及下单全链路 | 默认 `limit` 保持现有请求的限价语义，但不代表市价、FAK、FOK 已端到端支持。当前 `is_valid()` 仍要求 `price > 0`，可能拒绝不依赖正价格的市价请求；策略、回测或序列化若丢失类型，会回落为限价，造成实盘和回测语义不一致。新增字段还会改变结构布局，相关 C++ 使用方必须重新编译。 | 核对策略生成、请求复制、持久化/序列化、回测撮合和 CTP 字段映射是否保留类型；按交易接口规则定义各类型校验，确认目标柜台支持情况。覆盖默认限价、显式限价、市价、FAK、FOK 的字段、校验、成交及撤销语义。 |

## 现有边界

- `xtrader_demo` 当前仅创建 `runtime_engine`，没有实例化并启动 `ctp_gateway`，也没有注册通往网关的订单处理器；本次修复不改变这一点。
- `version.hh` 由 `apps/xtrader/version.hh.in` 经 CMake 配置生成，模板版本为 `0.1.3`，不是本次新增的手写源文件。缺失时应在 Linux 上重新配置构建目录。
- 上述修复只针对已经确认的编译阻塞，不承诺修复全部构建错误或完成真实交易验收。

## Linux 验证结果及剩余阻塞

在 Ubuntu VM 的 `seastar_xtrade` 仓库应用同一份头文件修改后，使用现有 Release 构建的编译参数执行检查：

- `ctp_gateway.cc` 的 C++ 语法检查通过，原有登录类型不可见、`request.type` 缺失问题未再出现；仍有已有的 `smp::count` 弃用警告。
- `ctp_spi_adapter.cc` 原先读取不存在的 `Volume`、`TodayVolume`。后续已核对当前 SDK 定义，分别改为 `Position`（该返回记录的持仓数量）和 `TodayPosition`（今仓数量），保留原日志标签。该回调目前仅打印柜台查询结果，没有将其写入系统持仓账本；不新增内存变量代替柜台数据，也不根据单条记录推算账户总持仓。后续接入账本时仍需按合约、买卖方向、投保标志及持仓日期等维度归并，并处理查询结束、错误及成交回报并发，防止重复累计。
- 网关目标的完整优化编译耗时较长，已主动中断后改为语法检查；Demo 尚未完成完整编译、链接和运行验证。

## 追加：runtime_engine 不完整类型修复

日期：2026-11-05。`runtime_engine.hh` 仅前置声明 `trading_account_service`，其内联默认构造及移动操作可能实例化 `unique_ptr` 的清理逻辑，要求账户服务类型完整。将默认构造、移动构造和移动赋值的定义移至已包含 `trading_account_service.hh` 的 `runtime_engine.cc`，头文件仅保留声明；移动操作保留 `noexcept`，析构函数保持原状。

| 编号 | 级别 | 涉及对象 | 风险及影响 | 后续处理与验收要求 |
|---|---|---|---|---|
| CTP-R04 | 低 | 特殊成员函数移至实现文件 | 保持原有成员初始化及默认移动行为，不改变账户和持仓逻辑；调用方需要链接 `runtime_engine.cc` 产生的目标文件。 | 编译、链接 Demo，确认默认构造及析构可用；核对需要移动操作的调用方。 |
| CTP-R05 | 中（既有） | 运行中 runtime_engine 的移动 | 默认移动会转移账户服务和回调，但源对象的标量状态不会自动清空。若回调捕获旧对象地址，或账户服务跨 shard 移动，可能导致状态、对象引用或线程归属不一致。本次只移动函数定义，不新增此风险，也不解决它。 | 不将本次修复视为支持运行中对象任意移动；后续审查移动调用点、回调捕获及 shard 归属，明确生命周期约束并覆盖相应场景。 |

### 本轮编译结果

- Ubuntu VM 中 `xtrader_demo.cc`、`runtime_engine.cc`、`runtime_sharded.cc` 已编译通过，原有不完整类型错误未再出现；默认移动操作的定义也被编译器接受。
- `app_xtrader_cli` 构建检查通过，Ninja 报告无需重建。
- Demo 后续在 `src/xtrader/redis_metadata.cc:56` 阻塞：匿名命名空间中的 `unescape_csv()` 未被使用，触发 `-Werror=unused-function`。本轮没有修改该函数，也未降低告警等级。
- `event_bus.cc:122` 另有忽略 `future<>` 返回值的已有警告，本轮未处理。
- 出现上述错误后，已主动中断尚未完成的并行编译；Demo 本轮没有完成链接，不能视为完整构建通过。

## 追加：队列初始化编译开销修复

日期：2026-11-05。GCC 11.4 将 `_slots{}` 中聚合槽位的成员初始化逐项展开，
仅网关构造部分的 GIMPLE 就达到约 17.7 MB。此前构建持续 2 小时 25 分钟，
多次采样停留在编译器变量位置跟踪处理，随后按要求中断；不是业务代码运行时死循环。

- 为 `md_slot`、`trader_slot` 增加用户定义的默认构造函数，保留原有成员默认初始化、
  2048/4096 队列容量、内存布局及 push/pop 行为。不能改成 `= default`：
  这里需要用户定义的构造函数来避免原有逐成员聚合展开。槽类型不再是聚合，
  已核对仓库调用点均为默认构造，没有多参数或指定成员聚合初始化。
- 将 `ctp_str_copy()` 的 `strncpy` 改为有界复制和补零，保持原有遇到空字符停止、
  最多复制 `N - 1` 字节、剩余目标空间补零的行为，消除 `-Werror=stringop-truncation`。
- 删除未使用的 `unescape_csv()`；CSV 解析仍由已有 `split_csv_line()` 处理。
- 在 Ubuntu VM 保留原 `-O2 -g` 和告警等级，执行
  `ninja -C build/release -j1 app_xtrader_demo app_xtrader_cli`：
  网关文件实际耗时约 **17.6 秒**，生成约 3.9 MiB 目标文件；
  CTP 适配器及 Redis 元数据也已通过编译。
- 整轮构建约 35.3 秒后因 Web 模块已有错误退出：包括 `http::request` 声明不可见、
  `sstring::push_back` 不存在、`json_kv(size_t)` 重复定义及 HTTP 服务接口不匹配。
  本次没有修改 Web 模块，Demo 尚未完成链接。
- CLI 单独构建报告无需重建，已有程序 `--help` 退出码为 0。

本轮日志：`/home/vboxuser/build-logs/seastar_xtrade_fixed_20261105_154603.log`。
以上结果说明初始化编译开销已解除，不代表 Demo 或真实交易链路已验收通过。
