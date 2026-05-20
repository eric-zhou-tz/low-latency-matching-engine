#include "book/order_book.hpp"

#include <cassert>
#include <iterator>
#include <variant>

namespace matching_engine {

/**
 * @brief Selects the mutable price-level storage for a side.
 */
OrderBook::PriceLevels& OrderBook::levels_for(Side side) {
    // Bids and asks share the same variant shape; side decides which storage owns the level.
    return side == Side::Buy ? bids_ : asks_;
}

/**
 * @brief Selects the immutable price-level storage for a side.
 */
const OrderBook::PriceLevels& OrderBook::levels_for(Side side) const {
    // Delegate through the same side rule so const and mutable callers stay aligned.
    return side == Side::Buy ? bids_ : asks_;
}

/**
 * @brief Checks whether a price can be represented by the current storage mode.
 */
bool OrderBook::price_in_range(PriceTick price) const noexcept {
    if (price_level_mode_ == PriceLevelMode::Tree) {
        return true;
    }

    // Ladder mode intentionally rejects prices outside the fixed construction window.
    return price >= min_tick_ && price <= max_tick_;
}

/**
 * @brief Converts an in-range price tick to a vector slot.
 */
std::size_t OrderBook::ladder_index(PriceTick price) const noexcept {
    // The ladder is anchored at min_tick_, not base_tick_, so every slot is non-negative.
    return static_cast<std::size_t>(price - min_tick_);
}

/**
 * @brief Finds a mutable level by price.
 */
OrderQueue* OrderBook::find_level(Side side, PriceTick price) {
    auto& levels = levels_for(side);

    if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        const auto found = tree->find(price);
        if (found == tree->end()) {
            return nullptr;
        }

        // Returning the queue pointer hides the map iterator from matching code.
        return &found->second;
    }

    if (!price_in_range(price)) {
        return nullptr;
    }

    auto& queue = std::get<LadderLevels>(levels)[ladder_index(price)];
    return queue.empty() ? nullptr : &queue;
}

/**
 * @brief Finds an immutable level by price.
 */
const OrderQueue* OrderBook::find_level(Side side, PriceTick price) const {
    const auto& levels = levels_for(side);

    if (const auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        const auto found = tree->find(price);
        if (found == tree->end()) {
            return nullptr;
        }

        // Returning the queue pointer hides the map iterator from read-only callers.
        return &found->second;
    }

    if (!price_in_range(price)) {
        return nullptr;
    }

    const auto& queue = std::get<LadderLevels>(levels)[ladder_index(price)];
    return queue.empty() ? nullptr : &queue;
}

/**
 * @brief Finds or creates a mutable level by price.
 */
OrderQueue* OrderBook::get_or_create_level(Side side, PriceTick price) {
    auto& levels = levels_for(side);

    if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        // try_emplace creates an empty FIFO queue only when the price is new.
        return &tree->try_emplace(price).first->second;
    }

    if (!price_in_range(price)) {
        return nullptr;
    }

    // Ladder slots are preallocated, so creation means returning the indexed queue.
    return &std::get<LadderLevels>(levels)[ladder_index(price)];
}

/**
 * @brief Returns a mutable level that should already exist.
 */
OrderQueue& OrderBook::require_level(Side side, PriceTick price) {
    OrderQueue* level = find_level(side, price);
    assert(level != nullptr);

    // Existing order metadata points at this level, so a missing level is a book invariant bug.
    return *level;
}

/**
 * @brief Returns an immutable level that should already exist.
 */
const OrderQueue& OrderBook::require_level(Side side, PriceTick price) const {
    const OrderQueue* level = find_level(side, price);
    assert(level != nullptr);

    // Existing order metadata points at this level, so a missing level is a book invariant bug.
    return *level;
}

/**
 * @brief Erases a level by price.
 */
void OrderBook::erase_level(Side side, PriceTick price) {
    auto& levels = levels_for(side);

    if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        // Map erase removes empty nodes so tree best-level lookup only sees live queues.
        tree->erase(price);
        return;
    }

    if (!price_in_range(price)) {
        return;
    }

    // Ladder erase clears the slot but keeps vector indexes stable for all prices.
    std::get<LadderLevels>(levels)[ladder_index(price)] = OrderQueue{};
}

/**
 * @brief Erases a level only when no orders remain.
 */
void OrderBook::erase_level_if_empty(Side side, PriceTick price) {
    auto& levels = levels_for(side);

    if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        const auto found = tree->find(price);
        if (found == tree->end() || !found->second.empty()) {
            return;
        }

        // Empty map levels are removed so best-level lookup only sees executable queues.
        tree->erase(found);
        return;
    }

    if (!price_in_range(price)) {
        return;
    }

    auto& queue = std::get<LadderLevels>(levels)[ladder_index(price)];
    if (queue.empty()) {
        // Empty ladder levels become default queues but their vector slot remains addressable.
        queue = OrderQueue{};
    }
}

/**
 * @brief Returns the mutable best level for one side.
 */
