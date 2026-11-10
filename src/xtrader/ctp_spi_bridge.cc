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
#include <seastar/xtrader/ctp_callback_bridge.hh>
#include <seastar/xtrader/ctp_gateway.hh>
#include <seastar/xtrader/ctp_spi_adapter.hh>

namespace seastar::xtrader {
namespace {

// B4: callback threads copy SDK fields only; conversion happens on the owner.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
template <typename Event, typename Kind>
Event make_callback(Kind kind, uint64_t generation, int request_id = 0, bool is_last = false,
                    const CThostFtdcRspInfoField* info = nullptr) noexcept {
    Event event;
    event.kind = kind;
    event.session_generation = generation;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_response_info = info != nullptr;
    if (info) {
        event.response_info = *info;
    }
    return event;
}

} // namespace

void MdSpiAdapter::OnFrontConnected() {
    if (_gateway) {
        _gateway->post_market_callback(make_callback<ctp_market_callback>(
            ctp_market_callback_kind::connected, _session_generation));
    }
}

void MdSpiAdapter::OnFrontDisconnected(int reason) {
    if (_gateway) {
        auto event = make_callback<ctp_market_callback>(ctp_market_callback_kind::disconnected, _session_generation);
        event.disconnect_reason = reason;
        _gateway->post_market_callback(event);
    }
}

void MdSpiAdapter::OnRspUserLogin(CThostFtdcRspUserLoginField* response, CThostFtdcRspInfoField* info,
                                 int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_market_callback>(
            ctp_market_callback_kind::login, _session_generation, request_id, is_last, info);
        event.has_payload = response != nullptr;
        if (response) {
            event.payload.login = *response;
        }
        _gateway->post_market_callback(event);
    }
}

void MdSpiAdapter::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* data) {
    if (_gateway) {
        auto event = make_callback<ctp_market_callback>(ctp_market_callback_kind::market_data, _session_generation);
        event.has_payload = data != nullptr;
        if (data) {
            event.payload.market_data = *data;
        }
        _gateway->post_market_callback(event);
    }
}

void MdSpiAdapter::OnRspError(CThostFtdcRspInfoField* info, int request_id, bool is_last) {
    if (_gateway) {
        _gateway->post_market_callback(make_callback<ctp_market_callback>(
            ctp_market_callback_kind::error, _session_generation, request_id, is_last, info));
    }
}

void MdSpiAdapter::deliver(ctp_market_callback& event) {
    auto* info = event.has_response_info ? &event.response_info : nullptr;
    switch (event.kind) {
    case ctp_market_callback_kind::connected:
        return deliver_front_connected();
    case ctp_market_callback_kind::disconnected:
        return deliver_front_disconnected(event.disconnect_reason);
    case ctp_market_callback_kind::login:
        return deliver_login(event.has_payload ? &event.payload.login : nullptr, info,
                             event.request_id, event.is_last);
    case ctp_market_callback_kind::market_data:
        return deliver_market_data(event.has_payload ? &event.payload.market_data : nullptr);
    case ctp_market_callback_kind::error:
        return deliver_error(info, event.request_id, event.is_last);
    case ctp_market_callback_kind::invalid:
        break;
    }
    _gateway->on_error(-1, "invalid CTP market callback kind");
}

void TraderSpiAdapter::OnFrontConnected() {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::connected, _session_generation));
    }
}

void TraderSpiAdapter::OnFrontDisconnected(int reason) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(ctp_trader_callback_kind::disconnected, _session_generation);
        event.disconnect_reason = reason;
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info,
                                        int request_id, bool is_last) {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::authenticate, _session_generation, request_id, is_last, info));
    }
}

void TraderSpiAdapter::OnRspUserLogin(CThostFtdcRspUserLoginField* response, CThostFtdcRspInfoField* info,
                                      int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::login, _session_generation, request_id, is_last, info);
        event.has_payload = response != nullptr;
        if (response) {
            event.payload.login = *response;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRspUserLogout(CThostFtdcUserLogoutField*, CThostFtdcRspInfoField* info,
                                       int request_id, bool is_last) {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::logout, _session_generation, request_id, is_last, info));
    }
}

