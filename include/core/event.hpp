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
    std::uint64_t order_id{};
};

/**
 * @brief Event emitted when a resting order is canceled.
 */
struct CanceledEvent {
    std::uint64_t order_id{};
};

/**
 * @brief Event emitted when a resting order is safely modified in place.
 */
struct ModifiedEvent {
    OrderId order_id{};
    Price old_price{};
    Price new_price{};
    Quantity old_quantity{};
    Quantity new_quantity{};
};

/**
 * @brief Event emitted when a modify uses cancel-replace semantics.
 */
struct ReplacedEvent {
    OrderId old_order_id{};
    OrderId new_order_id{};
    Price old_price{};
    Price new_price{};
    Quantity old_quantity{};
    Quantity new_quantity{};
};

/**
 * @brief Machine-readable reason for rejecting an action.
 */
enum class RejectReason {
    DuplicateOrderId,
    UnknownOrderId,
    InsufficientLiquidity,
    InvalidOrder,
};

/**
 * @brief Event emitted when an action cannot be applied.
 */
struct RejectedEvent {
    RejectReason reason{};
    std::uint64_t order_id{};
};

/**
 * @brief Event emitted for command-line book snapshot output.
 */
struct BookSnapshotEvent {
    std::string message;
};

/**
 * @brief Sum type for all events currently emitted by the engine.
 */
using Event = std::variant<TradeEvent,
                           AcceptedEvent,
                           CanceledEvent,
                           ModifiedEvent,
                           ReplacedEvent,
                           RejectedEvent,
                           BookSnapshotEvent>;

/**
 * @brief Single-event result type for order cancellation.
 */
using CancelResult = std::variant<CanceledEvent, RejectedEvent>;

/**
 * @brief Converts a rejection code into stable display text.
 *
 * @param reason Machine-readable rejection reason.
 * @return Human-readable reason prefix.
 */
[[nodiscard]] inline std::string reject_reason_text(RejectReason reason) {
    switch (reason) {
    case RejectReason::DuplicateOrderId:
        return "duplicate order id";
    case RejectReason::UnknownOrderId:
        return "unknown order id";
    case RejectReason::InsufficientLiquidity:
        return "insufficient liquidity";
    case RejectReason::InvalidOrder:
        return "invalid order";
    }

    return "unknown rejection";
}

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
            return "ACCEPTED accepted order " + std::to_string(accepted.order_id);
        }

        /**
         * @brief Formats a cancel event.
         */
        [[nodiscard]] std::string operator()(const CanceledEvent& canceled) const {
            return "CANCELED order_id=" + std::to_string(canceled.order_id);
        }

        /**
         * @brief Formats an in-place modify event.
         */
        [[nodiscard]] std::string operator()(const ModifiedEvent& modified) const {
            std::ostringstream output;
            output << "MODIFIED order_id=" << modified.order_id
                   << " old_price=" << modified.old_price << " new_price=" << modified.new_price
                   << " old_quantity=" << modified.old_quantity
                   << " new_quantity=" << modified.new_quantity;
            return output.str();
        }

        /**
         * @brief Formats a cancel-replace modify event.
         */
        [[nodiscard]] std::string operator()(const ReplacedEvent& replaced) const {
            std::ostringstream output;
            output << "REPLACED old_order_id=" << replaced.old_order_id
                   << " new_order_id=" << replaced.new_order_id
                   << " old_price=" << replaced.old_price << " new_price=" << replaced.new_price
                   << " old_quantity=" << replaced.old_quantity
                   << " new_quantity=" << replaced.new_quantity;
            return output.str();
        }

        /**
         * @brief Formats a rejection event.
         */
        [[nodiscard]] std::string operator()(const RejectedEvent& rejected) const {
            return "REJECTED " + reject_reason_text(rejected.reason) + " " +
                   std::to_string(rejected.order_id);
        }

        /**
         * @brief Formats a book snapshot event.
         */
        [[nodiscard]] std::string operator()(const BookSnapshotEvent& snapshot) const {
            // Snapshot messages are already presentation text for PRINT output.
            return snapshot.message;
        }
    };

    return std::visit(Formatter{}, event);
}

} // namespace matching_engine
