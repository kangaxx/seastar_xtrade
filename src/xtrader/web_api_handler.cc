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
#include <seastar/xtrader/web_api_handler.hh>
#include <seastar/xtrader/strategy_lifecycle.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/json_path.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/net/api.hh>

#include <chrono>
#include <iostream>
#include <sstream>

namespace seastar::xtrader {

namespace {

sstring json_escape(const sstring& s) {
    sstring r;
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:   r.push_back(c); break;
        }
    }
    return r;
}

sstring json_kv(const sstring& key, const sstring& val) {
    return "\"" + key + "\":\"" + json_escape(val) + "\"";
}

sstring json_kv(const sstring& key, uint64_t val) {
    return "\"" + key + "\":" + to_sstring(val);
}

sstring json_kv(const sstring& key, int val) {
    return "\"" + key + "\":" + to_sstring(val);
}

sstring json_kv(const sstring& key, size_t val) {
    return "\"" + key + "\":" + to_sstring(val);
}

sstring json_kv(const sstring& key, bool val) {
    return "\"" + key + "\":" + sstring(val ? "true" : "false");
}

} // anonymous namespace

// ==================== View Conversions ====================

dataset_view to_view(const redis_dataset_meta& m) {
    return {
        .symbol = m.symbol,
        .exchange = m.exchange,
        .timeframe = m.timeframe,
        .data_domain = m.data_domain,
        .active_file = m.active_file,
        .row_count = m.row_count,
        .start_datetime = m.start_datetime,
        .end_datetime = m.end_datetime,
        .manifest_hash = m.manifest_hash,
        .status = m.status,
        .updated_at = m.updated_at,
        .source_env = m.source_env,
    };
}

strategy_view to_view(const redis_strategy_state& s) {
    return {
        .strategy_id = s.strategy_id,
        .state = s.state,
        .subscribed_instruments = s.subscribed_instruments,
        .last_event_time = s.last_event_time,
        .signal_count = s.signal_count,
        .order_count = s.order_count,
        .trade_count = s.trade_count,
        .reject_count = s.reject_count,
        .queue_depth = s.queue_depth,
        .trace_id = s.trace_id,
        .updated_at = s.updated_at,
        .source_env = s.source_env,
    };
}

replay_view to_view(const redis_replay_meta& m) {
    return {
        .job_id = m.job_id,
        .data_source = m.data_source,
        .loaded_symbol = m.loaded_symbol,
        .loaded_exchange = m.loaded_exchange,
        .loaded_timeframe = m.loaded_timeframe,
        .processed_ticks_or_bars = m.processed_ticks_or_bars,
        .status = m.status,
        .result = m.result,
        .updated_at = m.updated_at,
        .source_env = m.source_env,
    };
}

validation_view to_view(const redis_validation_result& v) {
    return {
        .env = v.env,
        .check_id = v.check_id,
        .check_type = v.check_type,
        .status = v.status,
        .summary = v.summary,
        .details = v.details,
        .updated_at = v.updated_at,
    };
}

// ==================== JSON Serialization ====================

sstring dataset_to_json(const dataset_view& v) {
    return "{" +
        json_kv("symbol", v.symbol) + "," +
        json_kv("exchange", v.exchange) + "," +
        json_kv("timeframe", v.timeframe) + "," +
        json_kv("data_domain", v.data_domain) + "," +
        json_kv("active_file", v.active_file) + "," +
        json_kv("row_count", v.row_count) + "," +
        json_kv("start_datetime", v.start_datetime) + "," +
        json_kv("end_datetime", v.end_datetime) + "," +
        json_kv("manifest_hash", v.manifest_hash) + "," +
        json_kv("status", v.status) + "," +
        json_kv("updated_at", v.updated_at) + "," +
        json_kv("source_env", v.source_env) +
        "}";
}

sstring strategy_to_json(const strategy_view& v) {
    return "{" +
        json_kv("strategy_id", v.strategy_id) + "," +
        json_kv("state", v.state) + "," +
        json_kv("subscribed_instruments", v.subscribed_instruments) + "," +
        json_kv("last_event_time", v.last_event_time) + "," +
        json_kv("signal_count", v.signal_count) + "," +
        json_kv("order_count", v.order_count) + "," +
        json_kv("trade_count", v.trade_count) + "," +
        json_kv("reject_count", v.reject_count) + "," +
        json_kv("queue_depth", v.queue_depth) + "," +
        json_kv("trace_id", v.trace_id) + "," +
        json_kv("updated_at", v.updated_at) + "," +
        json_kv("source_env", v.source_env) +
        "}";
}

sstring replay_to_json(const replay_view& v) {
    return "{" +
        json_kv("job_id", v.job_id) + "," +
        json_kv("data_source", v.data_source) + "," +
        json_kv("loaded_symbol", v.loaded_symbol) + "," +
        json_kv("loaded_exchange", v.loaded_exchange) + "," +
        json_kv("loaded_timeframe", v.loaded_timeframe) + "," +
        json_kv("processed_ticks_or_bars", v.processed_ticks_or_bars) + "," +
        json_kv("status", v.status) + "," +
        json_kv("result", v.result) + "," +
        json_kv("updated_at", v.updated_at) + "," +
        json_kv("source_env", v.source_env) +
        "}";
}

