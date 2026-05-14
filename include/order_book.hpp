#pragma once

#include "event.hpp"
#include "order.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine {

/**
 * @brief Stores orders for a single symbol and emits domain events.
 *
 * This scaffold keeps only an id-indexed collection. Price-time queues and real
 * crossing logic belong here once matching is implemented.
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
    std::unordered_map<std::uint64_t, Order> orders_by_id_;
};

} // namespace matching_engine
