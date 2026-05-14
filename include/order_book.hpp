#pragma once

#include "event.hpp"
#include "order.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine {

/**
 * @brief Stores orders for a single symbol and emits domain events.
 *
 * The book is organized as side-specific price levels. Bids and asks are stored
 * separately because they have opposite best-price rules, while each price level
 * preserves FIFO order for price-time priority matching.
 */
class OrderBook {
public:
    /**
     * @brief Adds an order to the book.
     *
     * @param order Order to store.
     * @return Events describing the result of the operation.
     */
    [[nodiscard]] std::vector<Event> submit(Order order);

    /**
     * @brief Cancels an existing order by id.
     *
     * @param order_id Identifier to cancel.
     * @return Events describing the result of the operation.
     */
    [[nodiscard]] std::vector<Event> cancel(std::uint64_t order_id);

    /**
     * @brief Builds a compact textual snapshot of the current book.
     *
     * @return Human-readable book summary.
     */
    [[nodiscard]] std::string snapshot() const;

private:
    using Price = std::int64_t;

    /**
     * @brief Minimal location record used to cancel without scanning the book.
     */
    struct OrderLocation {
        Side side;
        Price price;
    };

    /**
     * @brief Adds an unmatched order to its side-specific price level.
     *
     * @param order Order that should rest on the book.
     */
    void add_resting_order(const Order& order);

    /**
     * @brief Matches an incoming buy order against resting asks.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param events Output event collection for generated trades.
     */
    void match_buy_order(Order& incoming, std::vector<Event>& events);

    /**
     * @brief Matches an incoming sell order against resting bids.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param events Output event collection for generated trades.
     */
    void match_sell_order(Order& incoming, std::vector<Event>& events);

    // Bids and asks live in separate maps because each side has a different
    // notion of "best": highest buy price versus lowest sell price.
    //
    // Bids use std::greater<> so begin() points at the highest price. Asks keep
    // the default ascending order so begin() points at the lowest price.
    //
    // Each price level stores a deque so new resting orders append at the back
    // and later matching can consume from the front for FIFO price-time priority.
    std::map<Price, std::deque<Order>, std::greater<Price>> bids_;
    std::map<Price, std::deque<Order>> asks_;

    std::unordered_map<std::uint64_t, OrderLocation> orders_by_id_;
};

} // namespace matching_engine
