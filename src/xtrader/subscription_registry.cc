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
#include <seastar/xtrader/subscription_registry.hh>

#include <algorithm>
#include <iostream>

namespace seastar::xtrader {

future<> subscription_registry::subscribe(const sstring& strategy_id,
                                           const sstring& instrument_id,
                                           event_kind kind,
                                           int priority) {
    key_t key{instrument_id, strategy_id, kind};
    auto it = _subscriptions.find(key);
    if (it != _subscriptions.end()) {
        it->second.priority = priority;
    } else {
        _subscriptions.emplace(std::move(key),
            subscription_entry{instrument_id, strategy_id, kind, priority});
    }
    return make_ready_future<>();
}

future<> subscription_registry::unsubscribe(const sstring& strategy_id,
                                             const sstring& instrument_id,
                                             event_kind kind) {
    key_t key{instrument_id, strategy_id, kind};
    _subscriptions.erase(key);
    return make_ready_future<>();
}

std::vector<subscription_entry> subscription_registry::find_subscribers(
    const sstring& instrument_id,
    event_kind kind) const {
    std::vector<subscription_entry> result;
    for (const auto& [key, entry] : _subscriptions) {
        if (key.instrument_id == instrument_id && key.kind == kind) {
            result.push_back(entry);
        }
    }
    std::sort(result.begin(), result.end(),
        [](const subscription_entry& a, const subscription_entry& b) {
            return a.priority > b.priority;
        });
    return result;
}

bool subscription_registry::has_subscriber(const sstring& strategy_id,
                                            const sstring& instrument_id,
                                            event_kind kind) const {
    key_t key{instrument_id, strategy_id, kind};
    return _subscriptions.find(key) != _subscriptions.end();
}

} // namespace seastar::xtrader
