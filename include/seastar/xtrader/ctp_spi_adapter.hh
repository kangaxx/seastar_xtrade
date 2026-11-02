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

#include <seastar/xtrader/domain_types.hh>
#include <ThostFtdcMdApi.h>
#include <ThostFtdcTraderApi.h>

#include <string>

namespace seastar::xtrader {

class ctp_gateway;

/**
 * MdSpiAdapter wraps CThostFtdcMdSpi to integrate with Seastar.
 *
 * All CTP callbacks are invoked from CTP's internal thread.
 * This adapter's only responsibility is to serialize data into
 * the SPSC ring buffer and return immediately (zero blocking).
 *
 * Key callbacks:
 * - OnRtnDepthMarketData: Market data push
 * - OnFrontConnected: Notify gateway for connection state
 * - OnRspUserLogin: Notify gateway for login state
 */
class MdSpiAdapter : public CThostFtdcMdSpi {
public:
    explicit MdSpiAdapter(ctp_gateway* gateway) noexcept;
    ~MdSpiAdapter() override = default;

    MdSpiAdapter(const MdSpiAdapter&) = delete;
    MdSpiAdapter& operator=(const MdSpiAdapter&) = delete;
    MdSpiAdapter(MdSpiAdapter&&) = delete;
    MdSpiAdapter& operator=(MdSpiAdapter&&) = delete;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID,
                        bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    ctp_gateway* _gateway;
};

/**
 * TraderSpiAdapter wraps CThostFtdcTraderSpi to integrate with Seastar.
 *
 * All CTP callbacks are invoked from CTP's internal thread.
 * This adapter's only responsibility is to serialize data into
 * the SPSC ring buffer and return immediately (zero blocking).
 *
 * Key callbacks that produce trader events:
 * - OnRtnOrder: Order status changes (accepted, partial fill, filled, canceled, rejected)
 * - OnRtnTrade: Trade execution notifications
 * - OnRspOrderInsert: Order submission rejection
 * - OnRspOrderAction: Order cancellation rejection
 * - OnRspQryTradingAccount: Account query result (for initialization)
 */
class TraderSpiAdapter : public CThostFtdcTraderSpi {
public:
    explicit TraderSpiAdapter(ctp_gateway* gateway) noexcept;
    ~TraderSpiAdapter() override = default;

    TraderSpiAdapter(const TraderSpiAdapter&) = delete;
    TraderSpiAdapter& operator=(const TraderSpiAdapter&) = delete;
    TraderSpiAdapter(TraderSpiAdapter&&) = delete;
    TraderSpiAdapter& operator=(TraderSpiAdapter&&) = delete;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID,
                           bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID,
                        bool bIsLast) override;
    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                         CThostFtdcRspInfoField* pRspInfo,
                         int nRequestID,
                         bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID,
                          bool bIsLast) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID,
                          bool bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                 CThostFtdcRspInfoField* pRspInfo,
                                 int nRequestID,
                                 bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID,
                                  bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    ctp_gateway* _gateway;
};

} // namespace seastar::xtrader
