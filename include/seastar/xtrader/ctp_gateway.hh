/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/xtrader/domain_types.hh>
#include <seastar/xtrader/pending_order_manager.hh>
#include <seastar/xtrader/spsc_ring_buffer.hh>

#include <memory>
#include <optional>
#include <functional>
#include <atomic>
#include <limits>
#include <stdexcept>

// Forward declarations for CTP types
class CThostFtdcMdApi;
class CThostFtdcTraderApi;
struct CThostFtdcTradingAccountField;
struct CThostFtdcRspUserLoginField;
struct CThostFtdcReqUserLoginField;

namespace seastar::xtrader {

class MdSpiAdapter;
class TraderSpiAdapter;
struct ctp_market_callback;
struct ctp_trader_callback;
struct ctp_gateway_callback_state;

// === Gateway Configuration ===

struct gateway_config {
    sstring broker_id;
    sstring investor_id;
    sstring password;
    sstring auth_code;
    sstring app_id;
    sstring user_product_info;
    sstring md_front_addr;
    sstring td_front_addr;
    // B1: the current gateway and account are both owned by shard 0.
    // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
    int account_shard_id = 0;
};

// === Gateway Status ===

enum class gateway_status {
    disconnected,
    connecting,
    authenticating,
    logging_in,
    ready,
    error,
};

// === CTP Gateway ===
//
// Responsibilities (Shard 0 only):
// - Hold MdApi and TraderApi instances
// - Copy CTP callbacks into bounded POD queues without allocating on SDK threads
// - Convert and deliver events to synchronous handlers on the owner shard
// - Execute ReqOrderInsert / ReqOrderAction serially
// - Manage OrderRef generation
// - Manage CTP API lifecycle (connect/authenticate/login/logout)
// - Sync account information from CTP
//
// Forbidden:
// - Do not directly modify any shard's position_book or trading_account
// - Do not hold cross-shard strategy pointers
//
class ctp_gateway {
public:
    explicit ctp_gateway(gateway_config config);
    ~ctp_gateway();

    ctp_gateway(const ctp_gateway&) = delete;
    ctp_gateway& operator=(const ctp_gateway&) = delete;
    ctp_gateway(ctp_gateway&&) = delete;
    ctp_gateway& operator=(ctp_gateway&&) = delete;

    // Lifecycle
    future<> start();
    future<> stop();

    // Trading
    [[nodiscard]] future<domain::order_status> submit_order(const domain::order_request& request);
    [[nodiscard]] future<domain::order_status> cancel_order(const sstring& order_sys_id);

    // Account
    future<domain::trading_account> sync_account_from_ctp();

    // B4: handlers run on the owner shard and must accept synchronously or throw.
    // An asynchronous downstream operation is not complete merely because it was queued.
    // Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
    using market_handler_t = std::function<void(const domain::market_data&)>;
    using order_handler_t = std::function<void(const domain::order&)>;
    using trade_handler_t = std::function<void(const domain::trade_report&)>;
    void set_market_handler(market_handler_t handler) {
        require_handler_configuration();
        _market_handler = std::move(handler);
    }
    void set_order_update_handler(order_handler_t handler) {
        require_handler_configuration();
        _order_handler = std::move(handler);
    }
    void set_trade_handler(trade_handler_t handler) {
        require_handler_configuration();
        _trade_handler = std::move(handler);
    }

    // Status
    [[nodiscard]] gateway_status status() const noexcept { return _status; }
    [[nodiscard]] int front_id() const noexcept { return _front_id; }
    [[nodiscard]] int session_id() const noexcept { return _session_id; }
    [[nodiscard]] sstring trading_day() const noexcept { return _trading_day; }

    // Shard routing
    static constexpr unsigned gateway_shard_id = 0;
    static unsigned instrument_shard_id(const sstring& instrument_id, unsigned num_shards);

    // B4: SDK threads post owned POD records; domain callbacks run on shard 0.
    // Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
    void post_market_callback(const ctp_market_callback& event) noexcept;
    void post_trader_callback(const ctp_trader_callback& event) noexcept;
    void drain_ctp_callbacks();

