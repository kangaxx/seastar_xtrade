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

#include <seastar/xtrader/market_data_pipeline.hh>

namespace seastar::xtrader {

// B5 versioned, isolated wire formats; encoders perform no blocking I/O.
// Source: development plan B5-B8, 2026-11-10. Signature: GitHub Copilot (gpt-6-astra).
sstring paper_tick_key(const sstring& account_alias, uint64_t run_id,
                       const sstring& exchange, const sstring& instrument);
sstring market_tick_json(const normalized_market_tick& tick);
sstring minute_bar_csv_header();
sstring minute_bar_csv_row(const recorded_minute_bar& bar);

} // namespace seastar::xtrader
