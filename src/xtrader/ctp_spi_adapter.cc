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
#include <seastar/xtrader/ctp_spi_adapter.hh>
#include <seastar/xtrader/ctp_gateway.hh>

#include "ctp_field_utils.hh"

#include <iostream>
#include <optional>
#include <stdexcept>

namespace seastar::xtrader {

namespace {

using ctp_detail::ctp_string;

std::optional<domain::side> convert_direction(char c) {
    switch (c) {
        case THOST_FTDC_D_Buy: return domain::side::buy;
        case THOST_FTDC_D_Sell: return domain::side::sell;
        default: return std::nullopt;
    }
}

std::optional<domain::offset_flag> convert_offset_flag(char c) {
    switch (c) {
        case THOST_FTDC_OF_Open: return domain::offset_flag::open;
        case THOST_FTDC_OF_Close: return domain::offset_flag::close;
        case THOST_FTDC_OF_CloseToday: return domain::offset_flag::close_today;
        case THOST_FTDC_OF_CloseYesterday: return domain::offset_flag::close_yesterday;
        default: return std::nullopt;
    }
}

std::optional<domain::order_submit_status> convert_submit_status(char c) {
    switch (c) {
        case THOST_FTDC_OSS_InsertSubmitted: return domain::order_submit_status::insert_submitted;
        case THOST_FTDC_OSS_CancelSubmitted: return domain::order_submit_status::cancel_submitted;
        case THOST_FTDC_OSS_ModifySubmitted: return domain::order_submit_status::modify_submitted;
        case THOST_FTDC_OSS_Accepted: return domain::order_submit_status::accepted;
        case THOST_FTDC_OSS_InsertRejected: return domain::order_submit_status::insert_rejected;
        case THOST_FTDC_OSS_CancelRejected: return domain::order_submit_status::cancel_rejected;
        case THOST_FTDC_OSS_ModifyRejected: return domain::order_submit_status::modify_rejected;
        default: return std::nullopt;
    }
}

} // anonymous namespace

// ==================== MdSpiAdapter Implementation ====================

MdSpiAdapter::MdSpiAdapter(ctp_gateway* gateway, uint64_t session_generation) noexcept
    : _gateway(gateway), _session_generation(session_generation)
{}

void MdSpiAdapter::deliver_front_connected() {
    std::cout << "[MdSpi] Front connected" << std::endl;
    if (_gateway) {
        _gateway->on_md_front_connected();
    }
}

void MdSpiAdapter::deliver_front_disconnected(int nReason) {
    std::cerr << "[MdSpi] Front disconnected, reason=" << nReason << std::endl;
    if (_gateway) {
        _gateway->on_md_front_disconnected(nReason);
    }
}

void MdSpiAdapter::deliver_login(CThostFtdcRspUserLoginField* pRspUserLogin,
                                   CThostFtdcRspInfoField* pRspInfo,
                                   int nRequestID,
                                   bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[MdSpi] Login failed, error_id=" << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? ctp_string(pRspInfo->ErrorMsg) : "unknown") << std::endl;
    } else if (pRspUserLogin) {
        std::cout << "[MdSpi] Login success, trading_day=" << ctp_string(pRspUserLogin->TradingDay) << std::endl;
    }
    if (_gateway) {
        _gateway->on_md_login_result(success, pRspUserLogin, nRequestID, bIsLast);
    }
}

void MdSpiAdapter::deliver_market_data(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    if (!pDepthMarketData) {
        if (_gateway) {
            _gateway->on_error(-1, "CTP market callback has no payload");
        }
        return;
    }

    if (_gateway) {
        domain::market_data data;
        data.instrument_id = ctp_string(pDepthMarketData->InstrumentID);
        data.last_price = pDepthMarketData->LastPrice;
        data.open_price = pDepthMarketData->OpenPrice;
        data.highest_price = pDepthMarketData->HighestPrice;
        data.lowest_price = pDepthMarketData->LowestPrice;
        data.volume = pDepthMarketData->Volume;
        data.turnover = pDepthMarketData->Turnover;
        data.open_interest = pDepthMarketData->OpenInterest;
        data.upper_limit_price = pDepthMarketData->UpperLimitPrice;
        data.lower_limit_price = pDepthMarketData->LowerLimitPrice;
        data.bid_price[0] = pDepthMarketData->BidPrice1;
        data.bid_volume[0] = pDepthMarketData->BidVolume1;
        data.ask_price[0] = pDepthMarketData->AskPrice1;
        data.ask_volume[0] = pDepthMarketData->AskVolume1;
        data.trading_day = ctp_string(pDepthMarketData->TradingDay);
        data.update_time = ctp_string(pDepthMarketData->UpdateTime);
        data.update_millisec = pDepthMarketData->UpdateMillisec;

        _gateway->on_market_data(data);
    }
}

