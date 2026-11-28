/*
 * Compile-time strategy registry.
 *
 * 编译期策略注册表：策略通过静态初始化自动注册，菜单据此枚举可选策略。
 * key 为策略类名（strategy_id）。支持注册与注销，注销后菜单不再展示。
 * Source: user request, 2026-11-28.
 */
#pragma once

#include <seastar/xtrader/strategy_base.hh>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace seastar::xtrader {

// 策略工厂：无参构造一个带默认配置的策略实例。
using strategy_factory = std::function<std::shared_ptr<strategy_base>()>;

class strategy_registry {
public:
    static strategy_registry& instance();

    // 注册策略；同名重复注册会覆盖（可用于替换旧策略实现）。
    void register_strategy(const std::string& key, strategy_factory factory);

    // 注销策略；注销后枚举与查找均不再返回，菜单不再展示。
    void unregister_strategy(const std::string& key);

    [[nodiscard]] std::vector<std::string> keys() const;
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] strategy_factory find(const std::string& key) const;

    // 按 key 创建策略实例；不存在返回空。
    [[nodiscard]] std::shared_ptr<strategy_base> create(const std::string& key) const;

private:
    strategy_registry() = default;
    std::map<std::string, strategy_factory> _factories;
};

} // namespace seastar::xtrader
