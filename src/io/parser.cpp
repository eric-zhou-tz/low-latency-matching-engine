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

/**
 * @brief Parses an ADD_SYMBOL ladder configuration pair.
 *
 * @param input Command stream positioned after the LADDER token.
 * @param action Action whose ladder metadata should be filled.
 * @return True when both required fields are present exactly once.
 */
[[nodiscard]] bool parse_ladder_fields(std::istringstream& input, AddSymbolAction& action) {
    bool has_base = false;
    bool has_range = false;

    for (int field = 0; field < 2; ++field) {
        std::string key;
        PriceTick value{};
        input >> key >> value;
        if (!input) {
            return false;
        }

        if (key == "BASE" && !has_base) {
            // Store the center tick used later to derive the prepared ladder window.
            action.base_tick = value;
            has_base = true;
        } else if (key == "RANGE" && !has_range && value >= 0) {
            // Range is a distance, so negative values would create nonsensical bounds.
            action.tick_range = value;
            has_range = true;
        } else {
            return false;
        }
    }

    std::string extra_token;
    if (input >> extra_token) {
        return false;
    }

    return has_base && has_range;
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

    if (command == "ADD_SYMBOL") {
        AddSymbolAction action;
        std::string price_level_type;
        input >> action.symbol >> price_level_type;
        if (!input || action.symbol.empty()) {
            return std::nullopt;
        }

        if (price_level_type == "TREE") {
            std::string extra_token;
            if (input >> extra_token) {
                return std::nullopt;
            }

            // Tree is the current default and requires no ladder bounds.
            action.price_level_mode = PriceLevelMode::Tree;
            return action;
        }

        if (price_level_type == "LADDER") {
            // Ladder commands must carry explicit metadata so the book can bound its vector.
            action.price_level_mode = PriceLevelMode::Ladder;
            if (!parse_ladder_fields(input, action)) {
                return std::nullopt;
            }
            return action;
        }

        return std::nullopt;
    }

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