void MdSpiAdapter::deliver_error(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo) {
        std::cerr << "[MdSpi] RspError, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << ctp_string(pRspInfo->ErrorMsg) << std::endl;
    }
    if (_gateway) {
        _gateway->on_error(pRspInfo ? pRspInfo->ErrorID : -1,
                           pRspInfo ? ctp_string(pRspInfo->ErrorMsg) : "unknown");
    }
}

// ==================== TraderSpiAdapter Implementation ====================

TraderSpiAdapter::TraderSpiAdapter(ctp_gateway* gateway, uint64_t session_generation) noexcept
    : _gateway(gateway), _session_generation(session_generation)
{}

void TraderSpiAdapter::deliver_front_connected() {
    std::cout << "[TraderSpi] Front connected" << std::endl;
    if (_gateway) {
        _gateway->on_td_front_connected();
    }
}

void TraderSpiAdapter::deliver_front_disconnected(int nReason) {
    std::cerr << "[TraderSpi] Front disconnected, reason=" << nReason << std::endl;
    if (_gateway) {
        _gateway->on_td_front_disconnected(nReason);
    }
}

void TraderSpiAdapter::deliver_authenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                                          CThostFtdcRspInfoField* pRspInfo,
                                          int nRequestID,
                                          bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Authenticate failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? ctp_string(pRspInfo->ErrorMsg) : "") << std::endl;
    } else {
        std::cout << "[TraderSpi] Authenticate success" << std::endl;
    }
    if (_gateway) {
        _gateway->on_authenticate_result(success, nRequestID, bIsLast);
    }
}

void TraderSpiAdapter::deliver_login(CThostFtdcRspUserLoginField* pRspUserLogin,
                                       CThostFtdcRspInfoField* pRspInfo,
                                       int nRequestID,
                                       bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Login failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? ctp_string(pRspInfo->ErrorMsg) : "") << std::endl;
    } else if (pRspUserLogin) {
        std::cout << "[TraderSpi] Login success, trading_day=" << ctp_string(pRspUserLogin->TradingDay)
                   << ", front_id=" << pRspUserLogin->FrontID
                   << ", session_id=" << pRspUserLogin->SessionID << std::endl;
    }
    if (_gateway) {
        _gateway->on_td_login_result(success, pRspUserLogin, nRequestID, bIsLast);
    }
}

void TraderSpiAdapter::deliver_logout(CThostFtdcUserLogoutField* pUserLogout,
                                       CThostFtdcRspInfoField* pRspInfo,
                                       int nRequestID,
                                       bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Logout failed, error_id=" << pRspInfo->ErrorID << std::endl;
        return;
    }
    std::cout << "[TraderSpi] Logout success" << std::endl;
}

