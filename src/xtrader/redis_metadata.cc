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
#include <seastar/xtrader/redis_metadata.hh>
#include <seastar/xtrader/redis_sync_client.hh>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace seastar::xtrader {

namespace {

std::string current_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string escape_csv(const std::string& s) {
    if (s.find(',') != std::string::npos
        || s.find('\"') != std::string::npos
        || s.find('\n') != std::string::npos) {
        std::string escaped;
        escaped.push_back('\"');
        for (char c : s) {
            if (c == '\"') escaped += "\"\"";
            else escaped.push_back(c);
        }
        escaped.push_back('\"');
        return escaped;
    }
    return s;
}

std::string unescape_csv(const std::string& s) {
    if (s.size() >= 2 && s.front() == '\"' && s.back() == '\"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '\"' && i + 1 < line.size() && line[i + 1] == '\"') {
                field.push_back('\"');
                ++i;
            } else if (c == '\"') {
                in_quotes = false;
            } else {
                field.push_back(c);
            }
        } else {
            if (c == '\"') {
                in_quotes = true;
            } else if (c == ',') {
                fields.push_back(field);
                field.clear();
            } else {
                field.push_back(c);
            }
        }
    }
    fields.push_back(field);
    return fields;
}

std::string join_csv(const std::vector<std::string>& fields) {
    std::ostringstream oss;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) oss << ',';
        oss << escape_csv(fields[i]);
    }
    return oss.str();
}

} // anonymous namespace

// ==================== redis_metadata_store ====================

redis_metadata_store::redis_metadata_store(redis_sync_client* client,
                                             const std::string& source_env)
    : _client(client)
    , _source_env(source_env)
{}

bool redis_metadata_store::is_connected() const {
    return _client && _client->is_connected();
}

// === Write Operations ===

bool redis_metadata_store::write_dataset_meta(const redis_dataset_meta& meta) {
    if (!_client || !_client->is_connected()) {
        std::cerr << "[WARN] redis_metadata_store: not connected, skip write_dataset_meta"
                  << std::endl;
        return false;
    }
    auto key = redis_keyspace::dataset_key(meta.exchange, meta.symbol, meta.timeframe);
    auto value = serialize_dataset_meta(meta);
    return _client->set(key, value);
}

bool redis_metadata_store::write_replay_meta(const redis_replay_meta& meta) {
    if (!_client || !_client->is_connected()) {
        std::cerr << "[WARN] redis_metadata_store: not connected, skip write_replay_meta"
                  << std::endl;
        return false;
    }
    auto key = redis_keyspace::replay_key(meta.job_id);
    auto value = serialize_replay_meta(meta);
    return _client->set(key, value);
}

bool redis_metadata_store::write_strategy_state(const redis_strategy_state& state) {
    if (!_client || !_client->is_connected()) {
        std::cerr << "[WARN] redis_metadata_store: not connected, skip write_strategy_state"
                  << std::endl;
        return false;
    }
    auto key = redis_keyspace::strategy_key(state.strategy_id);
    auto value = serialize_strategy_state(state);
    return _client->set(key, value);
}

bool redis_metadata_store::write_validation_result(
    const redis_validation_result& result) {
    if (!_client || !_client->is_connected()) {
        std::cerr << "[WARN] redis_metadata_store: not connected, skip write_validation_result"
                  << std::endl;
        return false;
    }
    auto key = redis_keyspace::validation_key(result.env, result.check_id);
    auto value = serialize_validation_result(result);
    return _client->set(key, value);
}

bool redis_metadata_store::write_event(const redis_event& event) {
    if (!_client || !_client->is_connected()) {
        std::cerr << "[WARN] redis_metadata_store: not connected, skip write_event"
                  << std::endl;
        return false;
    }
    auto key = redis_keyspace::event_key(event.trading_day, event.trace_id);
    auto value = serialize_event(event);
    return _client->set(key, value);
}

// === Read Operations ===

