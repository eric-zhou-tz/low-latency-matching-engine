#include "parser.hpp"

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
    if (token == "BUY") {
        return Side::Buy;
    }
    if (token == "SELL") {
        return Side::Sell;
    }
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
    std::istringstream input{line};
    std::string command;
    input >> command;

    if (command == "SUBMIT") {
        Order order;
        std::string side_token;
        input >> order.id >> order.symbol >> side_token >> order.price >> order.quantity;

        const auto side = parse_side(side_token);
        if (!input || !side || order.symbol.empty() || order.quantity == 0) {
            return std::nullopt;
        }

        order.side = *side;
        return SubmitOrderAction{order};
    }

    if (command == "CANCEL") {
        std::uint64_t order_id{};
        input >> order_id;
        if (!input) {
            return std::nullopt;
        }
        return CancelOrderAction{order_id};
    }

    if (command == "PRINT") {
        return PrintBookAction{};
    }

    return std::nullopt;
}

} // namespace matching_engine
