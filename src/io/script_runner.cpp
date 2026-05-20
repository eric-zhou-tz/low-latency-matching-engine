#include "io/script_runner.hpp"

#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"
#include "toy/exchange.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace matching_engine {

/**
 * @brief Parses a model name into the runtime engine selector.
 */
std::optional<EngineModel> parse_engine_model(const std::string& model_name) {
    if (model_name == "fast") {
        return EngineModel::Fast;
    }

    if (model_name == "toy-std") {
        return EngineModel::ToyStd;
    }

    return std::nullopt;
}

/**
 * @brief Runs command text through the public parser/exchange/formatter pipeline.
 */
void run_script(std::istream& input, std::ostream& output) {
    // Preserve the original public helper behavior by defaulting to the fast engine.
    run_script(input, output, EngineModel::Fast);
}

/**
 * @brief Runs command text through the selected parser/exchange/formatter pipeline.
 */
void run_script(std::istream& input, std::ostream& output, EngineModel model) {
    Parser parser;
    Exchange fast_exchange;
    toy::Exchange toy_exchange;
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

        // Route accepted actions through the selected exchange before formatting emitted events.
        if (model == EngineModel::ToyStd) {
            toy_exchange.process(*action, events);
        } else {
            fast_exchange.process(*action, events);
        }

        for (const auto& event : events) {
            output << format_event(event) << '\n';
        }
    }
}

} // namespace matching_engine
