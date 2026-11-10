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
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace seastar::xtrader {

// B4: callback threads copy self-contained POD slots to one owner shard.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
// Slot must contain no borrowed pointers, references, or shard-local objects.
// The sticky loss flag requires reconciliation; it must never be treated as
// proof that the missing callback can be discarded safely.
template <typename Slot, size_t Capacity>
class bounded_callback_queue {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    static_assert(Capacity <= static_cast<size_t>(std::numeric_limits<std::make_signed_t<size_t>>::max()) / 2);
    static_assert(std::is_trivially_copyable_v<Slot>);
    static_assert(std::is_nothrow_copy_assignable_v<Slot>);
    static_assert(std::is_nothrow_default_constructible_v<Slot>);
    static_assert(std::atomic<size_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    struct cell {
        std::atomic<size_t> sequence{0};
        Slot value{};
    };

public:
    bounded_callback_queue() noexcept {
        for (size_t index = 0; index < Capacity; ++index) {
            _cells[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    bounded_callback_queue(const bounded_callback_queue&) = delete;
    bounded_callback_queue& operator=(const bounded_callback_queue&) = delete;

    // Multiple producers may call this concurrently without allocation or locks.
    bool try_push(const Slot& value) noexcept {
        auto position = _enqueue.load(std::memory_order_relaxed);
        cell* target = nullptr;
        for (;;) {
            target = &_cells[position & (Capacity - 1)];
            const auto sequence = target->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::make_signed_t<size_t>>(sequence - position);
            if (difference == 0) {
                if (_enqueue.compare_exchange_weak(position, position + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                _lost.store(true, std::memory_order_release);
                return false;
            } else {
                position = _enqueue.load(std::memory_order_relaxed);
            }
        }
        target->value = value;
        target->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    // Only the owning shard may pop. False can also mean an earlier producer
    // has reserved its slot but has not published it yet.
    bool try_pop(Slot& value) noexcept {
        auto& target = _cells[_dequeue & (Capacity - 1)];
        const auto sequence = target.sequence.load(std::memory_order_acquire);
        if (sequence != _dequeue + 1) {
            return false;
        }
        value = target.value;
        target.sequence.store(_dequeue + Capacity, std::memory_order_release);
        ++_dequeue;
        return true;
    }

    [[nodiscard]] bool has_lost_event() const noexcept {
        return _lost.load(std::memory_order_acquire);
    }

private:
    std::array<cell, Capacity> _cells{};
    alignas(64) std::atomic<size_t> _enqueue{0};
    alignas(64) size_t _dequeue = 0;
    alignas(64) std::atomic<bool> _lost{false};
};

} // namespace seastar::xtrader
