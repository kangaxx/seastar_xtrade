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
#include <seastar/http/reply.hh>
#include <seastar/xtrader/redis_metadata.hh>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace seastar {

namespace httpd {
class http_server;
}

namespace xtrader {

class strategy_lifecycle_manager;

struct web_api_config {
    sstring listen_addr = "0.0.0.0";
    uint16_t port = 8765;
    sstring source_env = "vm";
};

struct dataset_view {
    sstring symbol;
    sstring exchange;
    sstring timeframe;
    sstring data_domain;
    sstring active_file;
    size_t row_count = 0;
    sstring start_datetime;
    sstring end_datetime;
    sstring manifest_hash;
    sstring status;
    sstring updated_at;
    sstring source_env;
};

struct strategy_view {
    sstring strategy_id;
    sstring state;
    sstring subscribed_instruments;
    sstring last_event_time;
    uint64_t signal_count = 0;
    uint64_t order_count = 0;
    uint64_t trade_count = 0;
    uint64_t reject_count = 0;
    int queue_depth = 0;
    sstring trace_id;
    sstring updated_at;
    sstring source_env;
};

struct replay_view {
    sstring job_id;
    sstring data_source;
    sstring loaded_symbol;
    sstring loaded_exchange;
    sstring loaded_timeframe;
    size_t processed_ticks_or_bars = 0;
    sstring status;
    sstring result;
    sstring updated_at;
    sstring source_env;
};

struct validation_view {
    sstring env;
    sstring check_id;
    sstring check_type;
    sstring status;
    sstring summary;
    sstring details;
    sstring updated_at;
};

dataset_view to_view(const redis_dataset_meta& m);
strategy_view to_view(const redis_strategy_state& s);
replay_view to_view(const redis_replay_meta& m);
validation_view to_view(const redis_validation_result& v);

sstring dataset_to_json(const dataset_view& v);
sstring strategy_to_json(const strategy_view& v);
sstring replay_to_json(const replay_view& v);
sstring validation_to_json(const validation_view& v);

sstring datasets_list_to_json(const std::vector<dataset_view>& list);
sstring strategies_list_to_json(const std::vector<strategy_view>& list);

class web_api_handler {
public:
    web_api_handler(redis_metadata_store* redis_store,
                     strategy_lifecycle_manager* strategy_mgr,
                     const web_api_config& cfg = {});
    ~web_api_handler();

    future<> start();
    future<> stop();

private:
    future<std::unique_ptr<http::reply>> handle_get_datasets(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);
    future<std::unique_ptr<http::reply>> handle_get_dataset(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);
    future<std::unique_ptr<http::reply>> handle_get_strategies(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);
    future<std::unique_ptr<http::reply>> handle_get_strategy(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);
    future<std::unique_ptr<http::reply>> handle_get_replay(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);
    future<std::unique_ptr<http::reply>> handle_get_validations(
        std::unique_ptr<http::request> req,
        std::unique_ptr<http::reply> rep);

    redis_metadata_store* _redis_store;
    strategy_lifecycle_manager* _strategy_mgr;
    web_api_config _cfg;
    std::unique_ptr<httpd::http_server> _server;
    bool _started = false;
};

} // namespace xtrader
} // namespace seastar
