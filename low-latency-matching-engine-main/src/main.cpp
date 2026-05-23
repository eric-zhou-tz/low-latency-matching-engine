#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"

#include <iostream>
#include <string>

/**
 * @brief Command-line entry point for the matching engine scaffold.
 *
 * Reads commands from stdin, parses them into typed actions, passes them through
 * the exchange, and prints resulting events. 
 */
int main() {
    // Build the two application components used by the command loop.
    matching_engine::Parser parser;
    matching_engine::Exchange exchange;

    // Read one command per line so scripts and redirected files work naturally.
    std::string line;
    while (std::getline(std::cin, line)) {
        // Convert text into a typed action before touching exchange state.
        const auto action = parser.parse_line(line);
        if (!action) {
            // Invalid input is reported and skipped without stopping the process.
            std::cout << "REJECTED invalid command\n";
            continue;
        }

        // Process the action and print each event in the order it was produced.
        for (const auto& event : exchange.process(*action)) {
            std::cout << matching_engine::format_event(event) << '\n';
        }
    }

    // Reaching EOF is a normal shutdown path for this command-line tool.
    return 0;
}
