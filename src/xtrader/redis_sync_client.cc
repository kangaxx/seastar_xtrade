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
#include <seastar/xtrader/redis_sync_client.hh>

#include <sys/time.h>

namespace seastar::xtrader {

redis_sync_client::~redis_sync_client() {
    disconnect();
}

bool redis_sync_client::connect(const std::string& host, int port, int timeout_ms) {
    disconnect();

    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    _ctx = redisConnectWithTimeout(host.c_str(), port, tv);
    if (_ctx == nullptr || _ctx->err) {
        disconnect();
        return false;
    }
    return true;
}

void redis_sync_client::disconnect() {
    if (_ctx != nullptr) {
        redisFree(_ctx);
        _ctx = nullptr;
    }
}

bool redis_sync_client::is_connected() const noexcept {
    return _ctx != nullptr;
}

bool redis_sync_client::set(const std::string& key, const std::string& value) {
    if (!is_connected()) {
        return false;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "SET %b %b", key.data(), key.size(), value.data(), value.size()));

    const bool ok = reply != nullptr && reply->type == REDIS_REPLY_STATUS;
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return ok;
}

std::string redis_sync_client::get(const std::string& key) {
    if (!is_connected()) {
        return {};
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "GET %b", key.data(), key.size()));

    std::string value;
    if (reply != nullptr && reply->type == REDIS_REPLY_STRING && reply->str != nullptr) {
        value.assign(reply->str, reply->len);
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return value;
}

long long redis_sync_client::del(const std::string& key) {
    if (!is_connected()) {
        return 0;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "DEL %b", key.data(), key.size()));

    long long ret = 0;
    if (reply != nullptr && reply->type == REDIS_REPLY_INTEGER) {
        ret = reply->integer;
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return ret;
}

long long redis_sync_client::lpush(const std::string& key, const std::string& value) {
    if (!is_connected()) {
        return 0;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "LPUSH %b %b", key.data(), key.size(), value.data(), value.size()));

    long long ret = 0;
    if (reply != nullptr && reply->type == REDIS_REPLY_INTEGER) {
        ret = reply->integer;
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return ret;
}

std::vector<std::string> redis_sync_client::lrange(const std::string& key, int start, int stop) {
    std::vector<std::string> out;
    if (!is_connected()) {
        return out;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "LRANGE %b %d %d", key.data(), key.size(), start, stop));

    if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY) {
        out.reserve(reply->elements);
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i] != nullptr && reply->element[i]->type == REDIS_REPLY_STRING && reply->element[i]->str != nullptr) {
                out.emplace_back(reply->element[i]->str, reply->element[i]->len);
            }
        }
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return out;
}

long long redis_sync_client::llen(const std::string& key) {
    if (!is_connected()) {
        return 0;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "LLEN %b", key.data(), key.size()));

    long long ret = 0;
    if (reply != nullptr && reply->type == REDIS_REPLY_INTEGER) {
        ret = reply->integer;
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return ret;
}

std::vector<std::string> redis_sync_client::keys(const std::string& pattern) {
    std::vector<std::string> out;
    if (!is_connected()) {
        return out;
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(_ctx, "KEYS %b", pattern.data(), pattern.size()));

    if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY) {
        out.reserve(reply->elements);
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i] != nullptr && reply->element[i]->type == REDIS_REPLY_STRING && reply->element[i]->str != nullptr) {
                out.emplace_back(reply->element[i]->str, reply->element[i]->len);
            }
        }
    }

    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return out;
}

} // namespace seastar::xtrader
