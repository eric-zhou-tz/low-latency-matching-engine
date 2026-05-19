cmake_minimum_required(VERSION 3.20)

project(
    matching_engine
    VERSION 0.2.3
    DESCRIPTION "Modern C++20 matching engine scaffold"
    LANGUAGES CXX
)

include(FetchContent)

FetchContent_Declare(
    unordered_dense
    URL https://github.com/martinus/unordered_dense/archive/refs/tags/v4.8.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(unordered_dense)

add_library(engine
    src/io/parser.cpp
    src/io/script_runner.cpp
    src/book/order_book.cpp
    src/exchange.cpp
)

target_compile_features(engine PUBLIC cxx_std_20)

target_include_directories(engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(engine
    PUBLIC
        unordered_dense::unordered_dense
)

set_target_properties(engine PROPERTIES
    CXX_EXTENSIONS OFF
)

add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE engine)

enable_testing()

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

add_executable(exchange_test tests/exchange_test.cpp)
target_link_libraries(exchange_test PRIVATE engine GTest::gtest_main)
gtest_discover_tests(exchange_test)

add_executable(parser_tests tests/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(parser_tests)

add_executable(golden_replay_tests tests/golden_replay_tests.cpp)
target_link_libraries(golden_replay_tests PRIVATE engine GTest::gtest_main)
target_compile_definitions(golden_replay_tests
    PRIVATE
        REPLAY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/replay"
)
gtest_discover_tests(golden_replay_tests)

add_executable(order_book_invariant_tests tests/invariants/order_book_invariant_tests.cpp)
target_link_libraries(order_book_invariant_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(order_book_invariant_tests)

add_executable(order_book_regression_tests tests/regression/order_book_regression_tests.cpp)
target_link_libraries(order_book_regression_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(order_book_regression_tests)

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

    add_executable(cancel_benchmark benchmark/cancel_benchmark.cpp)
    target_link_libraries(cancel_benchmark
        PRIVATE
            engine
            benchmark::benchmark
            benchmark::benchmark_main
    )

    add_executable(cancel_reserve_sweep_benchmark benchmark/cancel_benchmark.cpp)
    target_compile_definitions(cancel_reserve_sweep_benchmark
        PRIVATE
            MATCHING_ENGINE_ENABLE_RESERVE_SWEEP
    )
    target_link_libraries(cancel_reserve_sweep_benchmark
        PRIVATE
            engine
            benchmark::benchmark
            benchmark::benchmark_main
    )

    add_executable(end_to_end_benchmark benchmark/end_to_end_benchmark.cpp)
    target_link_libraries(end_to_end_benchmark
        PRIVATE
            engine
            benchmark::benchmark
            benchmark::benchmark_main
    )

    add_executable(latency_benchmark benchmark/latency_benchmark.cpp)
    target_link_libraries(latency_benchmark
        PRIVATE
            engine
    )
endif()
