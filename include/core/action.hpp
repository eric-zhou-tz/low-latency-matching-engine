#pragma once

#include "core/order.hpp"

#include <cstdint>
#include <variant>

namespace matching_engine {

/**
 * @brief Action describing a request to add a new order to the book.
 *
 * Submit actions are produced by Parser and consumed by Exchange. Keeping input
 * intent as a value type makes it easy to test parsing and later replay command
 * streams deterministically.
 */
struct SubmitOrderAction {
    Order order;
};

/**
 * @brief Action describing a request to cancel an existing order.
 */
struct CancelOrderAction {
    std::uint64_t order_id{};
};

/**
 * @brief Action requesting a human-readable snapshot of the current book.
 */
struct PrintBookAction {};

/**
 * @brief Sum type for every command accepted by the exchange.
 */
using Action = std::variant<SubmitOrderAction, CancelOrderAction, PrintBookAction>;

} // namespace matching_engine
