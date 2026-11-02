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

#include <iostream>

namespace seastar::xtrader {

// trading_account_service manages trading account state on a specific shard.
// All account updates are serialized through this service to avoid race conditions.
//
// Thread safety: All operations are serialized on the shard's reactor thread.
class trading_account_service {
public:
    trading_account_service() = default;
    ~trading_account_service() = default;

    // Check if service is initialized
    [[nodiscard]] bool is_initialized() const noexcept {
        return _initialized;
    }

    // Apply account delta atomically
    // Returns: true if applied, false if service not initialized
    bool apply_delta(const domain::account_delta& delta) {
        if (!_initialized) {
            std::cerr << "[ERROR] trading_account_service::apply_delta: "
                      << "service not initialized on shard, dropping delta for "
                      << delta.instrument_id << std::endl;
            return false;
        }
        _account.apply_delta(delta);
        return true;
    }

    // Get current account snapshot
    [[nodiscard]] const domain::trading_account& account() const noexcept {
        return _account;
    }

    // Initialize account with pre-balance
    void initialize(double pre_balance) {
        _account.pre_balance = pre_balance;
        _account.balance = pre_balance;
        _account.available = pre_balance;
        _initialized = true;
    }

    // Get initialized state
    [[nodiscard]] bool initialized() const noexcept { return _initialized; }

    // Get available balance
    [[nodiscard]] double available() const noexcept {
        return _account.available;
    }

    // Check if has sufficient balance for margin
    [[nodiscard]] bool has_sufficient_balance(double required_margin) const noexcept {
        return _account.available >= required_margin;
    }

private:
    domain::trading_account _account;
    bool _initialized = false;  // P0 FIX: track initialization state
};

} // namespace seastar::xtrader
