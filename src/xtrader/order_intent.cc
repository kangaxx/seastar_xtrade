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
#include <seastar/xtrader/order_intent.hh>

#include <iostream>

namespace seastar::xtrader {

void strategy_order_mapper::map(const sstring& strategy_id,
                                  const sstring& order_ref) {
    _ref_to_strategy[order_ref] = strategy_id;
}

sstring strategy_order_mapper::find_strategy_id(
    const sstring& order_ref) const {
    auto it = _ref_to_strategy.find(order_ref);
    if (it != _ref_to_strategy.end()) {
        return it->second;
    }
    return {};
}

sstring strategy_order_mapper::find_strategy_id_by_sys_id(
    const sstring& order_sys_id) const {
    auto it = _sys_id_to_ref.find(order_sys_id);
    if (it != _sys_id_to_ref.end()) {
        return find_strategy_id(it->second);
    }
    return {};
}

void strategy_order_mapper::map_sys_id(const sstring& order_ref,
                                         const sstring& order_sys_id) {
    _sys_id_to_ref[order_sys_id] = order_ref;
}

void strategy_order_mapper::remove(const sstring& order_ref) {
    _ref_to_strategy.erase(order_ref);

    for (auto it = _sys_id_to_ref.begin(); it != _sys_id_to_ref.end(); ) {
        if (it->second == order_ref) {
            it = _sys_id_to_ref.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace seastar::xtrader
