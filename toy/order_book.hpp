#pragma once

#include "core/event.hpp"
#include "core/order.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace matching_engine::toy {

/**
 * @brief Simple std-container order book used as a reference baseline.
 *
 * ToyOrderBook intentionally mirrors the public behavior of the optimized
 * OrderBook while using ordinary STL containers and straightforward scans.
 */
class OrderBook {
public:
    /**
     * @brief Adds a limit order, matching any immediately executable volume.
     *
     * @param order Incoming order to submit.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void submit(Order order, std::vector<Event>& out);

    /**
     * @brief Matches a market order without resting any leftover quantity.
     *
     * @param order Incoming market order.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void submit_market(Order order, std::vector<Event>& out);

    /**
     * @brief Cancels a resting order by id.
     *
     * @param order_id Identifier to cancel.
     * @return Single event describing the cancel result.
     */
    [[nodiscard]] CancelResult cancel(OrderId order_id);

    /**
     * @brief Modifies a resting order using the same public rules as fast mode.
     *
     * @param order_id Identifier of the resting order.
     * @param new_price Replacement limit price.
     * @param new_quantity Replacement total remaining quantity.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void modify(OrderId order_id, Price new_price, Quantity new_quantity, std::vector<Event>& out);

    /**
     * @brief Checks whether an order id is currently resting.
     *
     * @param order_id Identifier to look up.
     * @return True when the order is live in this book.
     */
    [[nodiscard]] bool contains_order(OrderId order_id) const;

    /**
     * @brief Builds the same compact book snapshot string as fast mode.
     *
     * @return Human-readable resting-order summary.
     */
    [[nodiscard]] std::string snapshot() const;

private:
    /**
     * @brief Naive per-price FIFO queue.
     */
    struct PriceLevel {
        std::deque<Order> orders;
    };

    /**
     * @brief Mutable location for a resting order found by scanning.
     */
    struct OrderCursor {
        Side side{};
        Price price{};
        std::deque<Order>::iterator order;
    };

    /**
     * @brief Immutable location for a resting order found by scanning.
     */
    struct ConstOrderCursor {
        Side side{};
        Price price{};
        std::deque<Order>::const_iterator order;
    };

    /**
     * @brief Finds a resting order by scanning all price levels.
     *
     * @param order_id Identifier to find.
     * @return Cursor to the live order or an empty cursor when missing.
     */
    [[nodiscard]] std::optional<OrderCursor> find_resting_order(OrderId order_id);

    /**
     * @brief Finds a resting order by scanning all price levels.
     *
     * @param order_id Identifier to find.
     * @return Cursor to the live order or an empty cursor when missing.
     */
    [[nodiscard]] std::optional<ConstOrderCursor> find_resting_order(OrderId order_id) const;

    /**
     * @brief Adds an unmatched order to the correct std::deque price level.
     *
     * @param order Remaining order quantity that should rest.
     */
    void add_resting_order(const Order& order);

    /**
     * @brief Removes a resting order at a previously scanned cursor.
     *
     * @param cursor Cursor identifying the order to remove.
     * @return Removed order when found.
     */
    [[nodiscard]] Order remove_resting_order(const OrderCursor& cursor);

    /**
     * @brief Resets per-action output and rejects duplicate live ids.
     *
     * @param order Incoming order to validate.
     * @param out Caller-owned event buffer filled with initial events.
     * @return True when matching can continue.
     */
    [[nodiscard]] bool prepare_incoming_order(const Order& order, std::vector<Event>& out) const;

    /**
     * @brief Checks whether a limit order can fully execute now.
     *
     * @param order Incoming limit order to test.
     * @return True when crossing liquidity covers the full quantity.
     */
    [[nodiscard]] bool can_fully_fill(const Order& order) const;

    /**
     * @brief Runs the shared limit-order match/rest flow.
     *
     * @param order Incoming order whose remainder may rest.
     * @param out Output event collection for generated trades.
     */
    void execute_incoming_order(Order order, std::vector<Event>& out);

    /**
     * @brief Matches a buy order against asks in ascending price order.
     *
     * @param incoming Mutable incoming order with remaining quantity.
     * @param out Output event collection for generated trades.
     */
    void match_buy_order(Order& incoming, std::vector<Event>& out);

    /**
     * @brief Matches a sell order against bids in descending price order.
     *
     * @param incoming Mutable incoming order with remaining quantity.
     * @param out Output event collection for generated trades.
     */
    void match_sell_order(Order& incoming, std::vector<Event>& out);

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel> asks_;
};

} // namespace matching_engine::toy
