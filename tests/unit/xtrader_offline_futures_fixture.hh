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

#include <seastar/xtrader/offline_futures_economics.hh>

#include "xtrader_offline_account_fixture.hh"

namespace seastar::xtrader::test {

// B2: synthetic terms are test inputs, not exchange or broker fee schedules.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
inline offline_futures_terms make_terms(const sstring& instrument_id = "rb9999") {
    const offline_rate zero{0, 1, offline_rounding::down};
    return {
        .instrument = test_instrument(instrument_id),
        .money_unit = "synthetic-unit",
        .tick_value_per_lot = 10,
        .price_tick = 1.0,
        .long_margin_rate = {1, 10, offline_rounding::up},
        .short_margin_rate = {1, 5, offline_rounding::up},
        .open_fee = {2, {1, 1000, offline_rounding::up}},
        .close_fee = {4, zero},
        .close_today_fee = {3, {2, 1000, offline_rounding::up}},
        .close_yesterday_fee = {1, zero},
    };
}

} // namespace seastar::xtrader::test
