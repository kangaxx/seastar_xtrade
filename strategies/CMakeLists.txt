# 独立业务策略库：业务策略与策略框架/回测引擎解耦，可被回测或实盘入口复用。
# Source: user request, 2026-11-27.
add_library(xtrader_strategies STATIC
    turtle_strategy.cc)

target_include_directories(xtrader_strategies PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xtrader_strategies PUBLIC
    seastar_private)
