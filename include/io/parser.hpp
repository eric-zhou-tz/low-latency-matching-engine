#pragma once

#include "core/action.hpp"

#include <optional>
#include <string>

namespace matching_engine {

/**
 * @brief Converts text commands into typed exchange actions.
 *
 * The parser is deliberately small and forgiving for the scaffold. Future work
 * can replace this with a richer protocol parser without changing Exchange.
 */
class Parser {
public:
    /**
     * @brief Parses a single input line.
     *
     * Supported commands:
     * - SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity>
     * - MARKET <id> <symbol> <BUY|SELL> <quantity>
     * - CANCEL <id>
     * - PRINT
     *
     * @param line Raw command line from an input stream.
     * @return A typed action, or std::nullopt when the command is invalid.
     */
    [[nodiscard]] std::optional<Action> parse_line(const std::string& line) const;
};

} // namespace matching_engine