sstring validation_to_json(const validation_view& v) {
    return "{" +
        json_kv("env", v.env) + "," +
        json_kv("check_id", v.check_id) + "," +
        json_kv("check_type", v.check_type) + "," +
        json_kv("status", v.status) + "," +
        json_kv("summary", v.summary) + "," +
        json_kv("details", v.details) + "," +
        json_kv("updated_at", v.updated_at) +
        "}";
}

sstring datasets_list_to_json(const std::vector<dataset_view>& list) {
    sstring result = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) result += ",";
        result += dataset_to_json(list[i]);
    }
    result += "]";
    return result;
}

sstring strategies_list_to_json(const std::vector<strategy_view>& list) {
    sstring result = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) result += ",";
        result += strategy_to_json(list[i]);
    }
    result += "]";
    return result;
}

// ==================== web_api_handler ====================

web_api_handler::web_api_handler(redis_metadata_store* redis_store,
                                   strategy_lifecycle_manager* strategy_mgr,
                                   const web_api_config& cfg)
    : _redis_store(redis_store)
    , _strategy_mgr(strategy_mgr)
    , _cfg(cfg)
{}

web_api_handler::~web_api_handler() = default;

future<> web_api_handler::start() {
    if (_started) return make_ready_future<>();

    _server = std::make_unique<httpd::http_server>();

    _server->set_routes([this](httpd::routes& r) {
        using namespace httpd;

        r.add(operation_type::GET, url("/api/datasets"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_datasets(std::move(req), std::move(rep));
            }, "json"));

        r.add(operation_type::GET, url("/api/datasets/:exchange/:symbol/:timeframe"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_dataset(std::move(req), std::move(rep));
            }, "json"));

        r.add(operation_type::GET, url("/api/strategies"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_strategies(std::move(req), std::move(rep));
            }, "json"));

        r.add(operation_type::GET, url("/api/strategies/:strategy_id"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_strategy(std::move(req), std::move(rep));
            }, "json"));

        r.add(operation_type::GET, url("/api/replays/:job_id"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_replay(std::move(req), std::move(rep));
            }, "json"));

        r.add(operation_type::GET, url("/api/validations/:env"),
            new function_handler([this](std::unique_ptr<http::request> req,
                                         std::unique_ptr<http::reply> rep) {
                return handle_get_validations(std::move(req), std::move(rep));
            }, "json"));
    });

    return _server->listen(ipv4_addr{_cfg.listen_addr, _cfg.port}).then([this] {
        _started = true;
        std::cout << "[INFO] web_api_handler: listening on "
                  << _cfg.listen_addr << ":" << _cfg.port << std::endl;
    });
}

future<> web_api_handler::stop() {
    if (!_started) return make_ready_future<>();
    _started = false;
    if (_server) {
        return _server->stop().then([this] {
            _server.reset();
        });
    }
    return make_ready_future<>();
}

