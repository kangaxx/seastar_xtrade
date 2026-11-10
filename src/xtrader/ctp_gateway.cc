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
#include <seastar/xtrader/ctp_callback_bridge.hh>
#include <seastar/xtrader/performance_metrics.hh>
#include <seastar/xtrader/ctp_spi_adapter.hh>
#include "ctp_field_utils.hh"
#include "ledger_validation.hh"
#include <seastar/core/smp.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/defer.hh>

#include <ThostFtdcMdApi.h>
#include <ThostFtdcTraderApi.h>

#include <cstring>
#include <charconv>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>

namespace seastar::xtrader {

// B4: each SDK callback queue owns only fixed-size, self-contained records.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
struct ctp_gateway_callback_state {
    ctp_callback_bridge<md_ring_capacity, trader_ring_capacity> bridge;
};

namespace {

// Safe string copy into CTP fixed-size char arrays
template<size_t N>
void ctp_str_copy(char (&dst)[N], const sstring& src) {
    ctp_detail::write_field(dst, src);
}

// Map domain::side to CTP direction
char to_ctp_direction(domain::side s) {
    switch (s) {
        case domain::side::buy:  return THOST_FTDC_D_Buy;
        case domain::side::sell: return THOST_FTDC_D_Sell;
    }
    throw std::invalid_argument("unsupported order direction");
}

// Map domain::offset_flag to CTP offset flag char
char to_ctp_offset(domain::offset_flag f) {
    switch (f) {
        case domain::offset_flag::open:            return THOST_FTDC_OF_Open;
        case domain::offset_flag::close:           return THOST_FTDC_OF_Close;
        case domain::offset_flag::close_today:     return THOST_FTDC_OF_CloseToday;
        case domain::offset_flag::close_yesterday: return THOST_FTDC_OF_CloseYesterday;
    }
    throw std::invalid_argument("unsupported order offset");
}

// Map domain::order_type to CTP price type
char to_ctp_price_type(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::fak:    return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::fok:    return THOST_FTDC_OPT_LimitPrice;
        case domain::order_type::market: return THOST_FTDC_OPT_AnyPrice;
    }
    throw std::invalid_argument("unsupported order type");
}

// Map domain::order_type to CTP time condition
char to_ctp_time_condition(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_TC_GFD;
        case domain::order_type::fak:    return THOST_FTDC_TC_IOC;
        case domain::order_type::fok:    return THOST_FTDC_TC_IOC;
        case domain::order_type::market: return THOST_FTDC_TC_IOC;
    }
    throw std::invalid_argument("unsupported order type");
}

