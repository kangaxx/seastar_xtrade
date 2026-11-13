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
    ../../src/xtrader/ctp_spi_bridge.cc
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
    ../../src/xtrader/market_data_paths.cc
    # B3 offline matching is compiled but not connected to a trading entry.
    # Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
    ../../src/xtrader/simulated_order_book.cc
    # B2 offline fund reservations are not connected to the runtime.
    # Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
    ../../src/xtrader/position_ledger.cc
    ../../src/xtrader/offline_fund_ledger.cc
    ../../src/xtrader/offline_account_ledger.cc
    ../../src/xtrader/offline_futures_economics.cc
    ../../src/xtrader/offline_futures_account.cc
    ../../src/xtrader/offline_simulated_execution.cc
    ../../src/xtrader/offline_bar_timeline.cc
    ../../src/xtrader/offline_bar_replay.cc
    ../../src/xtrader/offline_bar_strategy_replay.cc
    ../../src/xtrader/ctp_query_tracker.cc
    ../../src/xtrader/market_data_pipeline.cc
    ../../src/xtrader/market_record_format.cc
    ../../src/xtrader/paper_market_session.cc
    ../../src/xtrader/recovery_coordinator.cc
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
    market_capture_launcher.cc
    ../../src/xtrader/market_data_paths.cc
    ../../src/xtrader/data_importer.cc
    ../../src/xtrader/historical_data_manager.cc
    ../../src/xtrader/redis_sync_client.cc)

target_include_directories(app_xtrader_cli PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(app_xtrader_cli PRIVATE hiredis)

# Read-only collector runs outside the reactor and links MdApi only.
# Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(xtrader_market_capture_core STATIC
    ../../src/xtrader/market_capture_config.cc
    ../../src/xtrader/market_capture.cc
    ../../src/xtrader/market_tick_writer.cc
    ../../src/xtrader/market_data_paths.cc)
target_include_directories(xtrader_market_capture_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(xtrader_market_capture_core SYSTEM PRIVATE ${CTP_API_DIR})
target_link_libraries(xtrader_market_capture_core PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(app_xtrader_market_capture xtrader_market_capture.cc)
target_include_directories(app_xtrader_market_capture SYSTEM PRIVATE ${CTP_API_DIR})
target_link_libraries(app_xtrader_market_capture PRIVATE
    xtrader_market_capture_core "${CTP_API_DIR}/libthostmduserapi_se.so")
set_target_properties(app_xtrader_market_capture PROPERTIES
    OUTPUT_NAME xtrader_market_capture BUILD_RPATH "${CTP_API_DIR}")

add_executable(app_xtrader_market_config xtrader_market_config.cc)
target_link_libraries(app_xtrader_market_config PRIVATE xtrader_market_capture_core)
set_target_properties(app_xtrader_market_config PROPERTIES OUTPUT_NAME xtrader_market_config)
add_dependencies(app_xtrader_cli app_xtrader_market_capture app_xtrader_market_config)
add_dependencies(apps app_xtrader_market_capture app_xtrader_market_config)

if(Seastar_TESTING)
    add_executable(test_unit_xtrader_market_capture EXCLUDE_FROM_ALL
        ../../tests/unit/xtrader_market_capture_test.cc)
    target_include_directories(test_unit_xtrader_market_capture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/xtrader)
    target_include_directories(test_unit_xtrader_market_capture SYSTEM PRIVATE ${CTP_API_DIR})
    target_link_libraries(test_unit_xtrader_market_capture PRIVATE xtrader_market_capture_core)
    add_dependencies(unit_tests test_unit_xtrader_market_capture)
    add_custom_target(test_unit_xtrader_market_capture_run
        COMMAND $<TARGET_FILE:test_unit_xtrader_market_capture>
        DEPENDS test_unit_xtrader_market_capture USES_TERMINAL VERBATIM)
    add_test(NAME Seastar.unit.xtrader_market_capture
        COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR}
            --target test_unit_xtrader_market_capture_run)
    set_tests_properties(Seastar.unit.xtrader_market_capture PROPERTIES TIMEOUT ${Seastar_TEST_TIMEOUT})
endif()

# B1: separate fail-closed entry binaries; no CTP libraries are linked here.
# Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
foreach(entry IN ITEMS replay paper ctp_sim live)
    add_executable(app_xtrader_${entry} xtrader_entry_disabled.cc)
    target_compile_definitions(app_xtrader_${entry} PRIVATE XTRADER_ENTRY_NAME="xtrader_${entry}")
    set_target_properties(app_xtrader_${entry} PROPERTIES OUTPUT_NAME xtrader_${entry})
    add_dependencies(apps app_xtrader_${entry})
endforeach()

if(Seastar_TESTING)
    add_custom_target(test_unit_xtrader_cli_config_safety_run
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:app_xtrader_cli>
            -DFIXTURE_BASE=${CMAKE_CURRENT_BINARY_DIR}
            -P ${Seastar_SOURCE_DIR}/tests/unit/xtrader_cli_config_safety_test.cmake
        DEPENDS app_xtrader_cli
        USES_TERMINAL
        VERBATIM)
    add_test(NAME Seastar.unit.xtrader_cli_config_safety
        COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR}
            --target test_unit_xtrader_cli_config_safety_run)
    set_tests_properties(Seastar.unit.xtrader_cli_config_safety
        PROPERTIES TIMEOUT ${Seastar_TEST_TIMEOUT})

    add_custom_target(test_unit_xtrader_entry_isolation_run
        COMMAND ${CMAKE_COMMAND}
            -DREPLAY=$<TARGET_FILE:app_xtrader_replay>
            -DPAPER=$<TARGET_FILE:app_xtrader_paper>
            -DCTP_SIM=$<TARGET_FILE:app_xtrader_ctp_sim>
            -DLIVE=$<TARGET_FILE:app_xtrader_live>
            -P ${Seastar_SOURCE_DIR}/tests/unit/xtrader_entry_isolation_test.cmake
        DEPENDS app_xtrader_replay app_xtrader_paper app_xtrader_ctp_sim app_xtrader_live
        USES_TERMINAL)
    add_test(NAME Seastar.unit.xtrader_entry_isolation
        COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR}
            --target test_unit_xtrader_entry_isolation_run)
    set_tests_properties(Seastar.unit.xtrader_entry_isolation
        PROPERTIES TIMEOUT ${Seastar_TEST_TIMEOUT})
endif()

# Phase 6 offline probe: deliberately does not link CTP or start a reactor.
# Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
add_executable(app_xtrader_phase6_probe
    ../../tools/xtrader_phase6_probe.cc
    ../../src/xtrader/performance_metrics.cc
    ../../src/xtrader/health_check.cc)
target_include_directories(app_xtrader_phase6_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(app_xtrader_phase6_probe PRIVATE Threads::Threads)
set_target_properties(app_xtrader_phase6_probe PROPERTIES OUTPUT_NAME xtrader_phase6_probe)
add_dependencies(apps app_xtrader_phase6_probe)
