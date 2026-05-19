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
    if (token == "BUY") {
        return Side::Buy;
    }
    if (token == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

/**
 * @brief Parses an optional time-in-force token from the command stream.
 *
 * @param token Text token to parse.
 * @return Parsed policy when recognized.
 */
[[nodiscard]] std::optional<TimeInForce> parse_time_in_force(const std::string& token) {
    if (token == "GTC") {
        return TimeInForce::GoodTilCancel;
    }
    if (token == "IOC") {
        return TimeInForce::ImmediateOrCancel;
    }
    if (token == "FOK") {
        return TimeInForce::FillOrKill;
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
        SubmitOrderAction action;
        std::string side_token;
        input >> action.id >> action.symbol >> side_token >> action.price >> action.quantity;

        const auto side = parse_side(side_token);
        if (!input || !side || action.symbol.empty() || action.quantity == 0) {
            return std::nullopt;
        }

        std::string time_in_force_token;
        if (input >> time_in_force_token) {
            const auto time_in_force = parse_time_in_force(time_in_force_token);
            if (!time_in_force) {
                return std::nullopt;
            }
            action.time_in_force = *time_in_force;

            std::string extra_token;
            if (input >> extra_token) {
                return std::nullopt;
            }
        }

        action.side = *side;
        return action;
    }

    if (command == "MARKET") {
        MarketOrderAction action;
        std::string side_token;
        input >> action.id >> action.symbol >> side_token >> action.quantity;

        const auto side = parse_side(side_token);
        if (!input || !side || action.symbol.empty() || action.quantity == 0) {
            return std::nullopt;
        }

        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        action.side = *side;
        return action;
    }

    if (command == "CANCEL") {
        std::uint64_t order_id{};
        input >> order_id;
        if (!input) {
            return std::nullopt;
        }

        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        return CancelOrderAction{order_id};
    }

    if (command == "MODIFY") {
        ModifyOrderAction action;
        input >> action.order_id >> action.new_price >> action.new_quantity;
        if (!input || action.new_quantity == 0) {
            return std::nullopt;
        }

        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        return action;
    }

    if (command == "PRINT") {
        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        return PrintBookAction{};
    }

    return std::nullopt;
}

} // namespace matching_engine