void TraderSpiAdapter::deliver_order_insert(CThostFtdcInputOrderField* pInputOrder,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID,
                                        bool bIsLast) {
    if (!pRspInfo || pRspInfo->ErrorID == 0) {
        return;
    }
    std::cerr << "[TraderSpi] Order insert rejected, error_id=" << pRspInfo->ErrorID
              << ", msg=" << ctp_string(pRspInfo->ErrorMsg) << std::endl;
    if (_gateway && !pInputOrder) {
        _gateway->on_error(pRspInfo->ErrorID, "CTP order rejection has no request identity");
    } else if (pInputOrder && _gateway) {
        domain::order order;
        order.instrument_id = ctp_string(pInputOrder->InstrumentID);
        order.order_ref = ctp_string(pInputOrder->OrderRef);
        order.exchange_id = ctp_string(pInputOrder->ExchangeID);
        order.broker_id = ctp_string(pInputOrder->BrokerID);
        order.investor_id = ctp_string(pInputOrder->InvestorID);
        order.submit_status = domain::order_submit_status::insert_rejected;
        order.status = domain::order_status::rejected;
        order.error_id = pRspInfo->ErrorID;
        order.error_msg = ctp_string(pRspInfo->ErrorMsg);

        _gateway->on_order_return(order);
    }
}

void TraderSpiAdapter::deliver_order(CThostFtdcOrderField* pOrder) {
    if (!pOrder || !_gateway) {
        if (_gateway) {
            _gateway->on_error(-1, "CTP order callback has no payload");
        }
        return;
    }
    const auto direction = convert_direction(pOrder->Direction);
    const auto offset = convert_offset_flag(pOrder->CombOffsetFlag[0]);
    const auto submit_status = convert_submit_status(pOrder->OrderSubmitStatus);
    if (!direction || !offset || !submit_status || pOrder->CombHedgeFlag[0] != THOST_FTDC_HF_Speculation) {
        _gateway->on_error(-1, "CTP order has unsupported direction, offset, hedge or submit status");
        return;
    }

    domain::order order;
    order.instrument_id = ctp_string(pOrder->InstrumentID);
    order.order_ref = ctp_string(pOrder->OrderRef);
    order.order_sys_id = ctp_string(pOrder->OrderSysID);
    order.broker_id = ctp_string(pOrder->BrokerID);
    order.investor_id = ctp_string(pOrder->InvestorID);
    order.exchange_id = ctp_string(pOrder->ExchangeID);
    order.front_id = pOrder->FrontID;
    order.session_id = pOrder->SessionID;
    order.submit_status = *submit_status;

    switch (pOrder->OrderStatus) {
        case THOST_FTDC_OST_AllTraded:
            order.status = domain::order_status::filled;
            break;
        case THOST_FTDC_OST_PartTradedQueueing:
            order.status = domain::order_status::partially_filled;
            break;
        case THOST_FTDC_OST_PartTradedNotQueueing:
        case THOST_FTDC_OST_NoTradeNotQueueing:
            order.status = domain::order_status::canceled;
            break;
        case THOST_FTDC_OST_NoTradeQueueing:
            order.status = domain::order_status::accepted;
            break;
        case THOST_FTDC_OST_Canceled:
            order.status = domain::order_status::canceled;
            break;
        case THOST_FTDC_OST_Unknown:
            order.status = domain::order_status::unknown;
            break;
        default:
            _gateway->on_error(-1, "CTP order has an unsupported order status");
            return;
    }
    if (order.submit_status == domain::order_submit_status::insert_rejected) {
        order.status = domain::order_status::rejected;
    }

    order.volume_original = pOrder->VolumeTotalOriginal;
    order.volume_traded = pOrder->VolumeTraded;
    order.volume_remaining = pOrder->VolumeTotal;
    order.limit_price = pOrder->LimitPrice;
    order.direction = *direction;
    order.offset = *offset;
    order.hedge = domain::hedge_flag::speculation;
    order.insert_time = ctp_string(pOrder->InsertTime);
    order.cancel_time = ctp_string(pOrder->CancelTime);
    order.trading_day = ctp_string(pOrder->TradingDay);
    order.status_msg = ctp_string(pOrder->StatusMsg);

    _gateway->on_order_return(order);
}