// Map domain::order_type to CTP volume condition
char to_ctp_volume_condition(domain::order_type t) {
    switch (t) {
        case domain::order_type::limit:  return THOST_FTDC_VC_AV;
        case domain::order_type::fak:    return THOST_FTDC_VC_AV;
        case domain::order_type::fok:    return THOST_FTDC_VC_CV;
        case domain::order_type::market: return THOST_FTDC_VC_AV;
    }
    throw std::invalid_argument("unsupported order type");
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
    // B1: no CTP connection until trusted environment binding and readiness exist.
    // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
    constexpr bool trading_safety_ready = false;
    if (!trading_safety_ready) {
        return make_exception_future<>(std::runtime_error(
            "CTP gateway disabled: trusted environment binding and trading readiness are not implemented"));
    }
    if (this_shard_id() != gateway_shard_id ||
        _config.account_shard_id != static_cast<int>(gateway_shard_id)) {
        return make_exception_future<>(std::runtime_error(
            "CTP gateway and account must both use shard 0"));
    }
    if (_status == gateway_status::ready) {
        return make_ready_future<>();
    }
    if (_status != gateway_status::disconnected || _md_api || _td_api) {
        return make_exception_future<>(std::runtime_error("CTP gateway must be stopped before restarting"));
    }
    if (!_market_handler || !_order_handler || !_trade_handler) {
        return make_exception_future<>(std::runtime_error("CTP gateway event handlers are not installed"));
    }
    if (_pending_orders.pending_count() != 0) {
        return make_exception_future<>(std::runtime_error("pending orders require reconciliation before restart"));
    }
    if (_callback_generation == std::numeric_limits<uint64_t>::max()) {
        return make_exception_future<>(std::runtime_error("CTP callback generation exhausted"));
    }
    ++_callback_generation;
    _callback_state = std::make_unique<ctp_gateway_callback_state>();
    _callback_bridge_missing.store(false, std::memory_order_relaxed);
    _callback_failure_reported = false;
    _trading_day.clear();
    _front_id = 0;
    _session_id = 0;
    _order_ref_seq = 0;

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
    _md_spi = std::make_unique<MdSpiAdapter>(this, _callback_generation);
    _td_spi = std::make_unique<TraderSpiAdapter>(this, _callback_generation);

    // Register SPIs
    _md_api->RegisterSpi(_md_spi.get());
    _td_api->RegisterSpi(_td_spi.get());

    // Register front addresses
    _md_api->RegisterFront(const_cast<char*>(_config.md_front_addr.c_str()));
    _td_api->RegisterFront(const_cast<char*>(_config.td_front_addr.c_str()));

    // Both futures must exist before Init can deliver either connection callback.
    auto md_connected = connect_md();
    auto td_connected = connect_td();

    // Init API instances (starts connection)
    _md_api->Init();
    _td_api->Init();

    // Start drain timers
    _md_drain_timer.set_callback([this] { drain_md_ring(); });
    _md_drain_timer.arm_periodic(std::chrono::microseconds(max_drain_us));

    _trader_drain_timer.set_callback([this] {
        drain_ctp_callbacks();
        drain_trader_ring();
    });
    _trader_drain_timer.arm_periodic(std::chrono::microseconds(100));

    std::cout << "[INFO] ctp_gateway::start: CTP API instances created, waiting for connection..." << std::endl;

    // Wait for connections, then authenticate and login
    return when_all_succeed(std::move(md_connected), std::move(td_connected)).discard_result().then([this] {
        return login_td();
    }).then([this] {
        return login_md();
    }).then([this] {
        if (_callback_failure_reported || _status == gateway_status::error) {
            return make_exception_future<>(std::runtime_error("CTP callback bridge is not ready"));
        }
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

    if (_md_connected_promise || _td_connected_promise || _td_authenticated_promise
        || _md_login_promise || _td_login_promise || _account_query_promise) {
        fail_callback_bridge("gateway stopped while an operation was pending");
    }

    // Cancel drain timers
    _md_drain_timer.cancel();
    _trader_drain_timer.cancel();

    std::exception_ptr logout_failure;
    {
        auto release_api = defer([this] () noexcept {
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
        });

        if (_td_api && _status == gateway_status::ready) {
            try {
                CThostFtdcUserLogoutField logout_field{};
                ctp_str_copy(logout_field.BrokerID, _config.broker_id);
                ctp_str_copy(logout_field.UserID, _config.investor_id);
                const int result = _td_api->ReqUserLogout(&logout_field, next_request_id());
                if (result != 0) {
                    throw std::runtime_error("CTP logout request was not accepted");
                }
            } catch (const std::exception& error) {
                std::cerr << "[ERROR] ctp_gateway::stop: " << error.what() << std::endl;
                logout_failure = std::current_exception();
            }
        }
    }

    std::cout << "[INFO] ctp_gateway::stop: shutdown complete" << std::endl;
    if (logout_failure) {
        return make_exception_future<>(std::move(logout_failure));
    }
    return make_ready_future<>();
}

void ctp_gateway::post_market_callback(const ctp_market_callback& event) noexcept {
    if (!_callback_state) {
        _callback_bridge_missing.store(true, std::memory_order_release);
        return;
    }
    _callback_state->bridge.post_market(event);
}

void ctp_gateway::post_trader_callback(const ctp_trader_callback& event) noexcept {
    if (!_callback_state) {
        _callback_bridge_missing.store(true, std::memory_order_release);
        return;
    }
    _callback_state->bridge.post_trader(event);
}

bool ctp_gateway::callback_events_lost() const noexcept {
    return _callback_bridge_missing.load(std::memory_order_acquire)
        || (_callback_state && (_callback_state->bridge.market_event_lost()
                                || _callback_state->bridge.trader_event_lost()));
}

void ctp_gateway::fail_callback_bridge(const char* reason) {
    if (!_callback_failure_reported) {
        std::cerr << "[ERROR] ctp_gateway callback bridge: " << reason << std::endl;
    }
    _callback_failure_reported = true;
    _status = gateway_status::error;
    const auto failure = std::make_exception_ptr(std::runtime_error(reason));
    auto fail_promise = [&failure] (auto& pending) {
        if (pending) {
            pending->set_exception(failure);
            pending.reset();
        }
    };
    fail_promise(_md_connected_promise);
    fail_promise(_td_connected_promise);
    fail_promise(_td_authenticated_promise);
    fail_promise(_md_login_promise);
    fail_promise(_td_login_promise);
    fail_promise(_account_query_promise);
    _account_query_data.reset();
    _account_query_request_id = 0;
    _md_login_request_id = 0;
    _td_login_request_id = 0;
    _td_auth_request_id = 0;
}

void ctp_gateway::drain_ctp_callbacks() {
    if (!_callback_state) {
        if (_callback_bridge_missing.load(std::memory_order_acquire)) {
            fail_callback_bridge("callback arrived without an initialized bridge");
        }
        return;
    }
    if (callback_events_lost()) {
        fail_callback_bridge("callback event lost; reconciliation is required");
        return;
    }
    auto valid_generation = [this] (uint64_t generation) {
        if (generation == _callback_generation) {
            return true;
        }
        if (generation != 0 && generation < _callback_generation) {
            std::cerr << "[WARN] ctp_gateway: ignored callback from an older session" << std::endl;
            return false;
        }
        fail_callback_bridge("invalid callback session generation");
        return false;
    };
    try {
        ctp_trader_callback trader;
        for (size_t count = 0; count < max_drain_per_tick && !_callback_failure_reported
             && _callback_state->bridge.pop_trader(trader); ++count) {
            if (valid_generation(trader.session_generation)) {
                if (!_td_spi) {
                    fail_callback_bridge("trader callback without an active SPI");
                    break;
                }
                _td_spi->deliver(trader);
            }
        }
        ctp_market_callback market;
        for (size_t count = 0; count < max_drain_per_tick && !_callback_failure_reported
             && _callback_state->bridge.pop_market(market); ++count) {
            if (valid_generation(market.session_generation)) {
                if (!_md_spi) {
                    fail_callback_bridge("market callback without an active SPI");
                    break;
                }
                _md_spi->deliver(market);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] ctp_gateway callback delivery failed: " << error.what() << std::endl;
        fail_callback_bridge("callback delivery failed; reconciliation is required");
    }
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
    if (!_md_api || _callback_failure_reported) {
        return make_exception_future<>(std::runtime_error("MdApi not initialized"));
    }

    CThostFtdcReqUserLoginField login_field{};
    fill_login_field(&login_field, _config.broker_id, _config.investor_id,
                     _config.password, _config.user_product_info);
    _md_login_request_id = next_request_id();
    _md_login_promise = promise<>();
    auto login_result = _md_login_promise->get_future();

    std::cout << "[INFO] ctp_gateway: sending MD login request..." << std::endl;
    int ret = _md_api->ReqUserLogin(&login_field, _md_login_request_id);
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway: ReqUserLogin (MD) failed, ret=" << ret << std::endl;
        if (_md_login_promise) {
            _md_login_promise->set_exception(std::runtime_error("MD login failed"));
            _md_login_promise.reset();
        }
        _md_login_request_id = 0;
        return std::move(login_result);
    }

    return std::move(login_result).then([this] {
        std::cout << "[INFO] ctp_gateway: MD login success" << std::endl;
        return make_ready_future<>();
    });
}

future<> ctp_gateway::login_td() {
    if (!_td_api || _callback_failure_reported) {
        return make_exception_future<>(std::runtime_error("TraderApi not initialized"));
    }

    // Step 1: Authenticate
    CThostFtdcReqAuthenticateField auth_field{};
    ctp_str_copy(auth_field.BrokerID, _config.broker_id);
    ctp_str_copy(auth_field.UserID, _config.investor_id);
    ctp_str_copy(auth_field.AuthCode, _config.auth_code);
    ctp_str_copy(auth_field.AppID, _config.app_id);
    ctp_str_copy(auth_field.UserProductInfo, _config.user_product_info);
    _td_auth_request_id = next_request_id();
    _td_authenticated_promise = promise<>();
    auto authentication_result = _td_authenticated_promise->get_future();

    _status = gateway_status::authenticating;
    std::cout << "[INFO] ctp_gateway: sending authenticate request..." << std::endl;

    int ret = _td_api->ReqAuthenticate(&auth_field, _td_auth_request_id);
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway: ReqAuthenticate failed, ret=" << ret << std::endl;
        if (_td_authenticated_promise) {
            _td_authenticated_promise->set_exception(std::runtime_error("TD authentication failed"));
            _td_authenticated_promise.reset();
        }
        _td_auth_request_id = 0;
        return std::move(authentication_result);
    }

    return std::move(authentication_result).then([this] {
        if (!_td_api || _callback_failure_reported) {
            return make_exception_future<>(std::runtime_error("TraderApi not initialized"));
        }
        std::cout << "[INFO] ctp_gateway: authentication success, sending login..." << std::endl;

        // Step 2: Login
        CThostFtdcReqUserLoginField login_field{};
        fill_login_field(&login_field, _config.broker_id, _config.investor_id,
                         _config.password, _config.user_product_info);
        _td_login_request_id = next_request_id();
        _td_login_promise = promise<>();
        auto login_result = _td_login_promise->get_future();

        _status = gateway_status::logging_in;
        int login_ret = _td_api->ReqUserLogin(&login_field, _td_login_request_id);
        if (login_ret != 0) {
            std::cerr << "[ERROR] ctp_gateway: ReqUserLogin (TD) failed, ret=" << login_ret << std::endl;
            if (_td_login_promise) {
                _td_login_promise->set_exception(std::runtime_error("TD login failed"));
                _td_login_promise.reset();
            }
            _td_login_request_id = 0;
            return std::move(login_result);
        }

        return std::move(login_result).then([this] {
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
    // Phase 6: measure local submission, not exchange acknowledgement latency.
    // Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
    const auto started = performance_metrics::clock::now();
    auto finish = [started](domain::order_status status) {
        auto& metrics = performance_metrics::instance();
        metrics.record_order_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
            performance_metrics::clock::now() - started).count());
        if (status == domain::order_status::submitted) metrics.record_success("order");
        else metrics.record_error("order");
        return make_ready_future<domain::order_status>(status);
    };
    if (_status != gateway_status::ready || _callback_failure_reported
        || !_callback_state || callback_events_lost()) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: gateway not ready, status="
                   << static_cast<int>(_status) << std::endl;
        return finish(domain::order_status::rejected);
    }

    const bool valid_direction = request.direction == domain::side::buy || request.direction == domain::side::sell;
    const bool valid_offset = request.offset == domain::offset_flag::open
        || request.offset == domain::offset_flag::close || request.offset == domain::offset_flag::close_today
        || request.offset == domain::offset_flag::close_yesterday;
    const bool valid_type = request.type == domain::order_type::limit || request.type == domain::order_type::market
        || request.type == domain::order_type::fak || request.type == domain::order_type::fok;
    if (!request.is_valid() || !std::isfinite(request.price) || !valid_direction || !valid_offset || !valid_type
        || request.hedge != domain::hedge_flag::speculation) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: invalid order request" << std::endl;
        return finish(domain::order_status::rejected);
    }

    if (!_td_api) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: trader API is unavailable" << std::endl;
        return finish(domain::order_status::rejected);
    }

    // Generate order_ref
    constexpr uint64_t MAX_ORDER_REF = 999999999999ULL;
    if (_order_ref_seq >= MAX_ORDER_REF) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: order reference exhausted" << std::endl;
        return finish(domain::order_status::rejected);
    }
    ++_order_ref_seq;
    sstring order_ref_str = fmt::format("{:012d}", _order_ref_seq);

    // Build CTP order field
    CThostFtdcInputOrderField order_field{};
    std::memset(&order_field, 0, sizeof(order_field));

    try {
        ctp_str_copy(order_field.BrokerID, _config.broker_id);
        ctp_str_copy(order_field.InvestorID, _config.investor_id);
        ctp_str_copy(order_field.InstrumentID, request.instrument_id);
        ctp_str_copy(order_field.OrderRef, order_ref_str);
        ctp_str_copy(order_field.UserID, _config.investor_id);
    } catch (const std::invalid_argument& error) {
        std::cerr << "[ERROR] ctp_gateway::submit_order: " << error.what() << std::endl;
        return finish(domain::order_status::rejected);
    }

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
    order_field.RequestID = next_request_id();

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
        return finish(domain::order_status::rejected);
    }

    // Note: actual order status will be returned via OnRtnOrder/OnRspOrderInsert callbacks
    return finish(domain::order_status::submitted);
}

