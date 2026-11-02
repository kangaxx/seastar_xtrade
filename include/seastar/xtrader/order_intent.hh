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

#include <seastar/core/sstring.hh>
#include <seastar/xtrader/domain_types.hh>

#include <string>
#include <vector>

namespace seastar::xtrader {

struct order_intent {
    sstring strategy_id;
    sstring instrument_id;
    domain::side direction = domain::side::buy;
    domain::offset_flag offset = domain::offset_flag::open;
    domain::hedge_flag hedge = domain::hedge_flag::speculation;
    domain::order_type type = domain::order_type::limit;
    int volume = 0;
    double price = 0.0;
    sstring reason_tag;

    [[nodiscard]] bool is_valid() const noexcept {
        return !strategy_id.empty() && !instrument_id.empty() && volume > 0 && price > 0.0;
    }

    [[nodiscard]] domain::order_request to_order_request() const noexcept {
        return domain::order_request {
            .strategy_id = strategy_id,
            .instrument_id = instrument_id,
            .direction = direction,
            .offset = offset,
            .hedge = hedge,
            .volume = volume,
            .price = price,
        };
    }
};

class strategy_order_mapper {
public:
    strategy_order_mapper() = default;

    void map(const sstring& strategy_id, const sstring& order_ref);
    sstring find_strategy_id(const sstring& order_ref) const;
    sstring find_strategy_id_by_sys_id(const sstring& order_sys_id) const;
    void map_sys_id(const sstring& order_ref, const sstring& order_sys_id);

    void remove(const sstring& order_ref);

    size_t pending_count() const noexcept { return _ref_to_strategy.size(); }

private:
    std::unordered_map<sstring, sstring> _ref_to_strategy;
    std::unordered_map<sstring, sstring> _sys_id_to_ref;
};

} // namespace seastar::xtrader
