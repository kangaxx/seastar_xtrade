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

#include <hiredis/hiredis.h>

#include <string>
#include <vector>

namespace seastar::xtrader {

class redis_sync_client {
public:
    redis_sync_client() = default;
    ~redis_sync_client();

    redis_sync_client(const redis_sync_client&) = delete;
    redis_sync_client& operator=(const redis_sync_client&) = delete;

    bool connect(const std::string& host, int port, int timeout_ms = 3000);
    void disconnect();

    [[nodiscard]] bool is_connected() const noexcept;

    [[nodiscard]] bool set(const std::string& key, const std::string& value);
    [[nodiscard]] std::string get(const std::string& key);
    [[nodiscard]] long long del(const std::string& key);

    [[nodiscard]] long long lpush(const std::string& key, const std::string& value);
    [[nodiscard]] std::vector<std::string> lrange(const std::string& key, int start, int stop);
    [[nodiscard]] long long llen(const std::string& key);
    [[nodiscard]] std::vector<std::string> keys(const std::string& pattern);

private:
    redisContext* _ctx = nullptr;
};

} // namespace seastar::xtrader
