#include "order_book.hpp"

#include <algorithm>
#include <sstream>

namespace matching_engine {

/**
 * @brief Adds an order to the book, matching any immediately executable volume.
 *
 * Limit orders trade against the best opposite-side prices while they cross the
 * incoming limit. Any unfilled quantity is appended to the relevant resting
 * queue, preserving FIFO priority at that price level.
 */
std::vector<Event> OrderBook::submit(Order order) {
    // Reject duplicate ids before matching so each live order id stays unique.
    if (orders_by_id_.contains(order.id)) {
        return {RejectedEvent{"duplicate order id " + std::to_string(order.id)}};
    }

    // Acceptance is emitted first; any trades follow in matching order.
    const auto order_id = order.id;
    std::vector<Event> events{AcceptedEvent{"accepted order " + std::to_string(order_id)}};

    // Route to the opposite side of the book based on the incoming side.
    if (order.side == Side::Buy) {
        match_buy_order(order, events);
    } else {
        match_sell_order(order, events);
    }

    // If matching did not fully fill the order, leave the remainder resting.
    if (order.quantity > 0) {
        add_resting_order(order);
    }

    // Return the full observable result for this submission.
    return events;
}

/**
 * @brief Removes an order from the current book by id.
 *
 * The id index tells us the exact side and price level, so cancellation only
 * scans one FIFO queue instead of every order in the book.
 */
std::vector<Event> OrderBook::cancel(std::uint64_t order_id) {
    // Look up the side and price level where the order should live.
    const auto location = orders_by_id_.find(order_id);
    if (location == orders_by_id_.end()) {
        return {RejectedEvent{"unknown order id " + std::to_string(order_id)}};
    }

    // Copy location data before erasing the id index entry later.
    const auto side = location->second.side;
    const auto price = location->second.price;

    if (side == Side::Buy) {
        // Remove the order from its bid price level.
        auto level = bids_.find(price);
        if (level != bids_.end()) {
            auto& orders = level->second;
            const auto order = std::ranges::find(orders, order_id, &Order::id);
            if (order != orders.end()) {
                orders.erase(order);
            }
            // Drop empty levels so best-price lookup stays clean.
            if (orders.empty()) {
                bids_.erase(level);
            }
        }
    } else {
        // Remove the order from its ask price level.
        auto level = asks_.find(price);
        if (level != asks_.end()) {
            auto& orders = level->second;
            const auto order = std::ranges::find(orders, order_id, &Order::id);
            if (order != orders.end()) {
                orders.erase(order);
            }
            // Drop empty levels so best-price lookup stays clean.
            if (orders.empty()) {
                asks_.erase(level);
            }
        }
    }

    // Remove the index entry last and report the successful cancel.
    orders_by_id_.erase(location);
    return {CanceledEvent{.order_id = order_id}};
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    // Start with the live-order count for a quick summary.
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    // Print bids in book priority order: highest price, then FIFO within level.
    for (const auto& [_, orders] : bids_) {
        for (const auto& order : orders) {
            output << " [" << order.id << ' ' << order.symbol << ' ' << to_string(order.side)
                   << ' ' << order.price << 'x' << order.quantity << ']';
        }
    }

    // Print asks in book priority order: lowest price, then FIFO within level.
    for (const auto& [_, orders] : asks_) {
        for (const auto& order : orders) {
            output << " [" << order.id << ' ' << order.symbol << ' ' << to_string(order.side)
                   << ' ' << order.price << 'x' << order.quantity << ']';
        }
    }

    // Return the accumulated single-line representation.
    return output.str();
}

/**
 * @brief Appends an order to the appropriate price level.
 */
void OrderBook::add_resting_order(const Order& order) {
    // Append to the back so older orders at the same price stay ahead.
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }

    // Remember the exact level so future cancel operations are direct.
    orders_by_id_.emplace(order.id, OrderLocation{.side = order.side, .price = order.price});
}

/**
 * @brief Consumes resting asks while their price is at or below the buy limit.
 *
 * The front of each deque is the oldest order at that price, so matching from
 * front to back preserves FIFO price-time priority.
 */
void OrderBook::match_buy_order(Order& incoming, std::vector<Event>& events) {
    // Keep trading while the buy still has quantity and there is sell liquidity.
    while (incoming.quantity > 0 && !asks_.empty()) {
        // The best ask is the lowest ask price because asks_ is ascending.
        auto best_ask = asks_.begin();
        if (best_ask->first > incoming.price) {
            // The best sell is too expensive, so no further asks can cross.
            break;
        }

        // Match against the oldest resting order at the best price.
        auto& resting_orders = best_ask->second;
        auto& resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting.quantity);

        // Trades execute at the resting order's price.
        events.emplace_back(TradeEvent{.resting_order_id = resting.id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting.price,
                                       .quantity = trade_quantity});

        // Reduce both orders by the executed quantity.
        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;

        // Fully filled resting orders leave both the id index and the FIFO queue.
        if (resting.quantity == 0) {
            orders_by_id_.erase(resting.id);
            resting_orders.pop_front();
        }

        // Remove the price level when its queue has been exhausted.
        if (resting_orders.empty()) {
            asks_.erase(best_ask);
        }
    }
}

/**
 * @brief Consumes resting bids while their price is at or above the sell limit.
 *
 * Bids are sorted descending, so begin() is always the highest-priced bid.
 */
void OrderBook::match_sell_order(Order& incoming, std::vector<Event>& events) {
    // Keep trading while the sell still has quantity and there is buy liquidity.
    while (incoming.quantity > 0 && !bids_.empty()) {
        // The best bid is the highest bid price because bids_ is descending.
        auto best_bid = bids_.begin();
        if (best_bid->first < incoming.price) {
            // The best buy is too cheap, so no further bids can cross.
            break;
        }

        // Match against the oldest resting order at the best price.
        auto& resting_orders = best_bid->second;
        auto& resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting.quantity);

        // Trades execute at the resting order's price.
        events.emplace_back(TradeEvent{.resting_order_id = resting.id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting.price,
                                       .quantity = trade_quantity});

        // Reduce both orders by the executed quantity.
        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;

        // Fully filled resting orders leave both the id index and the FIFO queue.
        if (resting.quantity == 0) {
            orders_by_id_.erase(resting.id);
            resting_orders.pop_front();
        }

        // Remove the price level when its queue has been exhausted.
        if (resting_orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine
