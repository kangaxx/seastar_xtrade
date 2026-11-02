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

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace seastar::xtrader {

enum class event_kind {
    tick,
    bar,
    order,
    trade,
    account,
    timer,
};

struct subscription_entry {
    sstring instrument_id;
    sstring strategy_id;
    event_kind kind;
    int priority = 0;
};

class subscription_registry {
public:
    subscription_registry() = default;

    future<> subscribe(const sstring& strategy_id,
                        const sstring& instrument_id,
                        event_kind kind,
                        int priority = 0);
    future<> unsubscribe(const sstring& strategy_id,
                          const sstring& instrument_id,
                          event_kind kind);

    std::vector<subscription_entry> find_subscribers(
        const sstring& instrument_id,
        event_kind kind) const;

    size_t subscription_count() const noexcept { return _subscriptions.size(); }
    bool has_subscriber(const sstring& strategy_id,
                        const sstring& instrument_id,
                        event_kind kind) const;

private:
    struct key_t {
        sstring instrument_id;
        sstring strategy_id;
        event_kind kind;

        bool operator==(const key_t& o) const {
            return instrument_id == o.instrument_id
                && strategy_id == o.strategy_id
                && kind == o.kind;
        }
    };

    struct key_hash {
        size_t operator()(const key_t& k) const {
            size_t h1 = std::hash<std::string>{}(k.instrument_id);
            size_t h2 = std::hash<std::string>{}(k.strategy_id);
            size_t h3 = static_cast<size_t>(k.kind);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    std::unordered_map<key_t, subscription_entry, key_hash> _subscriptions;
};

} // namespace seastar::xtrader
