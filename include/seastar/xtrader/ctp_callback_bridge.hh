/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#pragma once

#include <seastar/xtrader/bounded_callback_queue.hh>
#include <ThostFtdcUserApiStruct.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace seastar::xtrader {

// B4: fixed, self-contained SDK records cross threads without Seastar objects.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class ctp_market_callback_kind {
    invalid,
    connected,
    disconnected,
    login,
    market_data,
    error,
};

enum class ctp_trader_callback_kind {
    invalid,
    connected,
    disconnected,
    authenticate,
    login,
    logout,
    order_insert_response,
    order_return,
    trade_return,
    order_action_response,
    account_query,
    position_query,
    error,
};

union ctp_market_callback_payload {
    char empty;
    CThostFtdcRspUserLoginField login;
    CThostFtdcDepthMarketDataField market_data;

    ctp_market_callback_payload() noexcept : empty(0) {}
};

union ctp_trader_callback_payload {
    char empty;
    CThostFtdcRspUserLoginField login;
    CThostFtdcInputOrderField input_order;
    CThostFtdcOrderField order;
    CThostFtdcTradeField trade;
    CThostFtdcInputOrderActionField order_action;
    CThostFtdcTradingAccountField account;
    CThostFtdcInvestorPositionField position;

    ctp_trader_callback_payload() noexcept : empty(0) {}
};

struct ctp_market_callback {
    ctp_market_callback_kind kind = ctp_market_callback_kind::invalid;
    uint64_t session_generation = 0;
    int request_id = 0;
    int disconnect_reason = 0;
    bool is_last = false;
    bool has_payload = false;
    bool has_response_info = false;
    CThostFtdcRspInfoField response_info{};
    ctp_market_callback_payload payload;
};

struct ctp_trader_callback {
    ctp_trader_callback_kind kind = ctp_trader_callback_kind::invalid;
    uint64_t session_generation = 0;
    int request_id = 0;
    int disconnect_reason = 0;
    bool is_last = false;
    bool has_payload = false;
    bool has_response_info = false;
    CThostFtdcRspInfoField response_info{};
    ctp_trader_callback_payload payload;
};

// Producers may call post concurrently; only the owner shard calls pop.
// Construct a new bridge for a new lifetime after callbacks are fully drained.
template <size_t MarketCapacity, size_t TraderCapacity>
class ctp_callback_bridge {
public:
    ctp_callback_bridge()
        : _market(std::make_unique<market_queue>()), _trader(std::make_unique<trader_queue>()) {}

    bool post_market(const ctp_market_callback& event) noexcept { return _market->try_push(event); }
    bool post_trader(const ctp_trader_callback& event) noexcept { return _trader->try_push(event); }
    bool pop_market(ctp_market_callback& event) noexcept { return _market->try_pop(event); }
    bool pop_trader(ctp_trader_callback& event) noexcept { return _trader->try_pop(event); }
    [[nodiscard]] bool market_event_lost() const noexcept { return _market->has_lost_event(); }
    [[nodiscard]] bool trader_event_lost() const noexcept { return _trader->has_lost_event(); }

private:
    using market_queue = bounded_callback_queue<ctp_market_callback, MarketCapacity>;
    using trader_queue = bounded_callback_queue<ctp_trader_callback, TraderCapacity>;
    std::unique_ptr<market_queue> _market;
    std::unique_ptr<trader_queue> _trader;
};

} // namespace seastar::xtrader
