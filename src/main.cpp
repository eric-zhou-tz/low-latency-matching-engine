#include "event.hpp"
#include "exchange.hpp"
#include "parser.hpp"

#include <iostream>
#include <string>

/**
 * @brief Command-line entry point for the matching engine scaffold.
 *
 * Reads commands from stdin, parses them into typed actions, passes them through
 * the exchange, and prints resulting events. 
 */
int main() {
    matching_engine::Parser parser;
    matching_engine::Exchange exchange;

    std::string line;
    while (std::getline(std::cin, line)) {
        const auto action = parser.parse_line(line);
        if (!action) {
            std::cout << "REJECTED invalid command\n";
            continue;
        }

        for (const auto& event : exchange.process(*action)) {
            std::cout << matching_engine::format_event(event) << '\n';
        }
    }

    return 0;
}
