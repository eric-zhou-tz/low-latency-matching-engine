#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"

#include <iostream>
#include <string>
#include <vector>

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
    std::vector<matching_engine::Event> events;
    events.reserve(8);

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

        // Reuse one event buffer for the whole command loop to avoid allocation churn.
        exchange.process(*action, events);

        // Print each event in the order it was produced.
        for (const auto& event : events) {
            std::cout << matching_engine::format_event(event) << '\n';
        }
    }

    // Reaching EOF is a normal shutdown path for this command-line tool.
    return 0;
}
