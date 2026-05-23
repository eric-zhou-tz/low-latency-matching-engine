#pragma once

#include "core/order.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace matching_engine {

/**
 * @brief Action describing a client request to submit a new order.
 *
 * Submit actions are produced by Parser and consumed by Exchange. The action
 * keeps the symbol needed for routing, while the book receives a compact
 * symbol-free Order after routing has finished.
 */
struct SubmitOrderAction {
    std::uint64_t id{};
    std::string symbol;
    Side side{Side::Buy};
    std::int64_t price{};
    std::uint64_t quantity{};
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
