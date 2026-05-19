#include "io/script_runner.hpp"

#include <iostream>

/**
 * @brief Command-line entry point for the matching engine scaffold.
 *
 * Reads commands from stdin, parses them into typed actions, passes them through
 * the exchange, and prints resulting events. 
 */
int main() {
    // Delegate the stdin/stdout command loop so tests exercise the same public path.
    matching_engine::run_script(std::cin, std::cout);
    return 0;
}
