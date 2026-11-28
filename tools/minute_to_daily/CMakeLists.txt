cmake_minimum_required(VERSION 3.16)
project(xtrader_minute_to_daily LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(xtrader_minute_to_daily SHARED src/minute_to_daily.cc)
target_include_directories(xtrader_minute_to_daily
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(xtrader_minute_to_daily PRIVATE -Wall -Wextra)
set_target_properties(xtrader_minute_to_daily PROPERTIES
    OUTPUT_NAME xtrader_minute_to_daily
    PUBLIC_HEADER include/xtrader/minute_to_daily.h)

add_executable(minute_to_daily_cli cli/minute_to_daily_cli.cc)
target_link_libraries(minute_to_daily_cli PRIVATE xtrader_minute_to_daily)
set_target_properties(minute_to_daily_cli PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

install(TARGETS xtrader_minute_to_daily
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/xtrader)
install(TARGETS minute_to_daily_cli RUNTIME DESTINATION bin)
