cmake_minimum_required(VERSION 3.20)

project(
    matching_engine
    VERSION 0.2.3
    DESCRIPTION "Modern C++20 matching engine scaffold"
    LANGUAGES CXX
)

add_library(engine
    src/parser.cpp
    src/order_book.cpp
    src/exchange.cpp
)

target_compile_features(engine PUBLIC cxx_std_20)

target_include_directories(engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(engine PROPERTIES
    CXX_EXTENSIONS OFF
)

add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE engine)

enable_testing()

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(order_book_test tests/order_book_test.cpp)
target_link_libraries(order_book_test PRIVATE engine GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(order_book_test)

add_executable(parser_tests tests/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(parser_tests)

if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Release")
    FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(insert_benchmark benchmark/insert_benchmark.cpp)
    target_link_libraries(insert_benchmark
        PRIVATE
            engine
            benchmark::benchmark
            benchmark::benchmark_main
    )

    add_executable(match_benchmark benchmark/match_benchmark.cpp)
    target_link_libraries(match_benchmark
        PRIVATE
            engine
            benchmark::benchmark
            benchmark::benchmark_main
    )
endif()
