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

#include <atomic>
#include <array>

namespace seastar::xtrader {

// SPSC (Single Producer Single Consumer) Lock-Free Ring Buffer
// Used for bridging CTP callback threads (producers) with Seastar Reactor threads (consumers).
//
// Constraints:
// - push() is only called from the CTP callback thread (single producer)
// - pop() is only called from the Seastar Reactor thread (single consumer)
// - Capacity must be a power of 2
// - head_ and tail_ are on separate cache lines (alignas(64)) to avoid false sharing
//
template<typename Slot, size_t Capacity>
class spsc_ring_buffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    static constexpr size_t capacity = Capacity;

    // Called from CTP callback thread (single producer)
    // Returns true if push succeeded, false if queue is full.
    bool push(const Slot& item) noexcept {
        auto tail = _tail.load(std::memory_order_relaxed);
        auto head = _head.load(std::memory_order_acquire);
        if (tail - head >= Capacity) {
            return false;  // queue full
        }
        _slots[(tail & (Capacity - 1))] = item;
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Called from Seastar Reactor thread (single consumer)
    // Returns true if pop succeeded, false if queue is empty.
    bool pop(Slot& item) noexcept {
        auto head = _head.load(std::memory_order_relaxed);
        auto tail = _tail.load(std::memory_order_acquire);
        if (head >= tail) {
            return false;  // queue empty
        }
        item = _slots[(head & (Capacity - 1))];
        _head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate size (not lock-free accurate, for monitoring only)
    [[nodiscard]] size_t approx_size() const noexcept {
        auto tail = _tail.load(std::memory_order_relaxed);
        auto head = _head.load(std::memory_order_relaxed);
        return (tail > head) ? (tail - head) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return _head.load(std::memory_order_relaxed) >= _tail.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<uint64_t> _head{0};  // consumer position
    alignas(64) std::atomic<uint64_t> _tail{0}; // producer position
    std::array<Slot, Capacity> _slots{};
};

// === Slot Types for CTP Bridge ===

struct md_slot {
    uint64_t capture_ns = 0;   // timestamp when callback entered (nanoseconds since boot)
    domain::market_data data;
};

struct trader_slot {
    uint64_t capture_ns = 0;
    domain::event_type type = domain::event_type::order;
    domain::order order_data;
    domain::trade_report trade_data;
};

// === Constants ===

inline constexpr size_t md_ring_capacity = 65536;    // 2^16, must be power of 2
inline constexpr size_t trader_ring_capacity = 16384; // 2^14, must be power of 2

using md_ring_buffer = spsc_ring_buffer<md_slot, md_ring_capacity>;
using trader_ring_buffer = spsc_ring_buffer<trader_slot, trader_ring_capacity>;

} // namespace seastar::xtrader
