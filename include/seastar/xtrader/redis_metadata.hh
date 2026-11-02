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

#include <string>
#include <vector>

namespace seastar::xtrader {

class redis_sync_client;

// === Redis Keyspace Constants ===

namespace redis_keyspace {

inline constexpr const char* dataset_prefix   = "xtrader:dataset:";
inline constexpr const char* replay_prefix    = "xtrader:replay:";
inline constexpr const char* strategy_prefix  = "xtrader:strategy:";
inline constexpr const char* event_prefix     = "xtrader:event:";
inline constexpr const char* validation_prefix = "xtrader:validation:";

inline std::string dataset_key(const std::string& exchange,
                                const std::string& symbol,
                                const std::string& timeframe) {
    return std::string(dataset_prefix) + exchange + ":" + symbol + ":" + timeframe;
}

inline std::string replay_key(const std::string& job_id) {
    return std::string(replay_prefix) + job_id + ":meta";
}

inline std::string strategy_key(const std::string& strategy_id) {
    return std::string(strategy_prefix) + strategy_id + ":state";
}

inline std::string event_key(const std::string& trading_day,
                              const std::string& trace_id) {
    return std::string(event_prefix) + trading_day + ":" + trace_id;
}

inline std::string validation_key(const std::string& env,
                                    const std::string& check_id) {
    return std::string(validation_prefix) + env + ":" + check_id;
}

} // namespace redis_keyspace

// === Dataset Metadata Model ===

struct redis_dataset_meta {
    std::string symbol;
    std::string exchange;
    std::string timeframe;
    std::string data_domain;
    std::string active_file;
    size_t row_count = 0;
    std::string start_datetime;
    std::string end_datetime;
    std::string last_import_mode;
    std::string last_import_time;
    std::string manifest_hash;
    std::string status;
    std::string updated_at;
    std::string source_env;
};

// === Replay Metadata Model ===

struct redis_replay_meta {
    std::string job_id;
    std::string data_source;
    std::string loaded_symbol;
    std::string loaded_exchange;
    std::string loaded_timeframe;
    size_t processed_ticks_or_bars = 0;
    std::string start_time;
    std::string end_time;
    std::string status;
    std::string result;
    std::string updated_at;
    std::string source_env;
};

// === Strategy State Model ===

struct redis_strategy_state {
    std::string strategy_id;
    std::string state;
    std::string subscribed_instruments;
    std::string last_event_time;
    uint64_t signal_count = 0;
    uint64_t order_count = 0;
    uint64_t trade_count = 0;
    uint64_t reject_count = 0;
    int queue_depth = 0;
    std::string trace_id;
    std::string updated_at;
    std::string source_env;
};

// === Validation Result Model ===

struct redis_validation_result {
    std::string env;
    std::string check_id;
    std::string check_type;
    std::string status;
    std::string summary;
    std::string details;
    std::string updated_at;
};

// === Structured Event Model ===

struct redis_event {
    std::string trading_day;
    std::string trace_id;
    std::string strategy_id;
    std::string instrument_id;
    std::string event_type;
    std::string timestamp;
    std::string payload_json;
};

// === Redis Metadata Store ===

class redis_metadata_store {
public:
    explicit redis_metadata_store(redis_sync_client* client,
                                   const std::string& source_env = "vm");
    ~redis_metadata_store() = default;

    bool write_dataset_meta(const redis_dataset_meta& meta);
    bool write_replay_meta(const redis_replay_meta& meta);
    bool write_strategy_state(const redis_strategy_state& state);
    bool write_validation_result(const redis_validation_result& result);
    bool write_event(const redis_event& event);

    bool read_dataset_meta(const std::string& exchange,
                            const std::string& symbol,
                            const std::string& timeframe,
                            redis_dataset_meta& out);
    bool read_replay_meta(const std::string& job_id,
                           redis_replay_meta& out);
    bool read_strategy_state(const std::string& strategy_id,
                              redis_strategy_state& out);
    bool read_validation_result(const std::string& env,
                                 const std::string& check_id,
                                 redis_validation_result& out);

    std::vector<std::string> list_dataset_keys();
    std::vector<std::string> list_strategy_keys();
    std::vector<std::string> keys(const std::string& pattern);

    bool is_connected() const;

private:
    static std::string serialize_dataset_meta(const redis_dataset_meta& meta);
    static std::string serialize_replay_meta(const redis_replay_meta& meta);
    static std::string serialize_strategy_state(const redis_strategy_state& state);
    static std::string serialize_validation_result(const redis_validation_result& result);
    static std::string serialize_event(const redis_event& event);

    static redis_dataset_meta deserialize_dataset_meta(const std::string& raw);
    static redis_replay_meta deserialize_replay_meta(const std::string& raw);
    static redis_strategy_state deserialize_strategy_state(const std::string& raw);
    static redis_validation_result deserialize_validation_result(const std::string& raw);

    redis_sync_client* _client;
    std::string _source_env;
};

} // namespace seastar::xtrader
