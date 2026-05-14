#pragma once

#include "order.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <variant>

namespace matching_engine {

/**
 * @brief Event emitted when two orders trade.
 *
 * Matching is intentionally not implemented yet, but the event exists so the
 * exchange boundary is shaped around observable facts rather than direct IO.
 */
struct TradeEvent {
    std::uint64_t resting_order_id{};
    std::uint64_t incoming_order_id{};
    std::int64_t price{};
    std::uint64_t quantity{};
};

/**
 * @brief Event emitted when an action is accepted by the exchange.
 */
struct AcceptedEvent {
    std::string message;
};

/**
 * @brief Event emitted when an action cannot be applied.
 */
struct RejectedEvent {
    std::string reason;
};

/**
 * @brief Sum type for all events currently emitted by the engine.
 */
using Event = std::variant<TradeEvent, AcceptedEvent, RejectedEvent>;

/**
 * @brief Formats an event for command-line display.
 *
 * @param event Event to format.
 * @return A single-line textual representation.
 */
[[nodiscard]] inline std::string format_event(const Event& event) {
    struct Formatter {
        /**
         * @brief Formats a trade event.
         */
        [[nodiscard]] std::string operator()(const TradeEvent& trade) const {
            std::ostringstream output;
            output << "TRADE resting=" << trade.resting_order_id
                   << " incoming=" << trade.incoming_order_id << " price=" << trade.price
                   << " quantity=" << trade.quantity;
            return output.str();
        }

        /**
         * @brief Formats an acceptance event.
         */
        [[nodiscard]] std::string operator()(const AcceptedEvent& accepted) const {
            return "ACCEPTED " + accepted.message;
        }

        /**
         * @brief Formats a rejection event.
         */
        [[nodiscard]] std::string operator()(const RejectedEvent& rejected) const {
            return "REJECTED " + rejected.reason;
        }
    };

    return std::visit(Formatter{}, event);
}

} // namespace matching_engine
