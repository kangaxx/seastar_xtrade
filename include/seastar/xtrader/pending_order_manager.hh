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

#include <chrono>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace seastar::xtrader {

// === Pending Order Status ===
// Tracks the lifecycle of a submitted order

struct pending_order {
    domain::order_request request;
    std::string order_ref;           // Client-side order reference
    std::string order_sys_id;        // Exchange-assigned order ID (filled by OnRtnOrder)
    std::string broker_id;
    std::string investor_id;
    int front_id = 0;                 // Filled by OnRtnOrder
    int session_id = 0;               // Filled by OnRtnOrder
    domain::order_status status = domain::order_status::submitted;
    std::chrono::steady_clock::time_point submit_time;
    std::chrono::steady_clock::time_point last_update_time;
    int traded_volume = 0;
    std::string error_msg;
};

// === Pending Order Manager ===
//
// Responsibilities:
// - Track all pending orders by order_ref
// - Update order status based on CTP callbacks
// - Support cancel operations
// - Detect timeout conditions
//
// Thread safety:
// - All methods must be called from the same shard (Gateway Shard)
// - This is a single-threaded in-memory data structure
//
class pending_order_manager {
public:
    using clock = std::chrono::steady_clock;

    pending_order_manager() = default;
    ~pending_order_manager() = default;

    // Disable copy/move
    pending_order_manager(const pending_order_manager&) = delete;
    pending_order_manager& operator=(const pending_order_manager&) = delete;

    // ==================== Order Lifecycle ====================

    /// Add a new pending order
    void add_order(const std::string& order_ref, pending_order order);

    /// Update order with system ID (from OnRtnOrder)
    void set_order_sys_id(const std::string& order_ref, 
                          const std::string& order_sys_id,
                          int front_id,
                          int session_id);

    /// Update order status
    void update_status(const std::string& order_ref, domain::order_status status);

    /// Update traded volume
    void update_traded_volume(const std::string& order_ref, int traded_volume);

    /// Set error message
    void set_error(const std::string& order_ref, const std::string& error_msg);

    /// Remove order (when terminal state reached)
    void remove_order(const std::string& order_ref);

    // ==================== Queries ====================

    /// Get order by order_ref
    [[nodiscard]] const pending_order* get_by_ref(const std::string& order_ref) const;
    [[nodiscard]] pending_order* get_by_ref(const std::string& order_ref);

    /// Get order by system ID
    [[nodiscard]] const pending_order* get_by_sys_id(const std::string& order_sys_id) const;
    [[nodiscard]] pending_order* get_by_sys_id(const std::string& order_sys_id);

    /// Check if order is in terminal state
    [[nodiscard]] bool is_terminal(const std::string& order_ref) const;

    /// Check if order can be canceled
    [[nodiscard]] bool can_cancel(const std::string& order_ref) const;

    /// Count pending orders
    [[nodiscard]] size_t pending_count() const;

    /// Get all pending orders
    [[nodiscard]] const std::unordered_map<std::string, pending_order>& all_orders() const;

    // ==================== Timeout Detection ====================

    /// Find orders that have exceeded timeout
    [[nodiscard]] std::vector<std::string> find_timed_out_orders(
        clock::duration timeout) const;

    /// Clean up orders in terminal state older than threshold
    void cleanup_old_orders(clock::duration max_age);

private:
    std::unordered_map<std::string, pending_order> _orders_by_ref;
    std::unordered_map<std::string, std::string> _orders_by_sys_id;  // sys_id -> ref

    static constexpr auto max_pending_orders = 10000;
};

// ==================== Helper Functions ====================

/// Generate a unique order_ref
inline std::string generate_order_ref(uint64_t& seq) {
    // Format: timestamp(10 digits) + sequence(12 digits)
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return fmt::format("{:010d}{:012d}", now % 10000000000ULL, ++seq);
}

/// Check if status is terminal
inline bool is_terminal_status(domain::order_status status) {
    return status == domain::order_status::filled ||
           status == domain::order_status::canceled ||
           status == domain::order_status::rejected;
}

} // namespace seastar::xtrader