future<domain::order_status> ctp_gateway::cancel_order(const sstring& order_sys_id) {
    if (_status != gateway_status::ready || _callback_failure_reported
        || !_callback_state || callback_events_lost()) {
        std::cerr << "[ERROR] ctp_gateway::cancel_order: gateway not ready" << std::endl;
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
    ctp_str_copy(action_field.ExchangeID, po->exchange_id);
    action_field.ActionFlag = THOST_FTDC_AF_Delete;
    ctp_str_copy(action_field.InstrumentID, po->request.instrument_id);
    action_field.RequestID = next_request_id();

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
    if (_account_query_promise) {
        return make_exception_future<domain::trading_account>(
            std::runtime_error("Another account query is still in progress"));
    }

    CThostFtdcQryTradingAccountField query_field{};
    std::memset(&query_field, 0, sizeof(query_field));
    ctp_str_copy(query_field.BrokerID, _config.broker_id);
    ctp_str_copy(query_field.InvestorID, _config.investor_id);
    ctp_str_copy(query_field.CurrencyID, "CNY");
    _account_query_request_id = next_request_id();
    _account_query_promise = promise<domain::trading_account>();
    auto query_result = _account_query_promise->get_future();
    _account_query_data.reset();

    std::cout << "[INFO] ctp_gateway::sync_account_from_ctp: querying account..." << std::endl;

    int ret = _td_api->ReqQryTradingAccount(&query_field, _account_query_request_id);
    if (ret != 0) {
        std::cerr << "[ERROR] ctp_gateway::sync_account_from_ctp: ReqQryTradingAccount failed, ret="
                   << ret << std::endl;
        if (_account_query_promise) {
            _account_query_promise->set_exception(std::runtime_error("ReqQryTradingAccount failed"));
            _account_query_promise.reset();
        }
        _account_query_request_id = 0;
        return std::move(query_result);
    }

    return std::move(query_result).then([](domain::trading_account account) {
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
    fail_callback_bridge("market data front disconnected; reconciliation is required");
}

void ctp_gateway::on_td_front_disconnected(int reason) {
    std::cerr << "[ctp_gateway] TD front disconnected, reason=" << reason << std::endl;
    fail_callback_bridge("trader front disconnected; reconciliation is required");
}

void ctp_gateway::on_md_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp,
                                    int request_id, bool is_last) {
    if (!_md_login_promise || request_id != _md_login_request_id || !is_last) {
        fail_callback_bridge("unmatched or incomplete market login response");
        return;
    }

    if (!success || !pRsp || ctp_detail::ctp_string(pRsp->BrokerID) != _config.broker_id
        || ctp_detail::ctp_string(pRsp->UserID) != _config.investor_id
        || ctp_detail::ctp_string(pRsp->TradingDay) != _trading_day) {
        fail_callback_bridge("market login identity or trading day is inconsistent");
        return;
    }
    _md_login_promise->set_value();
    _md_login_promise.reset();
    _md_login_request_id = 0;
}

void ctp_gateway::on_td_login_result(bool success, const CThostFtdcRspUserLoginField* pRsp,
                                    int request_id, bool is_last) {
    if (!_td_login_promise || request_id != _td_login_request_id || !is_last) {
        fail_callback_bridge("unmatched or incomplete trader login response");
        return;
    }
    if (!success || !pRsp || ctp_detail::ctp_string(pRsp->BrokerID) != _config.broker_id
        || ctp_detail::ctp_string(pRsp->UserID) != _config.investor_id) {
        fail_callback_bridge("trader login identity is inconsistent");
        return;
    }
    const auto trading_day = ctp_detail::ctp_string(pRsp->TradingDay);
    const auto max_ref = ctp_detail::ctp_string(pRsp->MaxOrderRef);
    uint64_t parsed_ref = 0;
    const auto parsed = std::from_chars(max_ref.data(), max_ref.data() + max_ref.size(), parsed_ref);
    constexpr uint64_t MAX_ORDER_REF = 999999999999ULL;
    if (!ledger_detail::is_valid_trading_day(trading_day) || pRsp->FrontID < 0 || pRsp->SessionID < 0
        || max_ref.empty() || parsed.ec != std::errc{} || parsed.ptr != max_ref.data() + max_ref.size()
        || parsed_ref >= MAX_ORDER_REF) {
        fail_callback_bridge("trader login contains invalid session or order reference data");
        return;
    }
    _front_id = pRsp->FrontID;
    _session_id = pRsp->SessionID;
    _trading_day = trading_day;
    _order_ref_seq = parsed_ref;
    _td_login_promise->set_value();
    _td_login_promise.reset();
    _td_login_request_id = 0;
}

void ctp_gateway::on_authenticate_result(bool success, int request_id, bool is_last) {
    if (!_td_authenticated_promise || request_id != _td_auth_request_id || !is_last) {
        fail_callback_bridge("unmatched or incomplete authentication response");
        return;
    }
    if (!success) {
        fail_callback_bridge("CTP authentication failed");
        return;
    }
    _td_authenticated_promise->set_value();
    _td_authenticated_promise.reset();
    _td_auth_request_id = 0;
}

void ctp_gateway::on_account_query_result(const CThostFtdcTradingAccountField* pAccount,
                                           bool success, bool is_last, int request_id) {
    if (!_account_query_promise || request_id != _account_query_request_id) {
        fail_callback_bridge("unmatched account query response");
        return;
    }

    if (!success) {
        fail_callback_bridge("account query failed; reconciliation is required");
        return;
    }

    if (pAccount) {
        if (_account_query_data) {
            fail_callback_bridge("account query returned more than one account");
            return;
        }
        const auto broker_id = ctp_detail::ctp_string(pAccount->BrokerID);
        const auto account_id = ctp_detail::ctp_string(pAccount->AccountID);
        const auto currency_id = ctp_detail::ctp_string(pAccount->CurrencyID);
        const auto trading_day = ctp_detail::ctp_string(pAccount->TradingDay);
        if (broker_id != _config.broker_id || !ledger_detail::has_identifier(account_id)
            || currency_id != "CNY" || trading_day != _trading_day
            || !std::isfinite(pAccount->PreBalance) || !std::isfinite(pAccount->Balance)
            || !std::isfinite(pAccount->Available) || !std::isfinite(pAccount->CurrMargin)
            || !std::isfinite(pAccount->FrozenMargin) || !std::isfinite(pAccount->FrozenCommission)
            || !std::isfinite(pAccount->Commission) || !std::isfinite(pAccount->CloseProfit)
            || !std::isfinite(pAccount->PositionProfit)) {
            fail_callback_bridge("account query returned inconsistent identity or amounts");
            return;
        }
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
        account.trading_day = trading_day;

        std::cout << "[INFO] ctp_gateway::on_account_query_result: "
                  << "balance=" << account.balance
                  << " available=" << account.available
                  << " margin=" << account.curr_margin
                  << " frozen_margin=" << account.frozen_margin << std::endl;

        _account_query_data = std::move(account);
    }

    if (is_last) {
        if (!_account_query_data) {
            fail_callback_bridge("account query returned no account");
            return;
        }
        _account_query_promise->set_value(std::move(*_account_query_data));
        _account_query_promise.reset();
        _account_query_data.reset();
        _account_query_request_id = 0;
    }
}

// ==================== SPI Callback Entry Points ====================

void ctp_gateway::on_market_data(const domain::market_data& data) {
    md_slot slot;
    slot.capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        seastar::steady_clock_type::now().time_since_epoch()).count();
    slot.data = data;

    if (!_md_ring.push(slot)) {
        performance_metrics::instance().record_md_dropped();
        ++_dropped_md_events;
        std::cerr << "[WARN] ctp_gateway: md_ring full, dropped market data for "
                  << data.instrument_id << std::endl;
        fail_callback_bridge("market data event lost; reconciliation is required");
        return;
    }
    performance_metrics::instance().record_queue_depth("market_data", _md_ring.approx_size());
}

void ctp_gateway::on_trade_report(const domain::trade_report& report) {
    const auto* pending = _pending_orders.get_by_ref(report.order_ref);
    if (!pending || report.trade_id.empty() || report.instrument_id != pending->request.instrument_id
        || report.broker_id != _config.broker_id || report.investor_id != _config.investor_id
        || report.trading_day != _trading_day || report.direction != pending->request.direction
        || report.offset != pending->request.offset || !report.hedge || *report.hedge != pending->request.hedge
        || report.volume <= 0 || !std::isfinite(report.price) || report.price <= 0
        || pending->exchange_id.empty() || report.exchange_id != pending->exchange_id
        || (!pending->order_sys_id.empty() && report.order_sys_id != pending->order_sys_id)) {
        throw std::runtime_error("CTP trade does not match a known account and pending order");
    }
    trader_slot slot;
    slot.capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        seastar::steady_clock_type::now().time_since_epoch()).count();
    slot.type = domain::event_type::trade;
    slot.trade_data = report;
    slot.trade_data.strategy_id = pending->request.strategy_id;
    slot.order_data = domain::order{};

    if (!_trader_ring.push(slot)) {
        performance_metrics::instance().record_trade_dropped();
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped trade event for "
                  << report.instrument_id << " (CRITICAL: violates zero-drop guarantee)" << std::endl;
        fail_callback_bridge("trade event lost; reconciliation is required");
        return;
    }
    performance_metrics::instance().record_queue_depth("trade", _trader_ring.approx_size());
}

