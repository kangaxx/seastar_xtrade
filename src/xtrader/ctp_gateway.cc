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
#include <seastar/xtrader/ctp_gateway.hh>
#include <seastar/xtrader/ctp_spi_adapter.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/reactor.hh>

#include <ThostFtdcMdApi.h>
#include <ThostFtdcTraderApi.h>

#include <cstring>
#include <iostream>

namespace seastar::xtrader {

namespace {

// Safe string copy into CTP fixed-size char arrays
template<size_t N>
void ctp_str_copy(char (&dst)[N], const sstring& src) {
    std::strncpy(dst, src.c_str(), N - 1);
    dst[N - 1] = '\0';
}

// Map domain::side to CTP direction
char to_ctp_direction(domain::side s) {
    return (s == domain::side::buy) ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
}

// Map domain::offset_flag to CTP offset flag char
char to_ctp_offset(domain::offset_flag f) {
    switch (f) {
        case domain::offset_flag::open:            return THOST_FTDC_OF_Open;
        case domain::offset_flag::close:           return THOST_FTDC_OF_Close;
        case domain::offset_flag::close_today:     return THOST_FTDC_OF_CloseToday;
        case domain::offset_flag::close_yesterday: return THOST_FTDC_OF_CloseYesterday;
        default: return THOST_FTDC_OF_Open;
    }
}

// Map domain::order_type to CTP price type
char to_ctp_price_type(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::fak:    return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::fok:    return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::market: return THOST_FTDC_OPT_AnyPrice;
        default: return THOST_FTDC_OPT_LimitPrice;
    }
}

// Map domain::order_type to CTP time condition
char to_ctp_time_condition(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_TC_GFD;
        case domain::order_type::fak:    return THOST_FTDC_TC_IOC;
        case domain::order_type::fok:    return THOST_FTDC_TC_IOC;
        case domain::order_type::market: return THOST_FTDC_TC_IOC;
        default: return THOST_FTDC_TC_GFD;
    }
}

// Map domain::order_type to CTP volume condition
char to_ctp_volume_condition(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_VC_AV;
        case domain::order_type::fak:    return THOST_FTDC_VC_AV;
        case domain::order_type::fok:    return THOST_FTDC_VC_CV;
        case domain::order_type::market: return THOST_FTDC_VC_AV;
        default: return THOST_FTDC_VC_AV;
    }
}

} // anonymous namespace

// ==================== ctp_gateway ====================

ctp_gateway::ctp_gateway(gateway_config config)
    : _config(std::move(config))
{}

ctp_gateway::~ctp_gateway() {
    // Ensure API instances are released
    if (_md_api) {
        _md_api->Release();
    }
    if (_td_api) {
        _td_api->Release();
    }
}

unsigned ctp_gateway::instrument_shard_id(const sstring& instrument_id, unsigned num_shards) {
    if (num_shards <= 1) {
        return default_account_shard;
    }
    unsigned hash = 0;
    for (char c : instrument_id) {
        hash = hash * 31 + static_cast<unsigned>(c);
    }
    return (hash % (num_shards - 1)) + 1;
}

// ==================== Lifecycle ====================

