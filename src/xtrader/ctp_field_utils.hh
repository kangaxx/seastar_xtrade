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

#include <seastar/core/sstring.hh>

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace seastar::xtrader::ctp_detail {

// B4: fixed-size SDK fields must never be silently truncated or overread.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
template <size_t Size>
sstring ctp_string(const char (&value)[Size]) {
    const auto* end = static_cast<const char*>(std::memchr(value, '\0', Size));
    if (!end) {
        throw std::runtime_error("CTP fixed-size string is not terminated");
    }
    return sstring(value, static_cast<size_t>(end - value));
}

template <size_t Size>
void write_field(char (&destination)[Size], const sstring& value) {
    if (value.size() >= Size || (!value.empty() && std::memchr(value.data(), '\0', value.size()))) {
        throw std::invalid_argument("CTP fixed-size string is too long or contains NUL");
    }
    std::memset(destination, 0, Size);
    if (!value.empty()) {
        std::memcpy(destination, value.data(), value.size());
    }
}

} // namespace seastar::xtrader::ctp_detail