void ctp_gateway::on_order_return(const domain::order& order) {
    const auto* pending = _pending_orders.get_by_ref(order.order_ref);
    const bool is_insert_rejection = order.submit_status == domain::order_submit_status::insert_rejected;
    if (!pending || order.instrument_id != pending->request.instrument_id
        || (!order.broker_id.empty() && order.broker_id != _config.broker_id)
        || (!order.investor_id.empty() && order.investor_id != _config.investor_id)
        || (!order.trading_day.empty() && order.trading_day != _trading_day)
        || (!pending->exchange_id.empty() && !order.exchange_id.empty()
            && order.exchange_id != pending->exchange_id)
        || (order.front_id != 0 && order.front_id != pending->front_id)
        || (order.session_id != 0 && order.session_id != pending->session_id)
        || order.volume_traded < 0 || order.volume_traded > pending->request.volume
        || (!is_insert_rejection && (order.volume_original != pending->request.volume
            || order.volume_remaining != pending->request.volume - order.volume_traded))
        || (order.hedge && *order.hedge != pending->request.hedge)
        || (!is_insert_rejection
            && (order.direction != pending->request.direction || order.offset != pending->request.offset))) {
        throw std::runtime_error("CTP order does not match a known account and pending order");
    }
    trader_slot slot;
    slot.capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        seastar::steady_clock_type::now().time_since_epoch()).count();
    slot.type = domain::event_type::order;
    slot.order_data = order;
    slot.order_data.strategy_id = pending->request.strategy_id;
    slot.order_data.broker_id = pending->broker_id;
    slot.order_data.investor_id = pending->investor_id;
    slot.order_data.front_id = pending->front_id;
    slot.order_data.session_id = pending->session_id;
    slot.order_data.trading_day = _trading_day;
    if (slot.order_data.order_sys_id.empty()) {
        slot.order_data.order_sys_id = pending->order_sys_id;
    }
    if (slot.order_data.exchange_id.empty()) {
        slot.order_data.exchange_id = pending->exchange_id;
    }
    slot.order_data.type = pending->request.type;
    slot.order_data.direction = pending->request.direction;
    slot.order_data.offset = pending->request.offset;
    slot.order_data.hedge = pending->request.hedge;
    slot.order_data.limit_price = pending->request.price;
    slot.order_data.volume_original = pending->request.volume;
    slot.order_data.volume_remaining = pending->request.volume - order.volume_traded;
    slot.trade_data = domain::trade_report{};

    _pending_orders.update_traded_volume(order.order_ref, order.volume_traded);
    if (!order.order_sys_id.empty()) {
        _pending_orders.set_order_sys_id(order.order_ref, order.order_sys_id, order.exchange_id,
                                        order.front_id, order.session_id);
    }
    _pending_orders.update_status(order.order_ref, order.status);

    if (!_trader_ring.push(slot)) {
        performance_metrics::instance().record_order_dropped();
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped order event for "
                  << order.order_sys_id << std::endl;
        fail_callback_bridge("order event lost; reconciliation is required");
        return;
    }
    performance_metrics::instance().record_queue_depth("order", _trader_ring.approx_size());
}