future<> ctp_gateway::start() {
    if (_status == gateway_status::ready) {
        return make_ready_future<>();
    }

    _status = gateway_status::connecting;

    // Create flow directories for CTP (stores connection info)
    _md_api = CThostFtdcMdApi::CreateFtdcMdApi("./flow/md/");
    _td_api = CThostFtdcTraderApi::CreateFtdcTraderApi("./flow/td/");

    if (!_md_api || !_td_api) {
        std::cerr << "[FATAL] ctp_gateway::start: failed to create CTP API instances" << std::endl;
        _status = gateway_status::error;
        return make_exception_future<>(std::runtime_error("Failed to create CTP API instances"));
    }

    // Create SPI adapters
    _md_spi = std::make_unique<MdSpiAdapter>(this);
    _td_spi = std::make_unique<TraderSpiAdapter>(this);

    // Register SPIs
    _md_api->RegisterSpi(_md_spi.get());
    _td_api->RegisterSpi(_td_spi.get());

    // Register front addresses
    _md_api->RegisterFront(const_cast<char*>(_config.md_front_addr.c_str()));
    _td_api->RegisterFront(const_cast<char*>(_config.td_front_addr.c_str()));

    // Init API instances (starts connection)
    _md_api->Init();
    _td_api->Init();

    // Start drain timers
    _md_drain_timer.set_callback([this] { drain_md_ring(); });
    _md_drain_timer.arm_periodic(std::chrono::microseconds(max_drain_us));

    _trader_drain_timer.set_callback([this] { drain_trader_ring(); });
    _trader_drain_timer.arm_periodic(std::chrono::microseconds(100));

    std::cout << "[INFO] ctp_gateway::start: CTP API instances created, waiting for connection..." << std::endl;

    // Wait for connections, then authenticate and login
    return connect_md().then([this] {
        return connect_td();
    }).then([this] {
        return login_td();
    }).then([this] {
        return login_md();
    }).then([this] {
        _status = gateway_status::ready;
        std::cout << "[INFO] ctp_gateway::start: gateway ready "
                   << "front_id=" << _front_id
                   << " session_id=" << _session_id
                   << " trading_day=" << _trading_day << std::endl;
        return make_ready_future<>();
    });
}

future<> ctp_gateway::stop() {
    std::cout << "[INFO] ctp_gateway::stop: shutting down..." << std::endl;

    // Cancel drain timers
    _md_drain_timer.cancel();
    _trader_drain_timer.cancel();

    // Logout from CTP (fire and forget - best effort)
    if (_td_api && _status == gateway_status::ready) {
        CThostFtdcUserLogoutField logout_field{};
        ctp_str_copy(logout_field.BrokerID, _config.broker_id);
        ctp_str_copy(logout_field.UserID, _config.investor_id);
        _td_api->ReqUserLogout(&logout_field, next_request_id());
    }

    // Release API instances
    if (_md_api) {
        _md_api->Release();
        _md_api = nullptr;
    }
    if (_td_api) {
        _td_api->Release();
        _td_api = nullptr;
    }

    _md_spi.reset();
    _td_spi.reset();

    _status = gateway_status::disconnected;

    std::cout << "[INFO] ctp_gateway::stop: shutdown complete" << std::endl;
    return make_ready_future<>();
}

// ==================== Connection Sub-Steps ====================

future<> ctp_gateway::connect_md() {
    _md_connected_promise = promise<>();
    return _md_connected_promise->get_future().then([this] {
        std::cout << "[INFO] ctp_gateway: MD front connected" << std::endl;
        return make_ready_future<>();
    });
}

future<> ctp_gateway::connect_td() {
    _td_connected_promise = promise<>();
    return _td_connected_promise->get_future().then([this] {
        std::cout << "[INFO] ctp_gateway: TD front connected" << std::endl;
        return make_ready_future<>();
    });
}

future<> ctp_gateway::login_md() {
    if (!_md_api) {
        return make_exception_future<>(std::runtime_error("MdApi not initialized"));
    }

    _md_login_promise = promise<>();

    CThostFtdcReqUserLoginField login_field{};
    fill_login_field(&login_field, _config.broker_id, _config.investor_id,
                     _config.password, _config.user_product_info);

    std::cout << "[INFO] ctp_gateway: sending MD login request..." << std::endl;
    int ret = _md_api->ReqUserLogin(&login_field, next_request_id());
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway: ReqUserLogin (MD) failed, ret=" << ret << std::endl;
        if (_md_login_promise) {
            _md_login_promise->set_exception(std::runtime_error("MD login failed"));
            _md_login_promise.reset();
        }
        return make_exception_future<>(std::runtime_error("MD login failed"));
    }

    return _md_login_promise->get_future().then([this] {
        std::cout << "[INFO] ctp_gateway: MD login success" << std::endl;
        return make_ready_future<>();
    });
}