    // Domain callback entry points (called only on the gateway owner shard).
    void on_market_data(const domain::market_data& data);
    void on_trade_report(const domain::trade_report& report);
    void on_order_return(const domain::order& order);
    void on_cancel_return(const domain::order& order);
    void on_error(int error_id, const sstring& error_msg);

    // Connection state callbacks (called from SPI adapters)
    void on_md_front_connected();
    void on_td_front_connected();
    void on_md_front_disconnected(int reason);
    void on_td_front_disconnected(int reason);
    void on_md_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp, int request_id, bool is_last);
    void on_td_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp, int request_id, bool is_last);
    void on_authenticate_result(bool success, int request_id, bool is_last);
    void on_account_query_result(const CThostFtdcTradingAccountField* pAccount,
                                  bool success, bool is_last, int request_id);

    // Drain methods called by timer<>
    void drain_md_ring();
    void drain_trader_ring();

private:
    // Connection sub-steps
    future<> connect_md();
    future<> connect_td();
    future<> login_md();
    future<> login_td();

    // CTP field helpers
    void fill_login_field(CThostFtdcReqUserLoginField* field, const sstring& broker_id,
                          const sstring& user_id, const sstring& password,
                          const sstring& product_info);
    [[nodiscard]] bool callback_events_lost() const noexcept;
    void fail_callback_bridge(const char* reason);
    void require_handler_configuration() const {
        if (_status != gateway_status::disconnected) {
            throw std::logic_error("gateway handlers cannot change while it is active");
        }
    }

    gateway_config _config;
    gateway_status _status = gateway_status::disconnected;

    // CTP API instances (owned)
    CThostFtdcMdApi* _md_api = nullptr;
    CThostFtdcTraderApi* _td_api = nullptr;
    std::unique_ptr<MdSpiAdapter> _md_spi;
    std::unique_ptr<TraderSpiAdapter> _td_spi;
    std::unique_ptr<ctp_gateway_callback_state> _callback_state;
    std::atomic<bool> _callback_bridge_missing{false};
    bool _callback_failure_reported = false;
    uint64_t _callback_generation = 0;

    // Connection state
    int _front_id = 0;
    int _session_id = 0;
    sstring _trading_day;
    unsigned _request_id_seq = 0;
    int next_request_id() {
        if (_request_id_seq >= static_cast<unsigned>(std::numeric_limits<int>::max())) {
            throw std::overflow_error("CTP request identifier exhausted");
        }
        return static_cast<int>(++_request_id_seq);
    }

    // Ring buffers
    md_ring_buffer _md_ring;
    trader_ring_buffer _trader_ring;

    // Drain timers
    timer<> _md_drain_timer;
    timer<> _trader_drain_timer;

    // Pending order management
    pending_order_manager _pending_orders;
    uint64_t _order_ref_seq = 0;

    // Async operation promises
    std::optional<promise<>> _md_connected_promise;
    std::optional<promise<>> _td_connected_promise;
    std::optional<promise<>> _td_authenticated_promise;
    std::optional<promise<>> _md_login_promise;
    std::optional<promise<>> _td_login_promise;
    int _md_login_request_id = 0;
    int _td_login_request_id = 0;
    int _td_auth_request_id = 0;
    std::optional<promise<domain::trading_account>> _account_query_promise;
    std::optional<domain::trading_account> _account_query_data;
    int _account_query_request_id = 0;

    market_handler_t _market_handler;
    order_handler_t _order_handler;
    trade_handler_t _trade_handler;

    // Metrics
    uint64_t _dropped_md_events = 0;
    uint64_t _dropped_trader_events = 0;
};

// === Shard Configuration Constants ===

inline constexpr unsigned default_gateway_shard = 0;
inline constexpr unsigned default_account_shard = 0;

// === Drain Loop Constants ===

inline constexpr size_t max_drain_per_tick = 1024;
inline constexpr size_t max_drain_us = 500;

} // namespace seastar::xtrader