void ctp_gateway::on_cancel_return(const domain::order& order) {
    const auto* pending = !order.order_ref.empty() ? _pending_orders.get_by_ref(order.order_ref)
                                                  : _pending_orders.get_by_sys_id(order.order_sys_id);
    if (!pending || (!order.order_sys_id.empty() && order.order_sys_id != pending->order_sys_id)
        || (!order.exchange_id.empty() && order.exchange_id != pending->exchange_id)
        || (!order.instrument_id.empty() && order.instrument_id != pending->request.instrument_id)
        || (!order.broker_id.empty() && order.broker_id != pending->broker_id)
        || (!order.investor_id.empty() && order.investor_id != pending->investor_id)
        || (order.front_id != 0 && order.front_id != pending->front_id)
        || (order.session_id != 0 && order.session_id != pending->session_id)) {
        throw std::runtime_error("CTP cancel response does not match a known pending order");
    }
    trader_slot slot;
    slot.capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        seastar::steady_clock_type::now().time_since_epoch()).count();
    slot.type = domain::event_type::cancel;
    slot.order_data = order;
    slot.order_data.strategy_id = pending->request.strategy_id;
    slot.order_data.order_ref = pending->order_ref;
    slot.order_data.order_sys_id = pending->order_sys_id;
    slot.order_data.exchange_id = pending->exchange_id;
    slot.order_data.instrument_id = pending->request.instrument_id;
    slot.order_data.broker_id = pending->broker_id;
    slot.order_data.investor_id = pending->investor_id;
    slot.order_data.front_id = pending->front_id;
    slot.order_data.session_id = pending->session_id;
    slot.order_data.direction = pending->request.direction;
    slot.order_data.offset = pending->request.offset;
    slot.order_data.type = pending->request.type;
    slot.order_data.limit_price = pending->request.price;
    slot.order_data.status = pending->status;
    slot.order_data.volume_original = pending->request.volume;
    slot.order_data.volume_traded = pending->traded_volume;
    slot.order_data.volume_remaining = pending->request.volume - pending->traded_volume;
    slot.order_data.hedge = pending->request.hedge;
    slot.trade_data = domain::trade_report{};

    if (!_trader_ring.push(slot)) {
        performance_metrics::instance().record_order_dropped();
        ++_dropped_trader_events;
        std::cerr << "[ERROR] ctp_gateway: trader_ring full, dropped cancel event for "
                  << order.order_sys_id << std::endl;
        fail_callback_bridge("cancel event lost; reconciliation is required");
        return;
    }
    performance_metrics::instance().record_queue_depth("order", _trader_ring.approx_size());
}

