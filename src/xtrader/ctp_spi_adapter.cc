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

#include <iostream>

namespace seastar::xtrader {

namespace {

domain::offset_flag convert_offset_flag(char c) {
    switch (c) {
        case THOST_FTDC_OF_Open: return domain::offset_flag::open;
        case THOST_FTDC_OF_Close: return domain::offset_flag::close;
        case THOST_FTDC_OF_CloseToday: return domain::offset_flag::close_today;
        case THOST_FTDC_OF_CloseYesterday: return domain::offset_flag::close_yesterday;
        default: return domain::offset_flag::open;
    }
}

} // anonymous namespace

// ==================== MdSpiAdapter Implementation ====================

MdSpiAdapter::MdSpiAdapter(ctp_gateway* gateway) noexcept
    : _gateway(gateway)
{}

void MdSpiAdapter::OnFrontConnected() {
    std::cout << "[MdSpi] Front connected" << std::endl;
    if (_gateway) {
        _gateway->on_md_front_connected();
    }
}

void MdSpiAdapter::OnFrontDisconnected(int nReason) {
    std::cerr << "[MdSpi] Front disconnected, reason=" << nReason << std::endl;
    if (_gateway) {
        _gateway->on_md_front_disconnected(nReason);
    }
}

void MdSpiAdapter::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                   CThostFtdcRspInfoField* pRspInfo,
                                   int nRequestID,
                                   bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[MdSpi] Login failed, error_id=" << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? pRspInfo->ErrorMsg : "unknown") << std::endl;
    } else if (pRspUserLogin) {
        std::cout << "[MdSpi] Login success, trading_day=" << pRspUserLogin->TradingDay << std::endl;
    }
    if (_gateway) {
        _gateway->on_md_login_result(success, pRspUserLogin);
    }
}

void MdSpiAdapter::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    if (!pDepthMarketData) {
        return;
    }

    if (_gateway) {
        domain::market_data data;
        data.instrument_id = std::string(pDepthMarketData->InstrumentID);
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
        data.trading_day = std::string(pDepthMarketData->TradingDay);
        data.update_time = std::string(pDepthMarketData->UpdateTime);
        data.update_millisec = pDepthMarketData->UpdateMillisec;

        _gateway->on_market_data(data);
    }
}

void MdSpiAdapter::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo) {
        std::cerr << "[MdSpi] RspError, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << pRspInfo->ErrorMsg << std::endl;
    }
    if (_gateway) {
        _gateway->on_error(pRspInfo ? pRspInfo->ErrorID : -1,
                           pRspInfo ? std::string(pRspInfo->ErrorMsg) : "unknown");
    }
}

// ==================== TraderSpiAdapter Implementation ====================

TraderSpiAdapter::TraderSpiAdapter(ctp_gateway* gateway) noexcept
    : _gateway(gateway)
{}

void TraderSpiAdapter::OnFrontConnected() {
    std::cout << "[TraderSpi] Front connected" << std::endl;
    if (_gateway) {
        _gateway->on_td_front_connected();
    }
}

void TraderSpiAdapter::OnFrontDisconnected(int nReason) {
    std::cerr << "[TraderSpi] Front disconnected, reason=" << nReason << std::endl;
    if (_gateway) {
        _gateway->on_td_front_disconnected(nReason);
    }
}

void TraderSpiAdapter::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                                          CThostFtdcRspInfoField* pRspInfo,
                                          int nRequestID,
                                          bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Authenticate failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? pRspInfo->ErrorMsg : "") << std::endl;
    } else {
        std::cout << "[TraderSpi] Authenticate success" << std::endl;
    }
    if (_gateway) {
        _gateway->on_authenticate_result(success);
    }
}

void TraderSpiAdapter::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                       CThostFtdcRspInfoField* pRspInfo,
                                       int nRequestID,
                                       bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Login failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1)
                   << ", msg=" << (pRspInfo ? pRspInfo->ErrorMsg : "") << std::endl;
    } else if (pRspUserLogin) {
        std::cout << "[TraderSpi] Login success, trading_day=" << pRspUserLogin->TradingDay
                   << ", front_id=" << pRspUserLogin->FrontID
                   << ", session_id=" << pRspUserLogin->SessionID << std::endl;
    }
    if (_gateway) {
        _gateway->on_td_login_result(success, pRspUserLogin);
    }
}

void TraderSpiAdapter::OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                                       CThostFtdcRspInfoField* pRspInfo,
                                       int nRequestID,
                                       bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Logout failed, error_id=" << pRspInfo->ErrorID << std::endl;
        return;
    }
    std::cout << "[TraderSpi] Logout success" << std::endl;
}

void TraderSpiAdapter::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID,
                                        bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Order insert rejected, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << pRspInfo->ErrorMsg << std::endl;
    }

    if (pInputOrder && _gateway) {
        domain::order order;
        order.instrument_id = std::string(pInputOrder->InstrumentID);
        order.order_ref = std::string(pInputOrder->OrderRef);
        order.exchange_id = std::string(pInputOrder->ExchangeID);
        order.status = domain::order_status::rejected;
        order.error_msg = pRspInfo ? std::string(pRspInfo->ErrorMsg) : "";

        _gateway->on_order_return(order);
    }
}

