# 独立业务策略库：策略编译为 OBJECT 库，编译期通过注册表自动注册。
# 使用 OBJECT 库（而非 STATIC）确保所有策略对象都被链接进主程序，
# 避免静态库「未引用对象被丢弃」导致自动注册失效。
# 要剔除某个策略，从下面的源文件列表中移除即可（不编译 = 不注册 = 菜单不展示）。
# Source: user request, 2026-11-28.
add_library(xtrader_strategies OBJECT
    turtle_strategy.cc
    rm_turtle_intraday.cc)

target_include_directories(xtrader_strategies PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xtrader_strategies PUBLIC
    seastar_private)
