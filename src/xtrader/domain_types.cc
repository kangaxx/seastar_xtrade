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
#include <seastar/xtrader/domain_types.hh>

#include <algorithm>
#include <cmath>

namespace seastar::xtrader::domain {

int position_book::consume(int& today, int& yesterday, int volume) noexcept {
    const int from_today = std::min(today, volume);
    today -= from_today;
    volume -= from_today;

    const int from_yesterday = std::min(yesterday, volume);
    yesterday -= from_yesterday;
    volume -= from_yesterday;

    return volume;
}

void position_book::apply_trade(const position_delta& delta) {
    if (delta.traded_volume <= 0) {
        return;
    }

    int unhandled = delta.traded_volume;

    if (delta.offset == offset_flag::open) {
        if (delta.direction == side::buy) {
            _view.long_today += delta.traded_volume;
        } else {
            _view.short_today += delta.traded_volume;
        }
        return;
    }

    if (delta.direction == side::buy) {
        if (delta.offset == offset_flag::close_today) {
            _view.short_today = std::max(0, _view.short_today - delta.traded_volume);
            return;
        }
        if (delta.offset == offset_flag::close_yesterday) {
            _view.short_yesterday = std::max(0, _view.short_yesterday - delta.traded_volume);
            return;
        }
        unhandled = consume(_view.short_today, _view.short_yesterday, delta.traded_volume);
    } else {
        if (delta.offset == offset_flag::close_today) {
            _view.long_today = std::max(0, _view.long_today - delta.traded_volume);
            return;
        }
        if (delta.offset == offset_flag::close_yesterday) {
            _view.long_yesterday = std::max(0, _view.long_yesterday - delta.traded_volume);
            return;
        }
        unhandled = consume(_view.long_today, _view.long_yesterday, delta.traded_volume);
    }

    if (unhandled > 0) {
        // Keep the skeleton deterministic: do not allow negative inventory in bootstrap stage.
        if (delta.direction == side::buy) {
            _view.short_yesterday = std::max(0, _view.short_yesterday - unhandled);
        } else {
            _view.long_yesterday = std::max(0, _view.long_yesterday - unhandled);
        }
    }
}

// B2: reject inconsistent account state or malformed settlement deltas atomically.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
bool trading_account::apply_delta(const account_delta& delta) noexcept {
    const bool valid_direction = delta.direction == side::buy || delta.direction == side::sell;
    const bool valid_offset = delta.offset == offset_flag::open || delta.offset == offset_flag::close
        || delta.offset == offset_flag::close_today || delta.offset == offset_flag::close_yesterday;
    if (!valid_direction || !valid_offset || delta.traded_volume < 0
        || !std::isfinite(delta.price) || delta.price < 0.0
        || !std::isfinite(delta.commission) || delta.commission < 0.0
        || !std::isfinite(delta.margin_change) || !std::isfinite(delta.close_profit_change)
        || (delta.offset == offset_flag::open && delta.close_profit_change != 0.0)) {
        return false;
    }
    if (!std::isfinite(pre_balance) || !std::isfinite(balance) || !std::isfinite(available)
        || !std::isfinite(curr_margin) || curr_margin < 0.0
        || !std::isfinite(frozen_margin) || frozen_margin < 0.0
        || !std::isfinite(frozen_commission) || frozen_commission < 0.0
        || !std::isfinite(commission) || commission < 0.0
        || !std::isfinite(close_profit) || !std::isfinite(position_profit)) {
        return false;
    }

    const bool is_open = delta.offset == offset_flag::open;
    const double next_margin = curr_margin + delta.margin_change;
    const double next_commission = commission + delta.commission;
    const double next_close_profit = close_profit + (is_open ? 0.0 : delta.close_profit_change);
    const double next_balance = balance - delta.commission
        + (is_open ? 0.0 : delta.close_profit_change);
    const double next_available = next_balance - next_margin - frozen_margin - frozen_commission;
    if (!std::isfinite(next_margin) || next_margin < 0.0
        || !std::isfinite(next_commission) || !std::isfinite(next_close_profit)
        || !std::isfinite(next_balance) || !std::isfinite(next_available)) {
        return false;
    }

    curr_margin = next_margin;
    commission = next_commission;
    close_profit = next_close_profit;
    balance = next_balance;
    available = next_available;
    return true;
}

} // namespace seastar::xtrader::domain