// === HTTP Handlers ===

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_datasets(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_status(http::reply::status_type::ok);
    rep->set_content_type("application/json");

    std::vector<dataset_view> list;

    if (_redis_store && _redis_store->is_connected()) {
        auto keys = _redis_store->list_dataset_keys();
        for (const auto& key : keys) {
            redis_dataset_meta meta;
            auto marker = sstring(redis_keyspace::dataset_prefix);
            auto pos = key.find(marker);
            if (pos != 0) continue;
            auto remainder = key.substr(marker.size());
            auto first_colon = remainder.find(':');
            auto second_colon = remainder.find(':', first_colon + 1);
            if (first_colon == sstring::npos || second_colon == sstring::npos) continue;
            sstring exchange  = remainder.substr(0, first_colon);
            sstring symbol    = remainder.substr(first_colon + 1, second_colon - first_colon - 1);
            sstring timeframe = remainder.substr(second_colon + 1);
            if (_redis_store->read_dataset_meta(exchange, symbol, timeframe, meta)) {
                list.push_back(to_view(meta));
            }
        }
    }

    rep->_content = datasets_list_to_json(list);
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_dataset(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_content_type("application/json");

    auto& params = req->param;
    sstring exchange  = params["exchange"];
    sstring symbol    = params["symbol"];
    sstring timeframe = params["timeframe"];

    redis_dataset_meta meta;
    bool found = _redis_store && _redis_store->is_connected()
        && _redis_store->read_dataset_meta(exchange, symbol, timeframe, meta);

    if (found) {
        rep->set_status(http::reply::status_type::ok);
        rep->_content = dataset_to_json(to_view(meta));
    } else {
        rep->set_status(http::reply::status_type::not_found);
        rep->_content = "{\"error\":\"not found\"}";
    }
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_strategies(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_status(http::reply::status_type::ok);
    rep->set_content_type("application/json");

    std::vector<strategy_view> list;

    if (_redis_store && _redis_store->is_connected()) {
        auto keys = _redis_store->list_strategy_keys();
        for (const auto& key : keys) {
            redis_strategy_state state;
            auto marker = sstring(redis_keyspace::strategy_prefix);
            auto strategy_id = key;
            if (strategy_id.find(marker) == 0) {
                strategy_id = strategy_id.substr(marker.size());
            }
            auto idx = strategy_id.find(":state");
            if (idx != sstring::npos) {
                strategy_id = strategy_id.substr(0, idx);
            }
            if (_redis_store->read_strategy_state(strategy_id, state)) {
                list.push_back(to_view(state));
            }
        }
    }

    if (_strategy_mgr) {
        for (const auto& [id, strategy] : _strategy_mgr->all_strategies()) {
            bool already = false;
            for (const auto& v : list) {
                if (v.strategy_id == id) { already = true; break; }
            }
            if (already) continue;

            strategy_view v;
            v.strategy_id = id;
            v.state = to_string(strategy->state());
            for (const auto& inst : strategy->config().subscribed_instruments) {
                if (!v.subscribed_instruments.empty())
                    v.subscribed_instruments += ",";
                v.subscribed_instruments += inst;
            }
            v.signal_count = strategy->stats().signal_count;
            v.order_count = strategy->stats().order_count;
            v.trade_count = strategy->stats().trade_count;
            v.reject_count = strategy->stats().reject_count;
            v.queue_depth = strategy->stats().queue_depth;
            v.updated_at = "memory";
            v.source_env = _cfg.source_env;
            list.push_back(v);
        }
    }

    rep->_content = strategies_list_to_json(list);
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_strategy(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_content_type("application/json");

    auto& params = req->param;
    sstring strategy_id = params["strategy_id"];

    redis_strategy_state state;
    bool found = _redis_store && _redis_store->is_connected()
        && _redis_store->read_strategy_state(strategy_id, state);

    if (found) {
        rep->set_status(http::reply::status_type::ok);
        rep->_content = strategy_to_json(to_view(state));
    } else if (_strategy_mgr) {
        auto strategy = _strategy_mgr->get_strategy(strategy_id);
        if (strategy) {
            rep->set_status(http::reply::status_type::ok);
            strategy_view v;
            v.strategy_id = strategy_id;
            v.state = to_string(strategy->state());
            for (const auto& inst : strategy->config().subscribed_instruments) {
                if (!v.subscribed_instruments.empty())
                    v.subscribed_instruments += ",";
                v.subscribed_instruments += inst;
            }
            v.signal_count = strategy->stats().signal_count;
            v.order_count = strategy->stats().order_count;
            v.trade_count = strategy->stats().trade_count;
            v.reject_count = strategy->stats().reject_count;
            v.queue_depth = strategy->stats().queue_depth;
            v.updated_at = "memory";
            v.source_env = _cfg.source_env;
            rep->_content = strategy_to_json(v);
        } else {
            rep->set_status(http::reply::status_type::not_found);
            rep->_content = "{\"error\":\"not found\"}";
        }
    } else {
        rep->set_status(http::reply::status_type::not_found);
        rep->_content = "{\"error\":\"not found\"}";
    }
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_replay(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_content_type("application/json");

    auto& params = req->param;
    sstring job_id = params["job_id"];

    if (_redis_store && _redis_store->is_connected()) {
        redis_replay_meta meta;
        if (_redis_store->read_replay_meta(job_id, meta)) {
            rep->set_status(http::reply::status_type::ok);
            rep->_content = replay_to_json(to_view(meta));
            return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
        }
    }

    rep->set_status(http::reply::status_type::not_found);
    rep->_content = "{\"error\":\"not found\"}";
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

future<std::unique_ptr<http::reply>> web_api_handler::handle_get_validations(
    std::unique_ptr<http::request> req,
    std::unique_ptr<http::reply> rep) {
    rep->set_status(http::reply::status_type::ok);
    rep->set_content_type("application/json");

    auto& params = req->param;
    sstring env = params["env"];

    std::vector<validation_view> list;
    if (_redis_store && _redis_store->is_connected()) {
        auto keys = _redis_store->keys(std::string(redis_keyspace::validation_prefix) + env + ":*");
        for (const auto& key : keys) {
            redis_validation_result result;
            auto marker = sstring(redis_keyspace::validation_prefix);
            std::string kv = key;
            if (kv.find(marker) == 0) {
                kv = kv.substr(marker.size());
            }
            auto sep = kv.find(':');
            if (sep != std::string::npos) {
                std::string check_id = kv.substr(sep + 1);
                if (_redis_store->read_validation_result(env, check_id, result)) {
                    list.push_back(to_view(result));
                }
            }
        }
    }

    sstring body = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) body += ",";
        body += validation_to_json(list[i]);
    }
    body += "]";
    rep->_content = body;
    return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
}

} // namespace seastar::xtrader