void ctp_gateway::on_error(int error_id, const sstring& error_msg) {
    std::cerr << "[ERROR] ctp_gateway: CTP error " << error_id << ": " << error_msg << std::endl;
    fail_callback_bridge("CTP reported an error; reconciliation is required");
}

// ==================== Drain Loops ====================

void ctp_gateway::drain_md_ring() {
    if (_callback_failure_reported) {
        return;
    }
    if (callback_events_lost()) {
        fail_callback_bridge("callback event lost; reconciliation is required");
        return;
    }
    md_slot slot;
    size_t drained = 0;
    const auto start_time = seastar::steady_clock_type::now();

    try {
        while (drained < max_drain_per_tick) {
            const auto elapsed = seastar::steady_clock_type::now() - start_time;
            if (elapsed.count() > static_cast<long>(max_drain_us * 1000)) {
                break;
            }

            if (!_md_ring.pop(slot)) {
                break;
            }

            auto& metrics = performance_metrics::instance();
            metrics.record_md_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
                seastar::steady_clock_type::now().time_since_epoch()
                    - std::chrono::nanoseconds(slot.capture_ns)).count());
            if (!_market_handler) {
                fail_callback_bridge("market data handler is not installed");
                return;
            }
            _market_handler(slot.data);
            metrics.record_success("market_data");

            ++drained;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] ctp_gateway market delivery failed: " << error.what() << std::endl;
        fail_callback_bridge("market data delivery failed; reconciliation is required");
        return;
    }
    auto& metrics = performance_metrics::instance();
    metrics.record_queue_depth("market_data", _md_ring.approx_size());
    metrics.record_drain_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
        seastar::steady_clock_type::now() - start_time).count());
    metrics.record_success("drain");
}