void TraderSpiAdapter::deliver_trade(CThostFtdcTradeField* pTrade) {
    if (!pTrade || !_gateway) {
        if (_gateway) {
            _gateway->on_error(-1, "CTP trade callback has no payload");
        }
        return;
    }
    const auto direction = convert_direction(pTrade->Direction);
    const auto offset = convert_offset_flag(pTrade->OffsetFlag);
    if (!direction || !offset || pTrade->HedgeFlag != THOST_FTDC_HF_Speculation) {
        _gateway->on_error(-1, "CTP trade has an unsupported direction, offset or hedge");
        return;
    }

    domain::trade_report report;
    report.trade_id = ctp_string(pTrade->TradeID);
    report.order_sys_id = ctp_string(pTrade->OrderSysID);
    report.order_ref = ctp_string(pTrade->OrderRef);
    report.instrument_id = ctp_string(pTrade->InstrumentID);
    report.exchange_id = ctp_string(pTrade->ExchangeID);
    report.broker_id = ctp_string(pTrade->BrokerID);
    report.investor_id = ctp_string(pTrade->InvestorID);
    report.direction = *direction;
    report.offset = *offset;
    report.price = pTrade->Price;
    report.volume = pTrade->Volume;
    report.trade_time = ctp_string(pTrade->TradeTime);
    report.trading_day = ctp_string(pTrade->TradingDay);
    report.fill_origin = domain::trade_fill_origin::ctp_report;
    report.is_approximate = false;
    report.hedge = domain::hedge_flag::speculation;

    _gateway->on_trade_report(report);
}

void TraderSpiAdapter::deliver_order_action(CThostFtdcInputOrderActionField* pInputOrderAction,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID,
                                        bool bIsLast) {
    if (!pRspInfo || pRspInfo->ErrorID == 0) {
        return;
    }
    std::cerr << "[TraderSpi] Order cancel rejected, error_id=" << pRspInfo->ErrorID
              << ", msg=" << ctp_string(pRspInfo->ErrorMsg) << std::endl;
    if (_gateway && !pInputOrderAction) {
        _gateway->on_error(pRspInfo->ErrorID, "CTP cancel rejection has no request identity");
    } else if (pInputOrderAction && _gateway) {
        domain::order order;
        order.order_sys_id = ctp_string(pInputOrderAction->OrderSysID);
        order.order_ref = ctp_string(pInputOrderAction->OrderRef);
        order.instrument_id = ctp_string(pInputOrderAction->InstrumentID);
        order.exchange_id = ctp_string(pInputOrderAction->ExchangeID);
        order.broker_id = ctp_string(pInputOrderAction->BrokerID);
        order.investor_id = ctp_string(pInputOrderAction->InvestorID);
        order.front_id = pInputOrderAction->FrontID;
        order.session_id = pInputOrderAction->SessionID;
        order.submit_status = domain::order_submit_status::cancel_rejected;
        order.status = domain::order_status::unknown;
        order.error_id = pRspInfo->ErrorID;
        order.error_msg = ctp_string(pRspInfo->ErrorMsg);

        _gateway->on_cancel_return(order);
    }
}

void TraderSpiAdapter::deliver_account(CThostFtdcTradingAccountField* pTradingAccount,
                                              CThostFtdcRspInfoField* pRspInfo,
                                              int nRequestID,
                                              bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Query trading account failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1) << std::endl;
    }

    if (_gateway) {
        _gateway->on_account_query_result(pTradingAccount, success, bIsLast, nRequestID);
    }
}

void TraderSpiAdapter::deliver_position(CThostFtdcInvestorPositionField* pInvestorPosition,
                                                 CThostFtdcRspInfoField* pRspInfo,
                                                 int nRequestID,
                                                 bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Query position failed, error_id=" << pRspInfo->ErrorID << std::endl;
        return;
    }

    if (pInvestorPosition) {
        std::cout << "[TraderSpi] Position: " << ctp_string(pInvestorPosition->InstrumentID)
                  << ", vol=" << pInvestorPosition->Position
                  << ", td_vol=" << pInvestorPosition->TodayPosition << std::endl;
    }
}

void TraderSpiAdapter::deliver_error(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo) {
        std::cerr << "[TraderSpi] RspError, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << ctp_string(pRspInfo->ErrorMsg) << std::endl;
    }
    if (_gateway) {
        _gateway->on_error(pRspInfo ? pRspInfo->ErrorID : -1,
                            pRspInfo ? ctp_string(pRspInfo->ErrorMsg) : "unknown");
    }
}

} // namespace seastar::xtrader
