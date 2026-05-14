#include "order_book.hpp"

#include <algorithm>
#include <sstream>

namespace matching_engine {

/**
 * @brief Adds an order to the book without performing matching yet.
 *
 * TODO: Add price-time priority queues and crossing logic here.
 */
std::vector<Event> OrderBook::submit(Order order) {
    if (orders_by_id_.contains(order.id)) {
        return {RejectedEvent{"duplicate order id " + std::to_string(order.id)}};
    }

    const auto order_id = order.id;
    add_resting_order(order);

    return {AcceptedEvent{"accepted order " + std::to_string(order_id)}};
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
 *
 * TODO: Call this only for the unfilled remainder once matching is implemented.
 */
void OrderBook::add_resting_order(const Order& order) {
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }

    orders_by_id_.emplace(order.id, OrderLocation{.side = order.side, .price = order.price});
}

} // namespace matching_engine