std::optional<OrderBook::PriceLevelRef> OrderBook::best_level(Side side) {
    auto& levels = levels_for(side);

    if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        if (tree->empty()) {
            return std::nullopt;
        }

        // Buy-side best is the highest key; sell-side best is the lowest key.
        const auto best = side == Side::Buy ? std::prev(tree->end()) : tree->begin();
        return PriceLevelRef{.price = best->first, .queue = &best->second};
    }

    auto& ladder = std::get<LadderLevels>(levels);
    if (side == Side::Buy) {
        for (std::size_t index = ladder.size(); index > 0; --index) {
            auto& queue = ladder[index - 1];
            if (!queue.empty()) {
                // Buy-side best scans from the highest price slot down.
                return PriceLevelRef{.price = min_tick_ + static_cast<PriceTick>(index - 1),
                                     .queue = &queue};
            }
        }
    } else {
        for (std::size_t index = 0; index < ladder.size(); ++index) {
            auto& queue = ladder[index];
            if (!queue.empty()) {
                // Sell-side best scans from the lowest price slot up.
                return PriceLevelRef{.price = min_tick_ + static_cast<PriceTick>(index),
                                     .queue = &queue};
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns the immutable best level for one side.
 */
std::optional<OrderBook::ConstPriceLevelRef> OrderBook::best_level(Side side) const {
    const auto& levels = levels_for(side);

    if (const auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        if (tree->empty()) {
            return std::nullopt;
        }

        // Buy-side best is the highest key; sell-side best is the lowest key.
        const auto best = side == Side::Buy ? std::prev(tree->end()) : tree->begin();
        return ConstPriceLevelRef{.price = best->first, .queue = &best->second};
    }

    const auto& ladder = std::get<LadderLevels>(levels);
    if (side == Side::Buy) {
        for (std::size_t index = ladder.size(); index > 0; --index) {
            const auto& queue = ladder[index - 1];
            if (!queue.empty()) {
                // Buy-side best scans from the highest price slot down.
                return ConstPriceLevelRef{.price = min_tick_ + static_cast<PriceTick>(index - 1),
                                          .queue = &queue};
            }
        }
    } else {
        for (std::size_t index = 0; index < ladder.size(); ++index) {
            const auto& queue = ladder[index];
            if (!queue.empty()) {
                // Sell-side best scans from the lowest price slot up.
                return ConstPriceLevelRef{.price = min_tick_ + static_cast<PriceTick>(index),
                                          .queue = &queue};
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Reports whether one side has any non-empty levels.
 */
bool OrderBook::levels_empty(Side side) const {
    const auto& levels = levels_for(side);

    if (const auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        return tree->empty();
    }

    const auto& ladder = std::get<LadderLevels>(levels);
    for (const auto& queue : ladder) {
        if (!queue.empty()) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Counts non-empty levels on one side.
 */
std::size_t OrderBook::level_count(Side side) const {
    const auto& levels = levels_for(side);

    if (const auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        return tree->size();
    }

    std::size_t count = 0;
    for (const auto& queue : std::get<LadderLevels>(levels)) {
        // Ladder vectors retain empty slots, so snapshots count only visible levels.
        count += queue.empty() ? 0 : 1;
    }

    return count;
}

/**
 * @brief Returns levels in best-to-worst order.
 */
std::vector<OrderBook::ConstPriceLevelRef> OrderBook::ordered_levels(Side side) const {
    const auto& levels = levels_for(side);
    std::vector<ConstPriceLevelRef> refs;
    refs.reserve(level_count(side));

    if (const auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
        if (side == Side::Buy) {
            for (auto level = tree->rbegin(); level != tree->rend(); ++level) {
                // Bids are copied from highest to lowest price to match priority order.
                refs.push_back(ConstPriceLevelRef{.price = level->first, .queue = &level->second});
            }
        } else {
            for (auto level = tree->begin(); level != tree->end(); ++level) {
                // Asks are copied from lowest to highest price to match priority order.
                refs.push_back(ConstPriceLevelRef{.price = level->first, .queue = &level->second});
            }
        }

        return refs;
    }

    const auto& ladder = std::get<LadderLevels>(levels);
    if (side == Side::Buy) {
        for (std::size_t index = ladder.size(); index > 0; --index) {
            const auto& queue = ladder[index - 1];
            if (!queue.empty()) {
                // Ladder bid snapshots use the same best-to-worst order as tree books.
                refs.push_back(ConstPriceLevelRef{
                    .price = min_tick_ + static_cast<PriceTick>(index - 1),
                    .queue = &queue});
            }
        }
    } else {
        for (std::size_t index = 0; index < ladder.size(); ++index) {
            const auto& queue = ladder[index];
            if (!queue.empty()) {
                // Ladder ask snapshots use the same best-to-worst order as tree books.
                refs.push_back(ConstPriceLevelRef{.price = min_tick_ + static_cast<PriceTick>(index),
                                                  .queue = &queue});
            }
        }
    }

    return refs;
}

/**
 * @brief Checks whether the opposite side can fill an incoming order.
 */
bool OrderBook::has_crossing_liquidity(const Order& order) const {
    std::uint64_t remaining = order.quantity;

    if (order.side == Side::Buy) {
        for (const auto level : ordered_levels(Side::Sell)) {
            if (level.price > order.price) {
                break;
            }

            // Aggregate volume lets FOK preflight avoid walking each FIFO order.
            if (level.queue->total_volume >= remaining) {
                return true;
            }
            remaining -= level.queue->total_volume;
        }
    } else {
        for (const auto level : ordered_levels(Side::Buy)) {
            if (level.price < order.price) {
                break;
            }

            // Aggregate volume lets FOK preflight avoid walking each FIFO order.
            if (level.queue->total_volume >= remaining) {
                return true;
            }
            remaining -= level.queue->total_volume;
        }
    }

    return false;
}

/**
 * @brief Clears all configured price levels.
 */
void OrderBook::clear_price_levels() noexcept {
    auto clear_side = [](PriceLevels& levels) {
        if (auto* tree = std::get_if<TreeLevels>(&levels); tree != nullptr) {
            // Tree clear removes all price nodes.
            tree->clear();
            return;
        }

        for (auto& queue : std::get<LadderLevels>(levels)) {
            // Ladder clear preserves slot count while dropping all FIFO state.
            queue = OrderQueue{};
        }
    };

    clear_side(bids_);
    clear_side(asks_);
}

} // namespace matching_engine
