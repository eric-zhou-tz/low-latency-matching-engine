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
    TimeInForce time_in_force{TimeInForce::GoodTilCancel};
};

/**
 * @brief Action describing a client request to submit a market order.
 *
 * Market orders carry no limit price. They consume available opposite-side
 * liquidity immediately and never leave a resting remainder on the book.
 */
struct MarketOrderAction {
    std::uint64_t id{};
    std::string symbol;
    Side side{Side::Buy};
    std::uint64_t quantity{};
};

/**
 * @brief Action describing a request to cancel an existing order.
 */
struct CancelOrderAction {
    std::uint64_t order_id{};
};

/**
 * @brief Action describing a request to modify a resting order.
 *
 * Modification is routed by order id because the exchange-level live index
 * already knows which symbol book owns each cancelable order.
 */
struct ModifyOrderAction {
    OrderId order_id{};
    Price new_price{};
    Quantity new_quantity{};
};

/**
 * @brief Action requesting a human-readable snapshot of the current book.
 */
struct PrintBookAction {};

/**
 * @brief Sum type for every command accepted by the exchange.
 */
using Action =
    std::variant<SubmitOrderAction,
                 MarketOrderAction,
                 CancelOrderAction,
                 ModifyOrderAction,
                 PrintBookAction>;

} // namespace matching_engine
