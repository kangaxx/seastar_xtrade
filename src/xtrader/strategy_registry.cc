/*
 * Compile-time strategy registry implementation.
 * Source: user request, 2026-11-28.
 */
#include <seastar/xtrader/strategy_registry.hh>

namespace seastar::xtrader {

strategy_registry& strategy_registry::instance() {
    static strategy_registry registry;
    return registry;
}

void strategy_registry::register_strategy(const std::string& key, strategy_factory factory) {
    if (key.empty() || !factory) {
        return;
    }
    _factories[key] = std::move(factory);
}

void strategy_registry::unregister_strategy(const std::string& key) {
    _factories.erase(key);
}

std::vector<std::string> strategy_registry::keys() const {
    std::vector<std::string> result;
    result.reserve(_factories.size());
    for (const auto& entry : _factories) {
        result.push_back(entry.first);
    }
    return result;
}

bool strategy_registry::contains(const std::string& key) const {
    return _factories.find(key) != _factories.end();
}

strategy_factory strategy_registry::find(const std::string& key) const {
    const auto it = _factories.find(key);
    if (it == _factories.end()) {
        return {};
    }
    return it->second;
}

std::shared_ptr<strategy_base> strategy_registry::create(const std::string& key) const {
    const auto factory = find(key);
    if (!factory) {
        return {};
    }
    return factory();
}

} // namespace seastar::xtrader
