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
#include <seastar/xtrader/pending_order_manager.hh>

#include <algorithm>
#include <stdexcept>

namespace seastar::xtrader {

void pending_order_manager::add_order(const std::string& order_ref, pending_order order) {
    // B4: never silently reuse a client identity or exceed the tracked-order bound.
    // Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
    if (order_ref.empty() || order.order_ref != order_ref || order.broker_id.empty()
        || order.investor_id.empty() || order.request.volume <= 0
        || _orders_by_ref.contains(order_ref)) {
        throw std::invalid_argument("invalid or duplicate pending order identity");
    }
    if (_orders_by_ref.size() >= max_pending_orders) {
        std::cerr << "[WARN] pending_order_manager: max pending orders reached, "
                  << "cleaning up old orders" << std::endl;
        cleanup_old_orders(std::chrono::hours(1));
        if (_orders_by_ref.size() >= max_pending_orders) {
            throw std::runtime_error("pending order capacity exhausted");
        }
    }

    order.submit_time = clock::now();
    order.last_update_time = order.submit_time;
    if (!_orders_by_ref.emplace(order_ref, std::move(order)).second) {
        throw std::logic_error("pending order identity was reused");
    }
}

void pending_order_manager::set_order_sys_id(const std::string& order_ref,
                                             const std::string& order_sys_id,
                                             const std::string& exchange_id,
                                             int front_id,
                                             int session_id) {
    auto it = _orders_by_ref.find(order_ref);
    if (it == _orders_by_ref.end() || order_sys_id.empty() || exchange_id.empty()) {
        throw std::invalid_argument("system order id requires a known pending order");
    }
    if ((!it->second.order_sys_id.empty() && it->second.order_sys_id != order_sys_id)
        || (!it->second.exchange_id.empty() && it->second.exchange_id != exchange_id)
        || (it->second.front_id != 0 && it->second.front_id != front_id)
        || (it->second.session_id != 0 && it->second.session_id != session_id)) {
        throw std::runtime_error("pending order identity changed");
    }
    const auto existing = _orders_by_sys_id.find(order_sys_id);
    if (existing != _orders_by_sys_id.end() && existing->second != order_ref) {
        throw std::runtime_error("system order id is ambiguous");
    }

    _orders_by_sys_id.emplace(order_sys_id, order_ref);
    it->second.order_sys_id = order_sys_id;
    it->second.exchange_id = exchange_id;
    it->second.front_id = front_id;
    it->second.session_id = session_id;
    it->second.last_update_time = clock::now();

}

void pending_order_manager::update_status(const std::string& order_ref, 
                                           domain::order_status status) {
    auto it = _orders_by_ref.find(order_ref);
    if (it == _orders_by_ref.end()) {
        // Try by sys_id
        auto sys_it = _orders_by_sys_id.find(order_ref);
        if (sys_it != _orders_by_sys_id.end()) {
            it = _orders_by_ref.find(sys_it->second);
        }
    }

    if (it == _orders_by_ref.end()) {
        throw std::invalid_argument("pending order status has no matching order");
    }
    const auto previous = it->second.status;
    const auto filled = it->second.traded_volume;
    const auto total = it->second.request.volume;
    const bool known = status == domain::order_status::submitted || status == domain::order_status::unknown
        || status == domain::order_status::accepted || status == domain::order_status::partially_filled
        || is_terminal_status(status);
    const bool contradictory_volume = (status == domain::order_status::accepted && filled != 0)
        || (status == domain::order_status::partially_filled && (filled <= 0 || filled >= total))
        || (status == domain::order_status::filled && filled != total)
        || (status == domain::order_status::canceled && filled >= total)
        || (status == domain::order_status::rejected && filled != 0);
    if (!known || contradictory_volume
        || (is_terminal_status(previous) && status != previous)
        || (previous == domain::order_status::partially_filled
            && (status == domain::order_status::submitted || status == domain::order_status::unknown
                || status == domain::order_status::accepted || status == domain::order_status::rejected))
        || (previous == domain::order_status::accepted
            && (status == domain::order_status::submitted || status == domain::order_status::unknown))) {
        throw std::runtime_error("pending order status is inconsistent; reconciliation is required");
    }

    it->second.status = status;
    it->second.last_update_time = clock::now();

}

void pending_order_manager::update_traded_volume(const std::string& order_ref, 
                                                  int traded_volume) {
    auto it = _orders_by_ref.find(order_ref);
    if (it == _orders_by_ref.end()) {
        throw std::invalid_argument("traded volume has no matching pending order");
    }
    if (traded_volume < it->second.traded_volume || traded_volume > it->second.request.volume
        || (is_terminal_status(it->second.status) && traded_volume != it->second.traded_volume)) {
        throw std::runtime_error("pending order traded volume is inconsistent");
    }
    it->second.traded_volume = traded_volume;
    it->second.last_update_time = clock::now();
}

void pending_order_manager::set_error(const std::string& order_ref, 
                                       const std::string& error_msg) {
    auto it = _orders_by_ref.find(order_ref);
    if (it == _orders_by_ref.end()) {
        throw std::invalid_argument("pending order error has no matching order");
    }
    it->second.error_msg = error_msg;
    it->second.last_update_time = clock::now();
}

void pending_order_manager::remove_order(const std::string& order_ref) {
    auto it = _orders_by_ref.find(order_ref);
    if (it == _orders_by_ref.end()) {
        return;
    }
    if (!is_terminal_status(it->second.status)) {
        throw std::logic_error("cannot remove an active pending order");
    }

    // Remove sys_id mapping if exists
    if (!it->second.order_sys_id.empty()) {
        _orders_by_sys_id.erase(it->second.order_sys_id);
    }

    _orders_by_ref.erase(it);
}

const pending_order* pending_order_manager::get_by_ref(const std::string& order_ref) const {
    auto it = _orders_by_ref.find(order_ref);
    if (it != _orders_by_ref.end()) {
        return &it->second;
    }
    return nullptr;
}

pending_order* pending_order_manager::get_by_ref(const std::string& order_ref) {
    auto it = _orders_by_ref.find(order_ref);
    if (it != _orders_by_ref.end()) {
        return &it->second;
    }
    return nullptr;
}

const pending_order* pending_order_manager::get_by_sys_id(const std::string& order_sys_id) const {
    auto sys_it = _orders_by_sys_id.find(order_sys_id);
    if (sys_it != _orders_by_sys_id.end()) {
        return get_by_ref(sys_it->second);
    }
    return nullptr;
}

pending_order* pending_order_manager::get_by_sys_id(const std::string& order_sys_id) {
    auto sys_it = _orders_by_sys_id.find(order_sys_id);
    if (sys_it != _orders_by_sys_id.end()) {
        return get_by_ref(sys_it->second);
    }
    return nullptr;
}

bool pending_order_manager::is_terminal(const std::string& order_ref) const {
    const auto* order = get_by_ref(order_ref);
    if (!order) {
        return false;
    }
    return is_terminal_status(order->status);
}

bool pending_order_manager::can_cancel(const std::string& order_ref) const {
    const auto* order = get_by_ref(order_ref);
    if (!order) {
        return false;
    }

    // Can cancel if:
    // 1. Not in terminal state
    // 2. Not already fully traded
    // 3. Has remaining volume
    if (is_terminal_status(order->status) || order->order_sys_id.empty()) {
        return false;
    }
    if (order->status == domain::order_status::partially_filled &&
        order->traded_volume >= order->request.volume) {
        return false;
    }
    return true;
}

size_t pending_order_manager::pending_count() const {
    return _orders_by_ref.size();
}

const std::unordered_map<std::string, pending_order>& 
pending_order_manager::all_orders() const {
    return _orders_by_ref;
}

std::vector<std::string> pending_order_manager::find_timed_out_orders(
    clock::duration timeout) const {
    std::vector<std::string> result;
    auto now = clock::now();

    for (const auto& [ref, order] : _orders_by_ref) {
        if (is_terminal_status(order.status)) {
            continue;  // Skip terminal orders
        }

        auto elapsed = now - order.submit_time;
        if (elapsed > timeout) {
            result.push_back(ref);
        }
    }

    return result;
}

void pending_order_manager::cleanup_old_orders(clock::duration max_age) {
    auto now = clock::now();
    std::vector<std::string> to_remove;

    for (const auto& [ref, order] : _orders_by_ref) {
        if (is_terminal_status(order.status)) {
            auto age = now - order.last_update_time;
            if (age > max_age) {
                to_remove.push_back(ref);
            }
        }
    }

    for (const auto& ref : to_remove) {
        remove_order(ref);
    }

    if (!to_remove.empty()) {
        std::cout << "[INFO] pending_order_manager: cleaned up " << to_remove.size()
                  << " old orders" << std::endl;
    }
}

} // namespace seastar::xtrader