void TraderSpiAdapter::OnRspOrderInsert(CThostFtdcInputOrderField* order, CThostFtdcRspInfoField* info,
                                        int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::order_insert_response, _session_generation, request_id, is_last, info);
        event.has_payload = order != nullptr;
        if (order) {
            event.payload.input_order = *order;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnErrRtnOrderInsert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField* info) {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::error, _session_generation, 0, true, info));
    }
}

void TraderSpiAdapter::OnRtnOrder(CThostFtdcOrderField* order) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(ctp_trader_callback_kind::order_return, _session_generation);
        event.has_payload = order != nullptr;
        if (order) {
            event.payload.order = *order;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRtnTrade(CThostFtdcTradeField* trade) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(ctp_trader_callback_kind::trade_return, _session_generation);
        event.has_payload = trade != nullptr;
        if (trade) {
            event.payload.trade = *trade;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRspOrderAction(CThostFtdcInputOrderActionField* action, CThostFtdcRspInfoField* info,
                                        int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::order_action_response, _session_generation, request_id, is_last, info);
        event.has_payload = action != nullptr;
        if (action) {
            event.payload.order_action = *action;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnErrRtnOrderAction(CThostFtdcOrderActionField*, CThostFtdcRspInfoField* info) {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::error, _session_generation, 0, true, info));
    }
}

void TraderSpiAdapter::OnRspQryTradingAccount(CThostFtdcTradingAccountField* account,
                                              CThostFtdcRspInfoField* info, int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::account_query, _session_generation, request_id, is_last, info);
        event.has_payload = account != nullptr;
        if (account) {
            event.payload.account = *account;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* position,
                                                 CThostFtdcRspInfoField* info, int request_id, bool is_last) {
    if (_gateway) {
        auto event = make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::position_query, _session_generation, request_id, is_last, info);
        event.has_payload = position != nullptr;
        if (position) {
            event.payload.position = *position;
        }
        _gateway->post_trader_callback(event);
    }
}

void TraderSpiAdapter::OnRspError(CThostFtdcRspInfoField* info, int request_id, bool is_last) {
    if (_gateway) {
        _gateway->post_trader_callback(make_callback<ctp_trader_callback>(
            ctp_trader_callback_kind::error, _session_generation, request_id, is_last, info));
    }
}

void TraderSpiAdapter::deliver(ctp_trader_callback& event) {
    auto* info = event.has_response_info ? &event.response_info : nullptr;
    switch (event.kind) {
    case ctp_trader_callback_kind::connected:
        return deliver_front_connected();
    case ctp_trader_callback_kind::disconnected:
        return deliver_front_disconnected(event.disconnect_reason);
    case ctp_trader_callback_kind::authenticate:
        return deliver_authenticate(nullptr, info, event.request_id, event.is_last);
    case ctp_trader_callback_kind::login:
        return deliver_login(event.has_payload ? &event.payload.login : nullptr, info,
                             event.request_id, event.is_last);
    case ctp_trader_callback_kind::logout:
        return deliver_logout(nullptr, info, event.request_id, event.is_last);
    case ctp_trader_callback_kind::order_insert_response:
        return deliver_order_insert(event.has_payload ? &event.payload.input_order : nullptr, info,
                                    event.request_id, event.is_last);
    case ctp_trader_callback_kind::order_return:
        return deliver_order(event.has_payload ? &event.payload.order : nullptr);
    case ctp_trader_callback_kind::trade_return:
        return deliver_trade(event.has_payload ? &event.payload.trade : nullptr);
    case ctp_trader_callback_kind::order_action_response:
        return deliver_order_action(event.has_payload ? &event.payload.order_action : nullptr, info,
                                    event.request_id, event.is_last);
    case ctp_trader_callback_kind::account_query:
        return deliver_account(event.has_payload ? &event.payload.account : nullptr, info,
                               event.request_id, event.is_last);
    case ctp_trader_callback_kind::position_query:
        return deliver_position(event.has_payload ? &event.payload.position : nullptr, info,
                                event.request_id, event.is_last);
    case ctp_trader_callback_kind::error:
        return deliver_error(info, event.request_id, event.is_last);
    case ctp_trader_callback_kind::invalid:
        break;
    }
    _gateway->on_error(-1, "invalid CTP trader callback kind");
}

} // namespace seastar::xtrader
