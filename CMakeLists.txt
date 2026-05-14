cmake_minimum_required(VERSION 3.20)

project(
    matching_engine
    VERSION 0.1.0
    DESCRIPTION "Modern C++20 matching engine scaffold"
    LANGUAGES CXX
)

add_library(matching_engine_core
    src/parser.cpp
    src/order_book.cpp
    src/exchange.cpp
)

target_compile_features(matching_engine_core PUBLIC cxx_std_20)

target_include_directories(matching_engine_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(matching_engine_core PROPERTIES
    CXX_EXTENSIONS OFF
)

add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_core)

enable_testing()

add_executable(parser_tests tests/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE matching_engine_core)
add_test(NAME parser_tests COMMAND parser_tests)

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE matching_engine_core)
add_test(NAME order_book_tests COMMAND order_book_tests)
