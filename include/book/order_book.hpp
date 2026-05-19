#pragma once

#include "book/order_pool.hpp"
#include "book/order_queue.hpp"
#include "core/event.hpp"
#include "core/order.hpp"

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
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
     * @brief Creates an empty order book with reserved order-id capacity.
     *
     * @param expected_order_capacity Expected number of live resting orders.
     */
    explicit OrderBook(std::size_t expected_order_capacity);

    /**
     * @brief Creates an empty order book with tuned order-id lookup density.
     *
     * @param expected_order_capacity Expected number of live resting orders.
     * @param order_id_max_load_factor Maximum load factor for the order-id map.
     */
    OrderBook(std::size_t expected_order_capacity, float order_id_max_load_factor);

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
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void submit(Order order, std::vector<Event>& out);

    /**
     * @brief Matches a market order without resting any unfilled remainder.
     *
     * @param order Market order to match immediately.
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void submit_market(Order order, std::vector<Event>& out);

    /**
     * @brief Cancels an existing order by id.
     *
     * @param order_id Identifier to cancel.
     * @return Single event describing whether the cancel succeeded.
     */
    [[nodiscard]] CancelResult cancel(OrderId order_id);

    /**
     * @brief Modifies a resting order using exchange-style priority rules.
     *
     * @param order_id Identifier of the resting order to modify.
     * @param new_price Replacement limit price.
     * @param new_quantity Replacement total remaining quantity.
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void modify(OrderId order_id, Price new_price, Quantity new_quantity, std::vector<Event>& out);

    /**
     * @brief Checks whether an order id is currently resting in this book.
     *
     * @param order_id Identifier to look up.
     * @return True when the order is live in this book.
     */
    [[nodiscard]] bool contains_order(std::uint64_t order_id) const;

    /**
     * @brief Builds a compact textual snapshot of the current book.
     *
     * @return Human-readable book summary.
     */
    [[nodiscard]] std::string snapshot() const;

    /**
     * @brief Reserves live order-id lookup capacity for expected book depth.
     *
     * @param expected_order_capacity Expected number of live resting orders.
     */
    void reserve_order_capacity(std::size_t expected_order_capacity);

    /**
     * @brief Sets the order-id lookup load factor for future reservations.
     *
     * @param order_id_max_load_factor Maximum load factor for the order-id map.
     */
    void set_order_id_max_load_factor(float order_id_max_load_factor);

private:
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
     * @brief Finds a live resting order by id.
     *
     * @param order_id Identifier to look up.
     * @return Pointer to the live order, or nullptr when missing.
     */
    [[nodiscard]] Order* find_resting_order(OrderId order_id) const;

    /**
     * @brief Removes one live resting order from its queue, index, and storage.
     *
     * @param order Resting order node to remove.
     */
    void remove_resting_order(Order* order);

    /**
     * @brief Prepares an incoming order and emits the acceptance or rejection.
     *
     * @param order Incoming order whose intrusive links should be reset.
     * @param out Caller-owned event buffer filled with initial events.
     * @return True when matching may continue.
     */
    [[nodiscard]] bool prepare_incoming_order(Order& order, std::vector<Event>& out) const;

    /**
     * @brief Checks whether a limit order can execute its full quantity now.
     *
     * @param order Incoming limit order to test against opposite-side liquidity.
     * @return True when crossing price levels hold enough visible quantity.
     */
    [[nodiscard]] bool can_fully_fill(const Order& order) const;

    /**
     * @brief Matches an incoming order and rests any remaining GTC quantity.
     *
     * @param order Incoming order to execute.
     * @param out Output event collection for generated trades.
     */
    void execute_incoming_order(Order order, std::vector<Event>& out);

    /**
     * @brief Matches an incoming buy order against resting asks.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param out Output event collection for generated trades.
     */
    void match_buy_order(Order& incoming, std::vector<Event>& out);

    /**
     * @brief Matches an incoming sell order against resting bids.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param out Output event collection for generated trades.
     */
    void match_sell_order(Order& incoming, std::vector<Event>& out);

    // keep bids and asks in separate maps because each side has the opposite best-price rule.
    std::map<Price, OrderQueue, std::greater<Price>> bids_;
    std::map<Price, OrderQueue> asks_;

    // order ids point at intrusive nodes owned by the pool, giving cancels direct unlink targets.
    ankerl::unordered_dense::map<std::uint64_t, Order*> orders_by_id_;
    OrderPool order_pool_;
};

} // namespace matching_engine