void TraderSpiAdapter::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder || !_gateway) {
        return;
    }

    domain::order order;
    order.instrument_id = std::string(pOrder->InstrumentID);
    order.order_ref = std::string(pOrder->OrderRef);
    order.order_sys_id = std::string(pOrder->OrderSysID);
    order.broker_id = std::string(pOrder->BrokerID);
    order.investor_id = std::string(pOrder->InvestorID);
    order.exchange_id = std::string(pOrder->ExchangeID);
    order.front_id = pOrder->FrontID;
    order.session_id = pOrder->SessionID;

    switch (pOrder->OrderStatus) {
        case THOST_FTDC_OST_AllTraded:
            order.status = domain::order_status::filled;
            break;
        case THOST_FTDC_OST_PartTradedQueueing:
            order.status = domain::order_status::partially_filled;
            break;
        case THOST_FTDC_OST_NoTradeQueueing:
            order.status = domain::order_status::accepted;
            break;
        case THOST_FTDC_OST_Canceled:
            order.status = domain::order_status::canceled;
            break;
        case THOST_FTDC_OST_Unknown:
        default:
            order.status = domain::order_status::unknown;
            break;
    }

    order.volume_original = pOrder->VolumeTotalOriginal;
    order.volume_traded = pOrder->VolumeTraded;
    order.limit_price = pOrder->LimitPrice;
    order.direction = (pOrder->Direction == THOST_FTDC_D_Buy) ? domain::side::buy : domain::side::sell;
    order.offset = convert_offset_flag(pOrder->CombOffsetFlag[0]);
    order.insert_time = std::string(pOrder->InsertTime);

    _gateway->on_order_return(order);
}

void TraderSpiAdapter::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade || !_gateway) {
        return;
    }

    domain::trade_report report;
    report.trade_id = std::string(pTrade->TradeID);
    report.order_sys_id = std::string(pTrade->OrderSysID);
    report.order_ref = std::string(pTrade->OrderRef);
    report.instrument_id = std::string(pTrade->InstrumentID);
    report.exchange_id = std::string(pTrade->ExchangeID);
    report.broker_id = std::string(pTrade->BrokerID);
    report.investor_id = std::string(pTrade->InvestorID);
    report.direction = (pTrade->Direction == THOST_FTDC_D_Buy) ? domain::side::buy : domain::side::sell;
    report.offset = convert_offset_flag(pTrade->OffsetFlag);
    report.price = pTrade->Price;
    report.volume = pTrade->Volume;
    report.trade_time = std::string(pTrade->TradeTime);
    report.trading_day = std::string(pTrade->TradingDay);

    _gateway->on_trade_report(report);
}

void TraderSpiAdapter::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID,
                                        bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Order cancel rejected, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << pRspInfo->ErrorMsg << std::endl;
    }

    if (pInputOrderAction && _gateway) {
        domain::order order;
        order.order_sys_id = std::string(pInputOrderAction->OrderSysID);
        order.submit_status = domain::order_submit_status::cancel_rejected;
        order.status = domain::order_status::unknown;
        order.error_msg = pRspInfo ? std::string(pRspInfo->ErrorMsg) : "";

        _gateway->on_cancel_return(order);
    }
}

void TraderSpiAdapter::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                              CThostFtdcRspInfoField* pRspInfo,
                                              int nRequestID,
                                              bool bIsLast) {
    bool success = pRspInfo == nullptr || pRspInfo->ErrorID == 0;
    if (!success) {
        std::cerr << "[TraderSpi] Query trading account failed, error_id="
                   << (pRspInfo ? pRspInfo->ErrorID : -1) << std::endl;
    }

    if (_gateway) {
        _gateway->on_account_query_result(pTradingAccount, success, bIsLast);
    }
}

void TraderSpiAdapter::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                                 CThostFtdcRspInfoField* pRspInfo,
                                                 int nRequestID,
                                                 bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[TraderSpi] Query position failed, error_id=" << pRspInfo->ErrorID << std::endl;
        return;
    }

    if (pInvestorPosition) {
        std::cout << "[TraderSpi] Position: " << pInvestorPosition->InstrumentID
                  << ", vol=" << pInvestorPosition->Volume
                  << ", td_vol=" << pInvestorPosition->TodayVolume << std::endl;
    }
}

void TraderSpiAdapter::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo) {
        std::cerr << "[TraderSpi] RspError, error_id=" << pRspInfo->ErrorID
                   << ", msg=" << pRspInfo->ErrorMsg << std::endl;
    }
    if (_gateway) {
        _gateway->on_error(pRspInfo ? pRspInfo->ErrorID : -1,
                            pRspInfo ? std::string(pRspInfo->ErrorMsg) : "unknown");
    }
}

} // namespace seastar::xtrader