future<> ctp_gateway::login_td() {
    if (!_td_api) {
        return make_exception_future<>(std::runtime_error("TraderApi not initialized"));
    }

    _td_authenticated_promise = promise<>();
    _td_login_promise = promise<>();

    // Step 1: Authenticate
    CThostFtdcReqAuthenticateField auth_field{};
    ctp_str_copy(auth_field.BrokerID, _config.broker_id);
    ctp_str_copy(auth_field.UserID, _config.investor_id);
    ctp_str_copy(auth_field.AuthCode, _config.auth_code);
    ctp_str_copy(auth_field.AppID, _config.app_id);
    ctp_str_copy(auth_field.UserProductInfo, _config.user_product_info);

    _status = gateway_status::authenticating;
    std::cout << "[INFO] ctp_gateway: sending authenticate request..." << std::endl;

    int ret = _td_api->ReqAuthenticate(&auth_field, next_request_id());
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway: ReqAuthenticate failed, ret=" << ret << std::endl;
        if (_td_authenticated_promise) {
            _td_authenticated_promise->set_exception(std::runtime_error("TD authentication failed"));
            _td_authenticated_promise.reset();
        }
        return make_exception_future<>(std::runtime_error("TD authentication failed"));
    }

    return _td_authenticated_promise->get_future().then([this] {
        std::cout << "[INFO] ctp_gateway: authentication success, sending login..." << std::endl;

        // Step 2: Login
        CThostFtdcReqUserLoginField login_field{};
        fill_login_field(&login_field, _config.broker_id, _config.investor_id,
                         _config.password, _config.user_product_info);

        _status = gateway_status::logging_in;
        int login_ret = _td_api->ReqUserLogin(&login_field, next_request_id());
        if (login_ret != 0) {
            std::cerr << "[ERROR] ctp_gateway: ReqUserLogin (TD) failed, ret=" << login_ret << std::endl;
            if (_td_login_promise) {
                _td_login_promise->set_exception(std::runtime_error("TD login failed"));
                _td_login_promise.reset();
            }
            return make_exception_future<>(std::runtime_error("TD login failed"));
        }

        return _td_login_promise->get_future().then([this] {
            std::cout << "[INFO] ctp_gateway: TD login success" << std::endl;
            return make_ready_future<>();
        });
    });
}

void ctp_gateway::fill_login_field(CThostFtdcReqUserLoginField* field,
                                    const sstring& broker_id,
                                    const sstring& user_id,
                                    const sstring& password,
                                    const sstring& product_info) {
    std::memset(field, 0, sizeof(CThostFtdcReqUserLoginField));
    ctp_str_copy(field->BrokerID, broker_id);
    ctp_str_copy(field->UserID, user_id);
    ctp_str_copy(field->Password, password);
    ctp_str_copy(field->UserProductInfo, product_info);
}

// ==================== Trading ====================