void ctp_gateway::drain_trader_ring() {
    if (_callback_failure_reported) {
        return;
    }
    if (callback_events_lost()) {
        fail_callback_bridge("callback event lost; reconciliation is required");
        return;
    }
    const auto started = performance_metrics::clock::now();
    auto& metrics = performance_metrics::instance();
    trader_slot slot;

    try {
        for (size_t count = 0; count < max_drain_per_tick && _trader_ring.pop(slot); ++count) {
            if (slot.type == domain::event_type::trade) {
                metrics.record_trade_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
                    seastar::steady_clock_type::now().time_since_epoch()
                        - std::chrono::nanoseconds(slot.capture_ns)).count());
                if (!_trade_handler) {
                    metrics.record_error("trade");
                    fail_callback_bridge("trade handler is not installed");
                    return;
                }
                _trade_handler(slot.trade_data);
                metrics.record_success("trade");
            } else if (slot.type == domain::event_type::order || slot.type == domain::event_type::cancel) {
                if (!_order_handler) {
                    fail_callback_bridge("order handler is not installed");
                    return;
                }
                _order_handler(slot.order_data);
                metrics.record_success("order");
            } else {
                fail_callback_bridge("unknown trader event type");
                return;
            }
        }
    } catch (const std::exception& error) {
        metrics.record_error(slot.type == domain::event_type::trade ? "trade" : "order");
        std::cerr << "[ERROR] ctp_gateway trader delivery failed: " << error.what() << std::endl;
        fail_callback_bridge("trader delivery failed; reconciliation is required");
        return;
    }
    // Order/trade share the same queue; depths must not be added together.
    metrics.record_queue_depth("order", _trader_ring.approx_size());
    metrics.record_queue_depth("trade", _trader_ring.approx_size());
    metrics.record_drain_latency_us(std::chrono::duration_cast<std::chrono::microseconds>(
        performance_metrics::clock::now() - started).count());
    metrics.record_success("drain");
}

} // namespace seastar::xtrader
