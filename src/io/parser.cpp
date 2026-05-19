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

/**
 * @brief Parses an optional time-in-force token from the command stream.
 *
 * @param token Text token to parse.
 * @return Parsed policy when recognized.
 */
[[nodiscard]] std::optional<TimeInForce> parse_time_in_force(const std::string& token) {
    // GTC is the default resting-limit policy, but accepting it keeps input explicit.
    if (token == "GTC") {
        return TimeInForce::GoodTilCancel;
    }
    if (token == "IOC") {
        return TimeInForce::ImmediateOrCancel;
    }
    if (token == "FOK") {
        return TimeInForce::FillOrKill;
    }
    // Unknown flags make the SUBMIT command malformed.
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
        SubmitOrderAction action;
        std::string side_token;
        input >> action.id >> action.symbol >> side_token >> action.price >> action.quantity;

        // Validate the fields that cannot be trusted from text input.
        const auto side = parse_side(side_token);
        if (!input || !side || action.symbol.empty() || action.quantity == 0) {
            return std::nullopt;
        }

        // Parse an optional limit-order lifetime flag after the core fields.
        std::string time_in_force_token;
        if (input >> time_in_force_token) {
            const auto time_in_force = parse_time_in_force(time_in_force_token);
            if (!time_in_force) {
                return std::nullopt;
            }
            action.time_in_force = *time_in_force;

            // Reject extra tokens so malformed commands do not silently pass.
            std::string extra_token;
            if (input >> extra_token) {
                return std::nullopt;
            }
        }

        // Store the parsed side and return a typed submit action.
        action.side = *side;
        return action;
    }

    if (command == "MARKET") {
        // Read the market order fields; market orders do not carry a limit price.
        MarketOrderAction action;
        std::string side_token;
        input >> action.id >> action.symbol >> side_token >> action.quantity;

        // Validate side and quantity before returning a command to the exchange.
        const auto side = parse_side(side_token);
        if (!input || !side || action.symbol.empty() || action.quantity == 0) {
            return std::nullopt;
        }

        // Reject extra tokens so malformed market commands do not silently pass.
        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        // Store the parsed side so the exchange can route to the correct book side.
        action.side = *side;
        return action;
    }

    if (command == "CANCEL") {
        // Cancellation only needs the unique order id.
        std::uint64_t order_id{};
        input >> order_id;
        if (!input) {
            return std::nullopt;
        }

        // Reject extra tokens so cancellation targets remain unambiguous.
        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        // Wrap the id in the action variant expected by the exchange.
        return CancelOrderAction{order_id};
    }

    if (command == "MODIFY") {
        // Read the replacement fields; the exchange routes by the original order id.
        ModifyOrderAction action;
        input >> action.order_id >> action.new_price >> action.new_quantity;
        if (!input || action.new_quantity == 0) {
            return std::nullopt;
        }

        // Reject extra tokens so malformed commands do not silently pass.
        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        // Return the typed modify action for exchange-level routing.
        return action;
    }

    if (command == "PRINT") {
        // Reject extra tokens because PRINT currently has no selector payload.
        std::string extra_token;
        if (input >> extra_token) {
            return std::nullopt;
        }

        // PRINT carries no payload; the exchange will snapshot known books.
        return PrintBookAction{};
    }

    // Unknown commands are rejected by returning no action.
    return std::nullopt;
}

} // namespace matching_engine
