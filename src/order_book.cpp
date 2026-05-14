#include "order_book.hpp"

#include <sstream>
#include <utility>

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
    orders_by_id_.emplace(order_id, std::move(order));

    return {AcceptedEvent{"accepted order " + std::to_string(order_id)}};
}

/**
 * @brief Removes an order from the current book by id.
 *
 * TODO: Maintain side-specific price levels when full matching logic lands.
 */
std::vector<Event> OrderBook::cancel(std::uint64_t order_id) {
    const auto erased = orders_by_id_.erase(order_id);
    if (erased == 0) {
        return {RejectedEvent{"unknown order id " + std::to_string(order_id)}};
    }

    return {AcceptedEvent{"cancelled order " + std::to_string(order_id)}};
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    for (const auto& [_, order] : orders_by_id_) {
        output << " [" << order.id << ' ' << order.symbol << ' ' << to_string(order.side)
               << ' ' << order.price << 'x' << order.quantity << ']';
    }

    return output.str();
}

} // namespace matching_engine
