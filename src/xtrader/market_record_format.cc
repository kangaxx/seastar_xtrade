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
#include <seastar/xtrader/market_record_format.hh>
#include <seastar/json/formatter.hh>
#include <fmt/format.h>

#include "ledger_validation.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace seastar::xtrader {
namespace {

void require_token(const sstring& token) {
    if (token.empty() || token.size() > 64
            || !std::all_of(token.begin(), token.end(), [] (unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            })) {
        throw std::invalid_argument("record key components require 1-64 ASCII letters, digits, '_' or '-'");
    }
}

sstring csv_text(const sstring& text) {
    sstring result = "\"";
    for (const char ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            throw std::invalid_argument("multiline or NUL CSV field is not supported");
        }
        if (ch == '"') {
            result += '"';
        }
        result += ch;
    }
    return result + "\"";
}

void require_number(double number, bool positive) {
    if (!std::isfinite(number) || number == std::numeric_limits<double>::max()
            || (positive ? number <= 0 : number < 0)) {
        throw std::invalid_argument("record contains missing or invalid numeric value");
    }
}

} // namespace

sstring paper_tick_key(const sstring& account_alias, uint64_t run_id,
                       const sstring& exchange, const sstring& instrument) {
    for (const auto* token : {&account_alias, &exchange, &instrument}) {
        require_token(*token);
    }
    if (run_id == 0) {
        throw std::invalid_argument("paper recording requires an explicit unique run ID");
    }
    return sstring(fmt::format("xtrader:v1:paper:{}:{}:{}:{}:ticks",
                               account_alias, run_id, exchange, instrument));
}

sstring market_tick_json(const normalized_market_tick& tick) {
    const auto& input = tick.source;
    const auto& data = input.tick;
    require_token(input.exchange_id);
    require_token(data.instrument_id);
    require_number(data.last_price, true);
    require_number(data.bid_price[0], true);
    require_number(data.ask_price[0], true);
    require_number(data.open_interest, false);
    if (!ledger_detail::is_valid_trading_day(data.trading_day) || input.sequence == 0
            || !ledger_detail::has_identifier(input.time_domain)
            || input.session_begin_ms < 0 || input.timestamp_ms < input.session_begin_ms
            || input.timestamp_ms >= input.session_end_ms || data.volume < 0
            || data.bid_volume[0] < 0 || data.ask_volume[0] < 0
            || data.bid_price[0] > data.ask_price[0]
            || (tick.incremental_volume && (*tick.incremental_volume < 0
                                            || *tick.incremental_volume > data.volume))) {
        throw std::invalid_argument("invalid normalized tick recording");
    }
    return sstring(fmt::format(
        "{{\"schema_version\":1,\"exchange\":{},\"instrument\":{},\"trading_day\":{},"
        "\"time_domain\":{},\"sequence\":{},\"timestamp_ms\":{},\"session_begin_ms\":{},\"session_end_ms\":{},"
        "\"last_price\":{},\"cumulative_volume\":{},\"incremental_volume\":{},"
        "\"bid_price\":{},\"bid_volume\":{},\"ask_price\":{},\"ask_volume\":{},"
        "\"open_interest\":{},\"has_sequence_gap\":{}}}",
        json::formatter::to_json(input.exchange_id), json::formatter::to_json(data.instrument_id),
        json::formatter::to_json(data.trading_day), json::formatter::to_json(input.time_domain),
        input.sequence, input.timestamp_ms, input.session_begin_ms, input.session_end_ms,
        data.last_price, data.volume,
        tick.incremental_volume ? fmt::format("{}", *tick.incremental_volume) : "null",
        data.bid_price[0], data.bid_volume[0], data.ask_price[0], data.ask_volume[0],
        data.open_interest, tick.has_sequence_gap));
}

sstring minute_bar_csv_header() {
    return "schema_version,exchange,instrument,trading_day,time_domain,begin_ms,end_ms,"
           "open,high,low,close,observed_volume,open_interest,is_complete\n";
}

sstring minute_bar_csv_row(const recorded_minute_bar& bar) {
    require_token(bar.exchange_id);
    require_token(bar.instrument_id);
    for (const auto value : {bar.open, bar.high, bar.low, bar.close}) {
        require_number(value, true);
    }
    require_number(bar.open_interest, false);
    if (!ledger_detail::is_valid_trading_day(bar.trading_day)
            || !ledger_detail::has_identifier(bar.time_domain) || bar.begin_ms < 0
            || bar.end_ms <= bar.begin_ms || bar.end_ms - bar.begin_ms > 60000
            || bar.observed_volume < 0 || bar.high < std::max(bar.open, bar.close)
            || bar.low > std::min(bar.open, bar.close) || bar.low > bar.high) {
        throw std::invalid_argument("invalid minute bar recording");
    }
    return sstring(fmt::format("1,{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
        csv_text(bar.exchange_id), csv_text(bar.instrument_id), csv_text(bar.trading_day),
        csv_text(bar.time_domain), bar.begin_ms, bar.end_ms,
        bar.open, bar.high, bar.low, bar.close, bar.observed_volume, bar.open_interest, bar.is_complete));
}

} // namespace seastar::xtrader
