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
    if (orders_by_id_.contains(order.id)) {
        return {RejectedEvent{"duplicate order id " + std::to_string(order.id)}};
    }

    const auto order_id = order.id;
    std::vector<Event> events{AcceptedEvent{"accepted order " + std::to_string(order_id)}};

    if (order.side == Side::Buy) {
        match_buy_order(order, events);
    } else {
        match_sell_order(order, events);
    }

    if (order.quantity > 0) {
        add_resting_order(order);
    }

    return events;
}

/**
 * @brief Removes an order from the current book by id.
 *
 * The id index tells us the exact side and price level, so cancellation only
 * scans one FIFO queue instead of every order in the book.
 */
std::vector<Event> OrderBook::cancel(std::uint64_t order_id) {
    const auto location = orders_by_id_.find(order_id);
    if (location == orders_by_id_.end()) {
        return {RejectedEvent{"unknown order id " + std::to_string(order_id)}};
    }

    const auto side = location->second.side;
    const auto price = location->second.price;

    if (side == Side::Buy) {
        auto level = bids_.find(price);
        if (level != bids_.end()) {
            auto& orders = level->second;
            const auto order = std::ranges::find(orders, order_id, &Order::id);
            if (order != orders.end()) {
                orders.erase(order);
            }
            if (orders.empty()) {
                bids_.erase(level);
            }
        }
    } else {
        auto level = asks_.find(price);
        if (level != asks_.end()) {
            auto& orders = level->second;
            const auto order = std::ranges::find(orders, order_id, &Order::id);
            if (order != orders.end()) {
                orders.erase(order);
            }
            if (orders.empty()) {
                asks_.erase(level);
            }
        }
    }

    orders_by_id_.erase(location);
    return {AcceptedEvent{"cancelled order " + std::to_string(order_id)}};
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    for (const auto& [_, orders] : bids_) {
        for (const auto& order : orders) {
            output << " [" << order.id << ' ' << order.symbol << ' ' << to_string(order.side)
                   << ' ' << order.price << 'x' << order.quantity << ']';
        }
    }

    for (const auto& [_, orders] : asks_) {
        for (const auto& order : orders) {
            output << " [" << order.id << ' ' << order.symbol << ' ' << to_string(order.side)
                   << ' ' << order.price << 'x' << order.quantity << ']';
        }
    }

    return output.str();
}

/**
 * @brief Appends an order to the appropriate price level.
 */
void OrderBook::add_resting_order(const Order& order) {
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }

    orders_by_id_.emplace(order.id, OrderLocation{.side = order.side, .price = order.price});
}

/**
 * @brief Consumes resting asks while their price is at or below the buy limit.
 *
 * The front of each deque is the oldest order at that price, so matching from
 * front to back preserves FIFO price-time priority.
 */
void OrderBook::match_buy_order(Order& incoming, std::vector<Event>& events) {
    while (incoming.quantity > 0 && !asks_.empty()) {
        auto best_ask = asks_.begin();
        if (best_ask->first > incoming.price) {
            break;
        }

        auto& resting_orders = best_ask->second;
        auto& resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting.quantity);

        events.emplace_back(TradeEvent{.resting_order_id = resting.id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting.price,
                                       .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;

        if (resting.quantity == 0) {
            orders_by_id_.erase(resting.id);
            resting_orders.pop_front();
        }

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
    while (incoming.quantity > 0 && !bids_.empty()) {
        auto best_bid = bids_.begin();
        if (best_bid->first < incoming.price) {
            break;
        }

        auto& resting_orders = best_bid->second;
        auto& resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting.quantity);

        events.emplace_back(TradeEvent{.resting_order_id = resting.id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting.price,
                                       .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;

        if (resting.quantity == 0) {
            orders_by_id_.erase(resting.id);
            resting_orders.pop_front();
        }

        if (resting_orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine
