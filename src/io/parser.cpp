#include "io/parser.hpp"

#include <sstream>
#include <string>

namespace matching_engine {
namespace {

/**
 * @brief Parses a side token from the command stream.
 *
 * @param token Text token to parse.
 * @return Parsed side when recognized.
 */
[[nodiscard]] std::optional<Side> parse_side(const std::string& token) {
    // Match only the protocol spellings accepted by the command format.
    if (token == "BUY") {
        return Side::Buy;
    }
    if (token == "SELL") {
        return Side::Sell;
    }
    // Anything else leaves the caller with an invalid parse.
    return std::nullopt;
}

} // namespace

/**
 * @brief Converts a single textual command into a typed action.
 *
 * The first command set is intentionally tiny so the project can compile and
 * demonstrate the pipeline before matching semantics are added.
 */
std::optional<Action> Parser::parse_line(const std::string& line) const {
    // Stream parsing keeps whitespace handling simple and deterministic.
    std::istringstream input{line};
    std::string command;
    input >> command;

    if (command == "SUBMIT") {
        // Read the order fields in the protocol order.
        Order order;
        std::string side_token;
        input >> order.id >> order.symbol >> side_token >> order.price >> order.quantity;

        // Validate the fields that cannot be trusted from text input.
        const auto side = parse_side(side_token);
        if (!input || !side || order.symbol.empty() || order.quantity == 0) {
            return std::nullopt;
        }

        // Store the parsed enum and return a typed submit action.
        order.side = *side;
        return SubmitOrderAction{order};
    }

    if (command == "CANCEL") {
        // Cancellation only needs the unique order id.
        std::uint64_t order_id{};
        input >> order_id;
        if (!input) {
            return std::nullopt;
        }
        // Wrap the id in the action variant expected by the exchange.
        return CancelOrderAction{order_id};
    }

    if (command == "PRINT") {
        // PRINT carries no payload; the exchange will snapshot known books.
        return PrintBookAction{};
    }

    // Unknown commands are rejected by returning no action.
    return std::nullopt;
}

} // namespace matching_engine
