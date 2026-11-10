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

#include <seastar/xtrader/position_ledger.hh>

#include <algorithm>
#include <array>
#include <cctype>

namespace seastar::xtrader::ledger_detail {

// B2/B3: common identity and enum validation for offline ledger and matching.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
inline bool has_identifier(const sstring& value) {
    return std::any_of(value.begin(), value.end(), [] (unsigned char ch) {
        return !std::isspace(ch);
    });
}

inline bool is_valid_trading_day(const sstring& day) {
    if (day.size() != 8 || !std::all_of(day.begin(), day.end(), [] (unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) {
        return false;
    }
    const int year = (day[0] - '0') * 1000 + (day[1] - '0') * 100
                   + (day[2] - '0') * 10 + day[3] - '0';
    const int month = (day[4] - '0') * 10 + day[5] - '0';
    const int date = (day[6] - '0') * 10 + day[7] - '0';
    if (year == 0 || month < 1 || month > 12 || date < 1) {
        return false;
    }
    constexpr std::array<int, 12> DAYS_PER_MONTH{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int last_day = DAYS_PER_MONTH[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        ++last_day;
    }
    return date <= last_day;
}

inline bool is_valid_account(const position_account_scope& account) {
    return has_identifier(account.environment) && has_identifier(account.broker_id)
        && has_identifier(account.account_id) && is_valid_trading_day(account.trading_day);
}

inline bool is_valid_hedge(const std::optional<domain::hedge_flag>& hedge) {
    return hedge && (*hedge == domain::hedge_flag::speculation || *hedge == domain::hedge_flag::hedge
        || *hedge == domain::hedge_flag::arbitrage);
}

inline bool is_valid_instrument(const position_instrument& instrument) {
    return has_identifier(instrument.exchange_id) && has_identifier(instrument.instrument_id)
        && is_valid_hedge(instrument.hedge);
}

inline bool is_valid_direction(const std::optional<domain::side>& direction) {
    return direction && (*direction == domain::side::buy || *direction == domain::side::sell);
}

inline bool is_valid_offset(const std::optional<domain::offset_flag>& offset) {
    return offset && (*offset == domain::offset_flag::open || *offset == domain::offset_flag::close
        || *offset == domain::offset_flag::close_today || *offset == domain::offset_flag::close_yesterday);
}

inline bool is_valid_close_rule(const std::optional<close_position_rule>& rule) {
    return rule && (*rule == close_position_rule::today_first
        || *rule == close_position_rule::yesterday_first);
}

} // namespace seastar::xtrader::ledger_detail
