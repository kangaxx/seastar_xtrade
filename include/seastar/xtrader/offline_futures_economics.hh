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

#include <cstdint>
#include <optional>

namespace seastar::xtrader {

// B2: explicit fixed-point economics for synthetic offline futures scenarios.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
enum class offline_rounding {
    down,
    up,
    half_up,
};

enum class offline_price_normalization {
    exact,
    nearest_one_ulp,
};

struct offline_normalized_price {
    int64_t price_ticks = 0;
    double price = 0.0;
    bool adjusted = false;
};

struct offline_rate {
    int64_t numerator = -1;
    int64_t denominator = 0;
    std::optional<offline_rounding> rounding;
};

struct offline_fee_rule {
    int64_t fixed_per_lot = -1;
    offline_rate notional_rate;
};

struct offline_futures_terms {
    position_instrument instrument;
    sstring money_unit;
    int64_t tick_value_per_lot = -1;
    double price_tick = 0.0;
    offline_rate long_margin_rate;
    offline_rate short_margin_rate;
    offline_fee_rule open_fee;
    offline_fee_rule close_fee;
    offline_fee_rule close_today_fee;
    offline_fee_rule close_yesterday_fee;
};

struct offline_reservation_value {
    int64_t margin_per_lot = 0;
    int64_t commission_per_lot = 0;
};

struct offline_fill_value {
    int64_t margin_change = 0;
    int64_t commission = 0;
    int64_t close_profit = 0;
};

// Pure offline calculations. Prices are positive integer ticks; the caller must
// bind them, the account unit, and retained opening lots to the supplied terms.
// Results are model outputs, not authoritative exchange or broker settlement.
class offline_futures_economics {
public:
    explicit offline_futures_economics(offline_futures_terms terms);

    [[nodiscard]] const offline_futures_terms& terms() const noexcept { return _terms; }
    [[nodiscard]] double price_at_ticks(int64_t price_ticks) const;
    [[nodiscard]] offline_normalized_price normalize_price(double price, offline_price_normalization policy) const;
    [[nodiscard]] offline_reservation_value reserve(domain::side direction, domain::offset_flag offset,
                                                     int64_t maximum_price_ticks) const;
    [[nodiscard]] offline_fill_value open_fill(domain::side direction, int64_t price_ticks,
                                               int64_t maximum_price_ticks, int volume) const;
    [[nodiscard]] offline_fill_value close_fill(domain::side closing_direction, domain::offset_flag offset,
                                                int64_t opening_price_ticks, int64_t price_ticks,
                                                int64_t maximum_price_ticks, int64_t opening_margin_per_lot,
                                                int volume) const;

private:
    [[nodiscard]] int64_t notional_per_lot(int64_t price_ticks) const;
    [[nodiscard]] int64_t margin_per_lot(domain::side direction, int64_t notional) const;
    [[nodiscard]] int64_t commission_per_lot(domain::offset_flag offset, int64_t notional) const;

    offline_futures_terms _terms;
};

} // namespace seastar::xtrader
