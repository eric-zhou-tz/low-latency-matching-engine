#pragma once

#include "event.hpp"
#include "order.hpp"
#include "order_pool.hpp"
#include "order_queue.hpp"

#include <cstdint>
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
     * @brief Creates an empty order book.
     */
    OrderBook() = default;

    /**
     * @brief Copies a book and rebuilds intrusive order links.
     *
     * @param other Book to copy.
     */
    OrderBook(const OrderBook& other);

    /**
     * @brief Copies a book and rebuilds intrusive order links.
     *
     * @param other Book to copy.
     * @return This book.
     */
    OrderBook& operator=(const OrderBook& other);

    /**
     * @brief Moves a book while preserving raw order pointers into owned blocks.
     *
     * @param other Book to move.
     */
    OrderBook(OrderBook&& other) noexcept;

    /**
     * @brief Moves a book while preserving raw order pointers into owned blocks.
     *
     * @param other Book to move.
     * @return This book.
     */
    OrderBook& operator=(OrderBook&& other) noexcept;

    /**
     * @brief Destroys the order book.
     */
    ~OrderBook();

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
     * @brief Clears all book state and owned order storage.
     */
    void clear() noexcept;

    /**
     * @brief Copies live resting orders from another book.
     *
     * @param other Book to copy from.
     */
    void copy_from(const OrderBook& other);

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
    // Each price level is an intrusive FIFO queue. Orders are stored in an
    // OrderPool and linked by raw pointers, avoiding std::list node allocation
    // while preserving stable FIFO order and O(1) cancel unlink by order id.
    std::map<Price, OrderQueue, std::greater<Price>> bids_;
    std::map<Price, OrderQueue> asks_;

    std::unordered_map<std::uint64_t, Order*> orders_by_id_;
    OrderPool order_pool_;
};

} // namespace matching_engine
