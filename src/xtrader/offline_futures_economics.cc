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
#include <seastar/xtrader/offline_futures_economics.hh>

#include "ledger_validation.hh"

#include <boost/multiprecision/cpp_int.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace seastar::xtrader {
namespace {

// B2: use explicitly configured ratios and checked integer monetary units.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
void validate_rate(const offline_rate& rate) {
    if (rate.numerator < 0 || rate.denominator <= 0 || !rate.rounding
        || (*rate.rounding != offline_rounding::down && *rate.rounding != offline_rounding::up
            && *rate.rounding != offline_rounding::half_up)) {
        throw std::invalid_argument("an explicit nonnegative rate and rounding rule are required");
    }
}

void validate_fee(const offline_fee_rule& fee) {
    if (fee.fixed_per_lot < 0) {
        throw std::invalid_argument("an explicit nonnegative per-lot fee is required");
    }
    validate_rate(fee.notional_rate);
}

void validate_direction(domain::side direction) {
    if (!ledger_detail::is_valid_direction(direction)) {
        throw std::invalid_argument("a valid order direction is required");
    }
}

int64_t checked_add(int64_t left, int64_t right) {
    if (left < 0 || right < 0 || left > std::numeric_limits<int64_t>::max() - right) {
        throw std::overflow_error("offline economics addition overflow");
    }
    return left + right;
}

int64_t checked_multiply(int64_t left, int64_t right) {
    if (left < 0 || right < 0 || (right != 0 && left > std::numeric_limits<int64_t>::max() / right)) {
        throw std::overflow_error("offline economics multiplication overflow");
    }
    return left * right;
}

int64_t apply_rate(int64_t amount, const offline_rate& rate) {
    using boost::multiprecision::uint128_t;
    const uint128_t product = uint128_t(amount) * uint128_t(rate.numerator);
    uint128_t result = product / rate.denominator;
    const uint128_t remainder = product % rate.denominator;
    if ((*rate.rounding == offline_rounding::up && remainder != 0)
        || (*rate.rounding == offline_rounding::half_up
            && remainder * 2 >= uint128_t(rate.denominator))) {
        ++result;
    }
    if (result > uint128_t(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("offline economics rate result overflow");
    }
    return result.convert_to<int64_t>();
}

void validate_fill(int64_t price_ticks, int64_t maximum_price_ticks, int volume) {
    if (price_ticks <= 0 || maximum_price_ticks <= 0 || price_ticks > maximum_price_ticks || volume <= 0) {
        throw std::invalid_argument("fill price, reserved maximum price, and volume are inconsistent");
    }
}

} // namespace

offline_futures_economics::offline_futures_economics(offline_futures_terms terms)
    : _terms(std::move(terms)) {
    if (!ledger_detail::is_valid_instrument(_terms.instrument)
        || !ledger_detail::has_identifier(_terms.money_unit) || _terms.tick_value_per_lot <= 0
        || !std::isfinite(_terms.price_tick) || _terms.price_tick <= 0.0) {
        throw std::invalid_argument("an instrument, money unit, tick size, and tick value are required");
    }
    validate_rate(_terms.long_margin_rate);
    validate_rate(_terms.short_margin_rate);
    validate_fee(_terms.open_fee);
    validate_fee(_terms.close_fee);
    validate_fee(_terms.close_today_fee);
    validate_fee(_terms.close_yesterday_fee);
}

double offline_futures_economics::price_at_ticks(int64_t price_ticks) const {
    constexpr int64_t MAX_EXACT_TICKS = (int64_t{1} << 53) - 1;
    if (price_ticks <= 0 || price_ticks > MAX_EXACT_TICKS) {
        throw std::invalid_argument("price ticks cannot be represented unambiguously");
    }
    const double price = static_cast<double>(price_ticks) * _terms.price_tick;
    const double previous = static_cast<double>(price_ticks - 1) * _terms.price_tick;
    const double next = static_cast<double>(price_ticks + 1) * _terms.price_tick;
    if (!std::isfinite(price) || !std::isfinite(next) || !(previous < price && price < next)) {
        throw std::invalid_argument("adjacent tick prices cannot be represented unambiguously");
    }
    return price;
}

offline_normalized_price offline_futures_economics::normalize_price(
        double price, offline_price_normalization policy) const {
    constexpr int64_t MAX_EXACT_TICKS = (int64_t{1} << 53) - 1;
    if (!std::isfinite(price) || price <= 0.0
        || (policy != offline_price_normalization::exact && policy != offline_price_normalization::nearest_one_ulp)) {
        throw std::invalid_argument("a positive finite price and explicit normalization policy are required");
    }
    const long double scaled = static_cast<long double>(price) / static_cast<long double>(_terms.price_tick);
    const long double nearest = std::round(scaled);
    if (!std::isfinite(scaled) || nearest < 1 || nearest > static_cast<long double>(MAX_EXACT_TICKS)) {
        throw std::invalid_argument("price is outside the unambiguous tick range");
    }
    const auto center = static_cast<int64_t>(nearest);
    int64_t exact_tick = 0;
    int64_t near_tick = 0;
    int exact_count = 0;
    int near_count = 0;
    for (int offset = -1; offset <= 1; ++offset) {
        const auto candidate = center + offset;
        if (candidate <= 0 || candidate > MAX_EXACT_TICKS) {
            continue;
        }
        const double candidate_price = static_cast<double>(candidate) * _terms.price_tick;
        if (!std::isfinite(candidate_price)) {
            continue;
        }
        if (price == candidate_price) {
            exact_tick = candidate;
            ++exact_count;
        } else if (price == std::nextafter(candidate_price, -std::numeric_limits<double>::infinity())
                   || price == std::nextafter(candidate_price, std::numeric_limits<double>::infinity())) {
            near_tick = candidate;
            ++near_count;
        }
    }
    if (exact_count == 1) {
        return {.price_ticks = exact_tick, .price = price_at_ticks(exact_tick), .adjusted = false};
    }
    if (exact_count == 0 && near_count == 1 && policy == offline_price_normalization::nearest_one_ulp) {
        return {.price_ticks = near_tick, .price = price_at_ticks(near_tick), .adjusted = true};
    }
    throw std::invalid_argument("price is off tick or ambiguously normalized");
}

int64_t offline_futures_economics::notional_per_lot(int64_t price_ticks) const {
    if (price_ticks <= 0) {
        throw std::invalid_argument("price must be a positive number of ticks");
    }
    return checked_multiply(price_ticks, _terms.tick_value_per_lot);
}

int64_t offline_futures_economics::margin_per_lot(domain::side direction, int64_t notional) const {
    return apply_rate(notional, direction == domain::side::buy
        ? _terms.long_margin_rate : _terms.short_margin_rate);
}

int64_t offline_futures_economics::commission_per_lot(domain::offset_flag offset, int64_t notional) const {
    const offline_fee_rule* fee = nullptr;
    switch (offset) {
    case domain::offset_flag::open: fee = &_terms.open_fee; break;
    case domain::offset_flag::close: fee = &_terms.close_fee; break;
    case domain::offset_flag::close_today: fee = &_terms.close_today_fee; break;
    case domain::offset_flag::close_yesterday: fee = &_terms.close_yesterday_fee; break;
    default: throw std::invalid_argument("a valid open or close offset is required");
    }
    return checked_add(fee->fixed_per_lot, apply_rate(notional, fee->notional_rate));
}

offline_reservation_value offline_futures_economics::reserve(domain::side direction, domain::offset_flag offset,
                                                             int64_t maximum_price_ticks) const {
    validate_direction(direction);
    const auto notional = notional_per_lot(maximum_price_ticks);
    const auto commission = commission_per_lot(offset, notional);
    return {
        .margin_per_lot = offset == domain::offset_flag::open ? margin_per_lot(direction, notional) : 0,
        .commission_per_lot = commission,
    };
}

offline_fill_value offline_futures_economics::open_fill(domain::side direction, int64_t price_ticks,
                                                        int64_t maximum_price_ticks, int volume) const {
    validate_direction(direction);
    validate_fill(price_ticks, maximum_price_ticks, volume);
    const auto notional = notional_per_lot(price_ticks);
    return {
        .margin_change = checked_multiply(margin_per_lot(direction, notional), volume),
        .commission = checked_multiply(commission_per_lot(domain::offset_flag::open, notional), volume),
        .close_profit = 0,
    };
}

offline_fill_value offline_futures_economics::close_fill(domain::side closing_direction, domain::offset_flag offset,
        int64_t opening_price_ticks, int64_t price_ticks, int64_t maximum_price_ticks,
        int64_t opening_margin_per_lot, int volume) const {
    validate_direction(closing_direction);
    validate_fill(price_ticks, maximum_price_ticks, volume);
    if (opening_price_ticks <= 0 || opening_margin_per_lot < 0 || offset == domain::offset_flag::open) {
        throw std::invalid_argument("close fill requires a valid opening lot and close offset");
    }
    const auto notional = notional_per_lot(price_ticks);
    const auto commission = checked_multiply(commission_per_lot(offset, notional), volume);
    const auto margin = checked_multiply(opening_margin_per_lot, volume);
    const int64_t price_difference = closing_direction == domain::side::sell
        ? price_ticks - opening_price_ticks : opening_price_ticks - price_ticks;
    const auto absolute_ticks = price_difference < 0 ? -price_difference : price_difference;
    const auto profit = checked_multiply(checked_multiply(absolute_ticks, _terms.tick_value_per_lot), volume);
    return {
        .margin_change = -margin,
        .commission = commission,
        .close_profit = price_difference < 0 ? -profit : profit,
    };
}

} // namespace seastar::xtrader
