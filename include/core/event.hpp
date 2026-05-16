#pragma once

#include "core/order.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <variant>

namespace matching_engine {

/**
 * @brief Event emitted when two orders trade.
 *
 * Trade events describe observable executions without coupling the matching
 * engine core to terminal output or any other presentation layer.
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
 * @brief Event emitted when a resting order is canceled.
 */
struct CanceledEvent {
    std::uint64_t order_id{};
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
using Event = std::variant<TradeEvent, AcceptedEvent, CanceledEvent, RejectedEvent>;

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
            // Build a compact execution line with both order ids and fill details.
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
            // Prefix the exchange-provided message with the event type.
            return "ACCEPTED " + accepted.message;
        }

        /**
         * @brief Formats a cancel event.
         */
        [[nodiscard]] std::string operator()(const CanceledEvent& canceled) const {
            // Include the canceled id so command-line users can confirm the target.
            return "CANCELED order_id=" + std::to_string(canceled.order_id);
        }

        /**
         * @brief Formats a rejection event.
         */
        [[nodiscard]] std::string operator()(const RejectedEvent& rejected) const {
            // Preserve the rejection reason supplied by the failing operation.
            return "REJECTED " + rejected.reason;
        }
    };

    // Let std::visit choose the formatter overload for the active event type.
    return std::visit(Formatter{}, event);
}

} // namespace matching_engine
