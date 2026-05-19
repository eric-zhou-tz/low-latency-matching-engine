#pragma once

#include "core/order.hpp"

#include <cstdint>

namespace matching_engine {

/**
 * @brief Intrusive FIFO queue for one price level.
 *
 * Orders carry their own prev/next links, so queue updates do not allocate
 * separate list nodes. This keeps cancel work to pointer rewiring and lowers
 * allocator pressure in randomized cancel workloads.
 */
class OrderQueue {
public:
    Order* head{};
    Order* tail{};
    std::uint64_t total_volume{};

    /**
     * @brief Appends an order to the FIFO tail.
     *
     * @param order Resting order to append.
     */
    void push_back(Order* order) noexcept {
        order->prev = tail;
        order->next = nullptr;

        if (tail != nullptr) {
            tail->next = order;
        } else {
            head = order;
        }

        tail = order;
        total_volume += order->quantity;
    }

    /**
     * @brief Removes an arbitrary order from this FIFO queue.
     *
     * @param order Resting order already linked into this level.
     */
    void remove(Order* order) noexcept {
        if (order->prev != nullptr) {
            order->prev->next = order->next;
        } else {
            head = order->next;
        }

        if (order->next != nullptr) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev;
        }

        total_volume -= order->quantity;
        order->prev = nullptr;
        order->next = nullptr;
    }

    /**
     * @brief Removes and returns the FIFO head order.
     *
     * @return Removed head order, or nullptr when empty.
     */
    Order* pop_front() noexcept {
        Order* order = head;
        if (order != nullptr) {
            remove(order);
        }

        return order;
    }

    /**
     * @brief Returns the FIFO head order without removing it.
     *
     * @return Oldest resting order at this price.
     */
    [[nodiscard]] Order* front() const noexcept {
        return head;
    }

    /**
     * @brief Reports whether this queue has no orders.
     *
     * @return True when no resting orders remain.
     */
    [[nodiscard]] bool empty() const noexcept {
        return head == nullptr;
    }
};

} // namespace matching_engine
