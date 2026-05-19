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
    matching_engine::Parser parser;
    matching_engine::Exchange exchange;
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    std::string line;
    while (std::getline(std::cin, line)) {
        const auto action = parser.parse_line(line);
        if (!action) {
            std::cout << "REJECTED invalid command\n";
            continue;
        }

        exchange.process(*action, events);

        for (const auto& event : events) {
            std::cout << matching_engine::format_event(event) << '\n';
        }
    }

    return 0;
}
