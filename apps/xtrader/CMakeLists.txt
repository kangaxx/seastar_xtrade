find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S" UTC)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.hh.in
    ${CMAKE_SOURCE_DIR}/include/seastar/xtrader/version.hh
    @ONLY)

seastar_add_app(xtrader_demo SOURCES
    xtrader_demo.cc
    ../../src/xtrader/domain_types.cc
    ../../src/xtrader/ctp_gateway.cc
    ../../src/xtrader/ctp_spi_adapter.cc
    ../../src/xtrader/pending_order_manager.cc
    ../../src/xtrader/runtime_engine.cc
    ../../src/xtrader/runtime_sharded.cc
    ../../src/xtrader/strategy_base.cc
    ../../src/xtrader/strategy_lifecycle.cc
    ../../src/xtrader/subscription_registry.cc
    ../../src/xtrader/event_bus.cc
    ../../src/xtrader/order_intent.cc
    ../../src/xtrader/redis_metadata.cc
    ../../src/xtrader/web_api_handler.cc
    ../../src/xtrader/backtest_driver.cc
    ../../src/xtrader/historical_data_manager.cc
    ../../src/xtrader/redis_sync_client.cc
    ../../src/xtrader/performance_metrics.cc
    ../../src/xtrader/health_check.cc)

# CTP API 配置
set(CTP_API_DIR "${CMAKE_SOURCE_DIR}/api/ctp/v6.7.2/linux64")
set(CTP_LIBS
    "${CTP_API_DIR}/libthostmduserapi_se.so"
    "${CTP_API_DIR}/libthosttraderapi_se.so")

target_include_directories(app_xtrader_demo PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CTP_API_DIR})

target_link_libraries(app_xtrader_demo PRIVATE
    ${CTP_LIBS}
    hiredis)

seastar_add_app(xtrader_cli SOURCES
    xtrader_cli.cc
    ../../src/xtrader/data_importer.cc
    ../../src/xtrader/historical_data_manager.cc
    ../../src/xtrader/redis_sync_client.cc)

target_include_directories(app_xtrader_cli PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(app_xtrader_cli PRIVATE hiredis)
