cmake_minimum_required(VERSION 3.20)

project(
    matching_engine
    VERSION 0.7.2
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
    src/io/cli_presentation.cpp
    src/io/local_benchmark_comparison.cpp
    src/io/parser.cpp
    src/io/script_runner.cpp
    src/book/order_book.cpp
    src/exchange.cpp
    toy/order_book.cpp
    toy/exchange.cpp
)

target_compile_features(engine PUBLIC cxx_std_20)

target_include_directories(engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}
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

add_executable(cli_presentation_tests tests/cli_presentation_tests.cpp)
target_link_libraries(cli_presentation_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(cli_presentation_tests)

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

add_executable(order_book_stress_tests tests/stress/order_book_stress_tests.cpp)
target_link_libraries(order_book_stress_tests PRIVATE engine GTest::gtest_main)
gtest_discover_tests(order_book_stress_tests)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

function(link_google_benchmark target_name)
    target_link_libraries(${target_name}
        PRIVATE
            engine
            benchmark::benchmark_main
    )
endfunction()

add_executable(core_hot_path_benchmark
    benchmarks/core_hot_path/passive_insert_benchmark.cpp
    benchmarks/core_hot_path/one_level_crossing_benchmark.cpp
    benchmarks/core_hot_path/cancel_lookup_benchmark.cpp
)
link_google_benchmark(core_hot_path_benchmark)

add_executable(core_hot_path_latency_benchmark
    benchmarks/core_hot_path/batch_latency_benchmark.cpp
)
target_include_directories(core_hot_path_latency_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/benchmarks/realistic_flow
)
target_link_libraries(core_hot_path_latency_benchmark PRIVATE engine)

add_executable(realistic_flow_benchmark
    benchmarks/realistic_flow/true_mixed_benchmark.cpp
    benchmarks/realistic_flow/end_to_end_true_mixed_benchmark.cpp
)
target_include_directories(realistic_flow_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/benchmarks/realistic_flow
)
link_google_benchmark(realistic_flow_benchmark)

add_executable(std_toy_comparison_benchmark
    benchmarks/std_toy_comparison/std_toy_comparison_benchmark.cpp
)
target_include_directories(std_toy_comparison_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/benchmarks/realistic_flow
)
link_google_benchmark(std_toy_comparison_benchmark)

add_executable(stress_benchmark
    benchmarks/stress/best_level_churn_benchmark.cpp
    benchmarks/stress/level_create_delete_churn_benchmark.cpp
    benchmarks/stress/shallow_gtc_mixed_benchmark.cpp
    benchmarks/stress/deep_sparse_gtc_mixed_benchmark.cpp
)
link_google_benchmark(stress_benchmark)

add_executable(determinism_replay_benchmark
    benchmarks/determinism_replay/replay_benchmark.cpp
)
target_compile_definitions(determinism_replay_benchmark
    PRIVATE
        REPLAY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/replay"
)
link_google_benchmark(determinism_replay_benchmark)

add_executable(experimental_reserve_sweep_benchmark
    benchmarks/core_hot_path/cancel_lookup_benchmark.cpp
)
target_compile_definitions(experimental_reserve_sweep_benchmark
    PRIVATE
        MATCHING_ENGINE_ENABLE_RESERVE_SWEEP
)
link_google_benchmark(experimental_reserve_sweep_benchmark)