bool redis_metadata_store::read_dataset_meta(const std::string& exchange,
                                              const std::string& symbol,
                                              const std::string& timeframe,
                                              redis_dataset_meta& out) {
    if (!_client || !_client->is_connected()) return false;
    auto key = redis_keyspace::dataset_key(exchange, symbol, timeframe);
    auto raw = _client->get(key);
    if (raw.empty()) return false;
    out = deserialize_dataset_meta(raw);
    return true;
}

bool redis_metadata_store::read_replay_meta(const std::string& job_id,
                                              redis_replay_meta& out) {
    if (!_client || !_client->is_connected()) return false;
    auto key = redis_keyspace::replay_key(job_id);
    auto raw = _client->get(key);
    if (raw.empty()) return false;
    out = deserialize_replay_meta(raw);
    return true;
}

bool redis_metadata_store::read_strategy_state(const std::string& strategy_id,
                                                redis_strategy_state& out) {
    if (!_client || !_client->is_connected()) return false;
    auto key = redis_keyspace::strategy_key(strategy_id);
    auto raw = _client->get(key);
    if (raw.empty()) return false;
    out = deserialize_strategy_state(raw);
    return true;
}

bool redis_metadata_store::read_validation_result(const std::string& env,
                                                   const std::string& check_id,
                                                   redis_validation_result& out) {
    if (!_client || !_client->is_connected()) return false;
    auto key = redis_keyspace::validation_key(env, check_id);
    auto raw = _client->get(key);
    if (raw.empty()) return false;
    out = deserialize_validation_result(raw);
    return true;
}

std::vector<std::string> redis_metadata_store::list_dataset_keys() {
    if (!_client || !_client->is_connected()) return {};
    return _client->keys(std::string(redis_keyspace::dataset_prefix) + "*");
}

std::vector<std::string> redis_metadata_store::list_strategy_keys() {
    if (!_client || !_client->is_connected()) return {};
    return _client->keys(std::string(redis_keyspace::strategy_prefix) + "*");
}

std::vector<std::string> redis_metadata_store::keys(const std::string& pattern) {
    if (!_client || !_client->is_connected()) return {};
    return _client->keys(pattern);
}

// === Serialization ===

std::string redis_metadata_store::serialize_dataset_meta(
    const redis_dataset_meta& meta) {
    auto now = current_iso8601();
    return join_csv({
        meta.symbol, meta.exchange, meta.timeframe, meta.data_domain,
        meta.active_file, std::to_string(meta.row_count),
        meta.start_datetime, meta.end_datetime,
        meta.last_import_mode, meta.last_import_time,
        meta.manifest_hash, meta.status,
        meta.updated_at.empty() ? now : meta.updated_at,
        meta.source_env.empty() ? "vm" : meta.source_env,
    });
}

std::string redis_metadata_store::serialize_replay_meta(
    const redis_replay_meta& meta) {
    auto now = current_iso8601();
    return join_csv({
        meta.job_id, meta.data_source, meta.loaded_symbol,
        meta.loaded_exchange, meta.loaded_timeframe,
        std::to_string(meta.processed_ticks_or_bars),
        meta.start_time, meta.end_time, meta.status, meta.result,
        meta.updated_at.empty() ? now : meta.updated_at,
        meta.source_env.empty() ? "vm" : meta.source_env,
    });
}

std::string redis_metadata_store::serialize_strategy_state(
    const redis_strategy_state& state) {
    auto now = current_iso8601();
    return join_csv({
        state.strategy_id, state.state, state.subscribed_instruments,
        state.last_event_time,
        std::to_string(state.signal_count),
        std::to_string(state.order_count),
        std::to_string(state.trade_count),
        std::to_string(state.reject_count),
        std::to_string(state.queue_depth),
        state.trace_id,
        state.updated_at.empty() ? now : state.updated_at,
        state.source_env.empty() ? "vm" : state.source_env,
    });
}

std::string redis_metadata_store::serialize_validation_result(
    const redis_validation_result& result) {
    auto now = current_iso8601();
    return join_csv({
        result.env, result.check_id, result.check_type,
        result.status, result.summary, result.details,
        result.updated_at.empty() ? now : result.updated_at,
    });
}

