#include "io/script_runner.hpp"

#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace matching_engine {

/**
 * @brief Runs command text through the public parser/exchange/formatter pipeline.
 */
void run_script(std::istream& input, std::ostream& output) {
    Parser parser;
    Exchange exchange;
    std::vector<Event> events;
    events.reserve(8);

    std::string line;
    while (std::getline(input, line)) {
        // Parse exactly one protocol line so malformed commands produce stable CLI output.
        const auto action = parser.parse_line(line);
        if (!action) {
            output << "REJECTED invalid command\n";
            continue;
        }

        // Route accepted actions through the exchange before formatting all emitted events.
        exchange.process(*action, events);
        for (const auto& event : events) {
            output << format_event(event) << '\n';
        }
    }
}

} // namespace matching_engine
