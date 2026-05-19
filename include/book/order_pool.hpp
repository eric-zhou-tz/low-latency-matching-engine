#pragma once

#include "core/order.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace matching_engine {

/**
 * @brief Owns stable contiguous storage for live orders.
 *
 * Orders are allocated from fixed-size blocks and returned through an intrusive
 * free list. Insert allocates only when a block is exhausted, while cancel and
 * match return slots without calling the general-purpose allocator.
 */
class OrderPool {
public:
    /**
     * @brief Creates an empty order pool.
     */
    OrderPool() = default;

    /**
     * @brief Move-constructs a pool while preserving stable order pointers.
     *
     * @param other Pool to move from.
     */
    OrderPool(OrderPool&& other) noexcept
        : blocks_(std::move(other.blocks_)),
          free_orders_(std::exchange(other.free_orders_, nullptr)),
          next_block_index_(std::exchange(other.next_block_index_, 0)) {
    }

    /**
     * @brief Move-assigns a pool while preserving stable order pointers.
     *
     * @param other Pool to move from.
     * @return This pool.
     */
    OrderPool& operator=(OrderPool&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        clear();
        blocks_ = std::move(other.blocks_);
        free_orders_ = std::exchange(other.free_orders_, nullptr);
        next_block_index_ = std::exchange(other.next_block_index_, 0);

        return *this;
    }

    /**
     * @brief Destroys all constructed order slots.
     */
    ~OrderPool() {
        clear();
    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    /**
     * @brief Allocates raw storage for an expected number of live orders.
     *
     * @param expected_order_capacity Expected number of simultaneously resting orders.
     */
    void reserve(std::size_t expected_order_capacity) {
        const std::size_t required_blocks =
            (expected_order_capacity + kBlockSize - 1) / kBlockSize;

        blocks_.reserve(required_blocks);

        while (blocks_.size() < required_blocks) {
            blocks_.push_back(Block{.data = allocator_.allocate(kBlockSize), .used = 0});
        }
    }

    /**
     * @brief Allocates or reuses storage for one live order.
     *
     * @param order Order value to copy into pool-owned storage.
     * @return Stable pointer to the stored order.
     */
    [[nodiscard]] Order* create(const Order& order) {
        if (free_orders_ != nullptr) {
            Order* reused = free_orders_;
            free_orders_ = free_orders_->next;
            *reused = order;
            reused->prev = nullptr;
            reused->next = nullptr;
            return reused;
        }

        while (next_block_index_ < blocks_.size() &&
               blocks_[next_block_index_].used == kBlockSize) {
            ++next_block_index_;
        }

        if (next_block_index_ == blocks_.size()) {
            blocks_.push_back(Block{.data = allocator_.allocate(kBlockSize), .used = 0});
        }

        auto& block = blocks_[next_block_index_];
        Order* stored = block.data + block.used++;
        std::construct_at(stored, order);
        stored->prev = nullptr;
        stored->next = nullptr;
        return stored;
    }

    /**
     * @brief Returns a dead order slot to the local free list.
     *
     * @param order Order that has already been unlinked from the book.
     */
    void release(Order* order) noexcept {
        order->prev = nullptr;
        order->next = free_orders_;
        order->quantity = 0;
        free_orders_ = order;
    }

    /**
     * @brief Clears all owned order storage.
     */
    void clear() noexcept {
        for (auto& block : blocks_) {
            for (std::size_t index = 0; index < block.used; ++index) {
                std::destroy_at(block.data + index);
            }
            allocator_.deallocate(block.data, kBlockSize);
        }

        blocks_.clear();
        free_orders_ = nullptr;
        next_block_index_ = 0;
    }

private:
    static constexpr std::size_t kBlockSize = 4096;

    /**
     * @brief Contiguous storage block for stable order slots.
     */
    struct Block {
        Order* data{};
        std::size_t used{};
    };

    std::allocator<Order> allocator_;
    std::vector<Block> blocks_;
    Order* free_orders_{};
    std::size_t next_block_index_{};
};

} // namespace matching_engine