std::string redis_metadata_store::serialize_event(const redis_event& event) {
    return join_csv({
        event.trading_day, event.trace_id, event.strategy_id,
        event.instrument_id, event.event_type, event.timestamp,
        event.payload_json,
    });
}

redis_dataset_meta redis_metadata_store::deserialize_dataset_meta(
    const std::string& raw) {
    auto f = split_csv_line(raw);
    redis_dataset_meta m;
    if (f.size() >= 1) m.symbol          = f[0];
    if (f.size() >= 2) m.exchange        = f[1];
    if (f.size() >= 3) m.timeframe       = f[2];
    if (f.size() >= 4) m.data_domain     = f[3];
    if (f.size() >= 5) m.active_file     = f[4];
    if (f.size() >= 6) m.row_count       = static_cast<size_t>(std::stoull(f[5]));
    if (f.size() >= 7) m.start_datetime  = f[6];
    if (f.size() >= 8) m.end_datetime    = f[7];
    if (f.size() >= 9) m.last_import_mode = f[8];
    if (f.size() >=10) m.last_import_time = f[9];
    if (f.size() >=11) m.manifest_hash   = f[10];
    if (f.size() >=12) m.status          = f[11];
    if (f.size() >=13) m.updated_at      = f[12];
    if (f.size() >=14) m.source_env      = f[13];
    return m;
}

redis_replay_meta redis_metadata_store::deserialize_replay_meta(
    const std::string& raw) {
    auto f = split_csv_line(raw);
    redis_replay_meta m;
    if (f.size() >= 1)  m.job_id                  = f[0];
    if (f.size() >= 2)  m.data_source             = f[1];
    if (f.size() >= 3)  m.loaded_symbol           = f[2];
    if (f.size() >= 4)  m.loaded_exchange         = f[3];
    if (f.size() >= 5)  m.loaded_timeframe        = f[4];
    if (f.size() >= 6)  m.processed_ticks_or_bars = std::stoull(f[5]);
    if (f.size() >= 7)  m.start_time              = f[6];
    if (f.size() >= 8)  m.end_time                = f[7];
    if (f.size() >= 9)  m.status                  = f[8];
    if (f.size() >=10)  m.result                  = f[9];
    if (f.size() >=11)  m.updated_at              = f[10];
    if (f.size() >=12)  m.source_env              = f[11];
    return m;
}

redis_strategy_state redis_metadata_store::deserialize_strategy_state(
    const std::string& raw) {
    auto f = split_csv_line(raw);
    redis_strategy_state s;
    if (f.size() >= 1)  s.strategy_id             = f[0];
    if (f.size() >= 2)  s.state                   = f[1];
    if (f.size() >= 3)  s.subscribed_instruments   = f[2];
    if (f.size() >= 4)  s.last_event_time          = f[3];
    if (f.size() >= 5)  s.signal_count             = std::stoull(f[4]);
    if (f.size() >= 6)  s.order_count              = std::stoull(f[5]);
    if (f.size() >= 7)  s.trade_count              = std::stoull(f[6]);
    if (f.size() >= 8)  s.reject_count             = std::stoull(f[7]);
    if (f.size() >= 9)  s.queue_depth              = std::stoi(f[8]);
    if (f.size() >=10)  s.trace_id                 = f[9];
    if (f.size() >=11)  s.updated_at               = f[10];
    if (f.size() >=12)  s.source_env               = f[11];
    return s;
}

redis_validation_result redis_metadata_store::deserialize_validation_result(
    const std::string& raw) {
    auto f = split_csv_line(raw);
    redis_validation_result v;
    if (f.size() >= 1) v.env        = f[0];
    if (f.size() >= 2) v.check_id   = f[1];
    if (f.size() >= 3) v.check_type = f[2];
    if (f.size() >= 4) v.status     = f[3];
    if (f.size() >= 5) v.summary    = f[4];
    if (f.size() >= 6) v.details    = f[5];
    if (f.size() >= 7) v.updated_at = f[6];
    return v;
}

} // namespace seastar::xtrader