future<domain::order_status> ctp_gateway::submit_order(const domain::order_request& request) {
    if (_status != gateway_status::ready) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: gateway not ready, status="
                   << static_cast<int>(_status) << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    if (!request.is_valid()) {
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    if (!_td_api) {
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    // Generate order_ref
    ++_order_ref_seq;
    sstring order_ref_str = fmt::format("{:012d}", _order_ref_seq);

    // Build CTP order field
    CThostFtdcInputOrderField order_field{};
    std::memset(&order_field, 0, sizeof(order_field));

    ctp_str_copy(order_field.BrokerID, _config.broker_id);
    ctp_str_copy(order_field.InvestorID, _config.investor_id);
    ctp_str_copy(order_field.InstrumentID, request.instrument_id);
    ctp_str_copy(order_field.OrderRef, order_ref_str);
    ctp_str_copy(order_field.UserID, _config.investor_id);

    order_field.OrderPriceType = to_ctp_price_type(request.type);
    order_field.Direction = to_ctp_direction(request.direction);
    order_field.CombOffsetFlag[0] = to_ctp_offset(request.offset);
    order_field.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    order_field.LimitPrice = request.price;
    order_field.VolumeTotalOriginal = request.volume;
    order_field.TimeCondition = to_ctp_time_condition(request.type);
    order_field.VolumeCondition = to_ctp_volume_condition(request.type);
    order_field.MinVolume = (request.type == domain::order_type::fok) ? request.volume : 1;
    order_field.ContingentCondition = THOST_FTDC_CC_Immediately;
    order_field.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    order_field.IsAutoSuspend = 0;
    order_field.RequestID = static_cast<int>(next_request_id());

    // Record as pending order
    pending_order po;
    po.request = request;
    po.order_ref = order_ref_str;
    po.broker_id = _config.broker_id;
    po.investor_id = _config.investor_id;
    po.front_id = _front_id;
    po.session_id = _session_id;
    _pending_orders.add_order(order_ref_str, std::move(po));

    // Send order to CTP
    std::cout << "[INFO] ctp_gateway::submit_order: sending order "
              << "instrument=" << request.instrument_id
              << " direction=" << (request.direction == domain::side::buy ? "buy" : "sell")
              << " offset=" << static_cast<int>(request.offset)
              << " volume=" << request.volume
              << " price=" << request.price
              << " order_ref=" << order_ref_str << std::endl;

    int ret = _td_api->ReqOrderInsert(&order_field, order_field.RequestID);
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: ReqOrderInsert failed, ret=" << ret << std::endl;
        _pending_orders.update_status(order_ref_str, domain::order_status::rejected);
        _pending_orders.set_error(order_ref_str, "ReqOrderInsert failed");
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    // Note: actual order status will be returned via OnRtnOrder/OnRspOrderInsert callbacks
    return make_ready_future<domain::order_status>(domain::order_status::submitted);
}

future<domain::order_status> ctp_gateway::cancel_order(const sstring& order_sys_id) {
    if (_status != gateway_status::ready) {
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    if (!_td_api) {
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    // Look up pending order
    auto* po = _pending_orders.get_by_sys_id(order_sys_id);
    if (!po) {
        std::cerr << "[ERROR] ctp_gateway::cancel_order: order_sys_id not found: " << order_sys_id << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    if (!_pending_orders.can_cancel(po->order_ref)) {
        std::cerr << "[ERROR] ctp_gateway::cancel_order: order cannot be canceled: " << order_sys_id << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    // Build cancel request
    CThostFtdcInputOrderActionField action_field{};
    std::memset(&action_field, 0, sizeof(action_field));

    ctp_str_copy(action_field.BrokerID, _config.broker_id);
    ctp_str_copy(action_field.InvestorID, _config.investor_id);
    ctp_str_copy(action_field.OrderRef, po->order_ref);
    action_field.FrontID = po->front_id;
    action_field.SessionID = po->session_id;
    ctp_str_copy(action_field.OrderSysID, order_sys_id);
    action_field.ActionFlag = THOST_FTDC_AF_Delete;
    ctp_str_copy(action_field.InstrumentID, po->request.instrument_id);
    action_field.RequestID = static_cast<int>(next_request_id());

    std::cout << "[INFO] ctp_gateway::cancel_order: canceling order_sys_id=" << order_sys_id
              << " order_ref=" << po->order_ref << std::endl;

    int ret = _td_api->ReqOrderAction(&action_field, action_field.RequestID);
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway::cancel_order: ReqOrderAction failed, ret=" << ret << std::endl;
        return make_ready_future<domain::order_status>(domain::order_status::rejected);
    }

    return make_ready_future<domain::order_status>(domain::order_status::submitted);
}

// ==================== Account Sync ====================

future<domain::trading_account> ctp_gateway::sync_account_from_ctp() {
    if (_status != gateway_status::ready) {
        std::cerr << "[ERROR] ctp_gateway::sync_account_from_ctp: gateway not ready" << std::endl;
        return make_exception_future<domain::trading_account>(
            std::runtime_error("Gateway not ready"));
    }

    if (!_td_api) {
        return make_exception_future<domain::trading_account>(
            std::runtime_error("TraderApi not initialized"));
    }

    _account_query_promise = promise<domain::trading_account>();

    CThostFtdcQryTradingAccountField query_field{};
    std::memset(&query_field, 0, sizeof(query_field));
    ctp_str_copy(query_field.BrokerID, _config.broker_id);
    ctp_str_copy(query_field.InvestorID, _config.investor_id);
    ctp_str_copy(query_field.CurrencyID, "CNY");

    std::cout << "[INFO] ctp_gateway::sync_account_from_ctp: querying account..." << std::endl;

    int ret = _td_api->ReqQryTradingAccount(&query_field, next_request_id());
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway::sync_account_from_ctp: ReqQryTradingAccount failed, ret="
                   << ret << std::endl;
        if (_account_query_promise) {
            _account_query_promise->set_exception(std::runtime_error("ReqQryTradingAccount failed"));
            _account_query_promise.reset();
        }
        return make_exception_future<domain::trading_account>(
            std::runtime_error("ReqQryTradingAccount failed"));
    }

    return _account_query_promise->get_future().then([](domain::trading_account account) {
        return make_ready_future<domain::trading_account>(std::move(account));
    });
}

// ==================== Connection State Callbacks ====================

void ctp_gateway::on_md_front_connected() {
    std::cout << "[ctp_gateway] MD front connected callback" << std::endl;
    if (_md_connected_promise) {
        _md_connected_promise->set_value();
        _md_connected_promise.reset();
    }
}

void ctp_gateway::on_td_front_connected() {
    std::cout << "[ctp_gateway] TD front connected callback" << std::endl;
    if (_td_connected_promise) {
        _td_connected_promise->set_value();
        _td_connected_promise.reset();
    }
}

void ctp_gateway::on_md_front_disconnected(int reason) {
    std::cerr << "[ctp_gateway] MD front disconnected, reason=" << reason << std::endl;
}

void ctp_gateway::on_td_front_disconnected(int reason) {
    std::cerr << "[ctp_gateway] TD front disconnected, reason=" << reason << std::endl;
}

void ctp_gateway::on_md_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp) {
    if (!_md_login_promise) {
        return;
    }

    if (success && pRsp) {
        _trading_day = sstring(pRsp->TradingDay);
        _md_login_promise->set_value();
    } else {
        _md_login_promise->set_exception(std::runtime_error("MD login failed"));
    }
    _md_login_promise.reset();
}

void ctp_gateway::on_td_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp) {
    if (!_td_login_promise) {
        return;
    }

    if (success && pRsp) {
        _front_id = pRsp->FrontID;
        _session_id = pRsp->SessionID;
        _trading_day = sstring(pRsp->TradingDay);
        _td_login_promise->set_value();
    } else {
        _td_login_promise->set_exception(std::runtime_error("TD login failed"));
    }
    _td_login_promise.reset();
}

void ctp_gateway::on_authenticate_result(bool success) {
    if (!_td_authenticated_promise) {
        return;
    }

    if (success) {
        _td_authenticated_promise->set_value();
    } else {
        _td_authenticated_promise->set_exception(std::runtime_error("TD authentication failed"));
    }
    _td_authenticated_promise.reset();
}

void ctp_gateway::on_account_query_result(const CThostFtdcTradingAccountField* pAccount,
                                           bool success, bool is_last) {
    if (!_account_query_promise) {
        return;
    }

    if (!success) {
        _account_query_promise->set_exception(std::runtime_error("Account query failed"));
        _account_query_promise.reset();
        return;
    }

    if (pAccount) {
        domain::trading_account account;
        account.pre_balance = pAccount->PreBalance;
        account.balance = pAccount->Balance;
        account.available = pAccount->Available;
        account.curr_margin = pAccount->CurrMargin;
        account.frozen_margin = pAccount->FrozenMargin;
        account.frozen_commission = pAccount->FrozenCommission;
        account.commission = pAccount->Commission;
        account.close_profit = pAccount->CloseProfit;
        account.position_profit = pAccount->PositionProfit;
        account.trading_day = sstring(pAccount->TradingDay);

        std::cout << "[INFO] ctp_gateway::on_account_query_result: "
                  << "balance=" << account.balance
                  << " available=" << account.available
                  << " margin=" << account.curr_margin
                  << " frozen_margin=" << account.frozen_margin << std::endl;

        _account_query_promise->set_value(std::move(account));
    } else if (is_last) {
        // No account data returned
        _account_query_promise->set_value(domain::trading_account{});
    }

    if (is_last) {
        _account_query_promise.reset();
    }
}

// ==================== SPI Callback Entry Points ====================

void ctp_gateway::on_market_data(const domain::market_data& data) {
    md_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.data = data;

    if (!_md_ring.push(slot)) {
        ++_dropped_md_events;
        std::cerr << "[WARN] ctp_gateway: md_ring full, dropped market data for "
                  << data.instrument_id << std::endl;
    }
}

void ctp_gateway::on_trade_report(const domain::trade_report& report) {
    trader_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.type = domain::event_type::trade;
    slot.trade_data = report;
    slot.order_data = domain::order{};

    // Update pending order traded volume
    _pending_orders.update_traded_volume(report.order_ref, report.volume);

    if (!_trader_ring.push(slot)) {
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped trade event for "
                  << report.instrument_id << " (CRITICAL: violates zero-drop guarantee)" << std::endl;
    }
}

void ctp_gateway::on_order_return(const domain::order& order) {
    trader_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.type = domain::event_type::order;
    slot.order_data = order;
    slot.trade_data = domain::trade_report{};

    // Update pending order status
    _pending_orders.update_status(order.order_ref, order.status);

    if (!_trader_ring.push(slot)) {
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped order event for "
                  << order.order_sys_id << std::endl;
    }
}

void ctp_gateway::on_cancel_return(const domain::order& order) {
    trader_slot slot;
    slot.capture_ns = seastar::steady_clock_type::now().time_since_epoch().count();
    slot.type = domain::event_type::cancel;
    slot.order_data = order;
    slot.trade_data = domain::trade_report{};

    _pending_orders.update_status(order.order_sys_id, order.status);

    if (!_trader_ring.push(slot)) {
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped cancel event for "
                  << order.order_sys_id << std::endl;
    }
}

void ctp_gateway::on_error(int error_id, const sstring& error_msg) {
    std::cerr << "[ERROR] ctp_gateway: CTP error " << error_id << ": " << error_msg << std::endl;
}

// ==================== Drain Loops ====================

void ctp_gateway::drain_md_ring() {
    md_slot slot;
    size_t drained = 0;
    const auto start_time = seastar::steady_clock_type::now();

    while (drained < max_drain_per_tick) {
        const auto elapsed = seastar::steady_clock_type::now() - start_time;
        if (elapsed.count() > static_cast<long>(max_drain_us * 1000)) {
            break;
        }

        if (!_md_ring.pop(slot)) {
            break;
        }

        const auto target_shard = instrument_shard_id(slot.data.instrument_id, smp::count);

        (void)smp::submit_to(target_shard, [slot = std::move(slot)] {
            // TODO(Phase 4): Process market data on target shard
        });

        ++drained;
    }
}

void ctp_gateway::drain_trader_ring() {
    trader_slot slot;

    while (_trader_ring.pop(slot)) {
        unsigned target_shard;

        if (slot.type == domain::event_type::trade) {
            if (_trade_handler) {
                _trade_handler(slot.trade_data);
            }
            continue;
        } else if (slot.type == domain::event_type::order) {
            target_shard = instrument_shard_id(slot.order_data.instrument_id, smp::count);
        } else {
            target_shard = gateway_shard_id;
        }

        (void)smp::submit_to(target_shard, [slot = std::move(slot)] {
        });
    }
}

} // namespace seastar::xtrader
