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

#include <seastar/xtrader/offline_fund_ledger.hh>

#include <utility>

namespace seastar::xtrader::test {

// B2: synthetic, explicitly valued offline account fixtures.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
inline position_account_scope test_account() {
    return {
        .environment = "offline-a",
        .broker_id = "synthetic-broker",
        .account_id = "synthetic-account",
        .trading_day = "20260102",
    };
}

inline position_instrument test_instrument(const sstring& instrument_id = "rb9999") {
    return {
        .exchange_id = "XSGE",
        .instrument_id = instrument_id,
        .hedge = domain::hedge_flag::speculation,
    };
}

inline offline_fund_order make_order(const position_account_scope& account,
        const position_instrument& instrument, const sstring& order_id, domain::side direction,
        domain::offset_flag offset, int volume, int64_t margin, int64_t fee) {
    position_order position{
        .account = account,
        .order_id = order_id,
        .instrument = instrument,
        .direction = direction,
        .offset = offset,
        .volume = volume,
    };
    if (offset == domain::offset_flag::close) {
        position.close_rule = close_position_rule::today_first;
    }
    return {.position = std::move(position), .margin_per_lot = margin, .commission_per_lot = fee};
}

inline offline_fund_trade make_trade(const offline_fund_order& order, const sstring& trade_id,
        int volume, int64_t margin_change, int64_t commission, int64_t profit) {
    return {
        .position = {
            .account = order.position.account,
            .trade_id = trade_id,
            .order_id = order.position.order_id,
            .instrument = order.position.instrument,
            .direction = order.position.direction,
            .offset = order.position.offset,
            .volume = volume,
            .price = 100.0,
        },
        .commission = commission,
        .margin_change = margin_change,
        .close_profit = profit,
    };
}

} // namespace seastar::xtrader::test
