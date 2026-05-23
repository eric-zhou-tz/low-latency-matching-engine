#include "book/order_book.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace matching_engine {

/**
 * @brief Creates an empty book and reserves order lookup capacity.
 */
OrderBook::OrderBook(std::size_t expected_order_capacity) {
    // Size the flat id index up front to avoid rehashing during known-depth setup.
    reserve_order_capacity(expected_order_capacity);
}

/**
 * @brief Creates an empty book with tuned order lookup density.
 */
OrderBook::OrderBook(std::size_t expected_order_capacity, float order_id_max_load_factor) {
    // Apply the requested density before reserve so bucket sizing uses it.
    set_order_id_max_load_factor(order_id_max_load_factor);
    reserve_order_capacity(expected_order_capacity);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook::OrderBook(const OrderBook& other) {
    // Copy live orders into fresh storage so raw pointers never alias the source.
    copy_from(other);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook& OrderBook::operator=(const OrderBook& other) {
    // Guard self-assignment so current raw pointers are not discarded needlessly.
    if (this == &other) {
        return *this;
    }

    // Drop old storage before rebuilding this book from the source queues.
    clear();
    copy_from(other);

    // Return this object so assignment can be chained normally.
    return *this;
}

/**
 * @brief Moves a book and preserves pointers into transferred order blocks.
 */
OrderBook::OrderBook(OrderBook&& other) noexcept
    : bids_(std::move(other.bids_)),
      asks_(std::move(other.asks_)),
      orders_by_id_(std::move(other.orders_by_id_)),
      order_pool_(std::move(other.order_pool_)) {
    // Raw order pointers remain valid because OrderPool transfers ownership of storage.
}

/**
 * @brief Moves a book and preserves pointers into transferred order blocks.
 */
OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    // Avoid clearing after self-move; otherwise the source and destination match.
    if (this == &other) {
        return *this;
    }

    // Release current state before taking ownership of the other book's blocks.
    clear();
    bids_ = std::move(other.bids_);
    asks_ = std::move(other.asks_);
    orders_by_id_ = std::move(other.orders_by_id_);
    order_pool_ = std::move(other.order_pool_);

    // Return this object so assignment can be chained normally.
    return *this;
}

/**
 * @brief Destroys all owned order storage.
 */
OrderBook::~OrderBook() {
    // Release maps first, then destroy the arena slots they pointed into.
    clear();
}

/**
 * @brief Adds an order to the book, matching any immediately executable volume.
 *
 * Limit orders trade against the best opposite-side prices while they cross the
 * incoming limit. Any unfilled quantity is appended to the relevant resting
 * queue, preserving FIFO priority at that price level.
 */
std::vector<Event> OrderBook::submit(Order order) {
    // Incoming stack orders should never inherit intrusive links from callers.
    order.prev = nullptr;
    order.next = nullptr;

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
 * The id index points directly at the intrusive order node. Cancellation still
 * finds the price level for aggregate cleanup, but FIFO removal is just pointer
 * rewiring and does not allocate or scan through same-price orders.
 */
std::vector<Event> OrderBook::cancel(std::uint64_t order_id) {
    // Look up the live order pointer so cancellation can unlink it directly.
    const auto found = orders_by_id_.find(order_id);
    if (found == orders_by_id_.end()) {
        return {RejectedEvent{"unknown order id " + std::to_string(order_id)}};
    }

    // Keep the order pointer long enough to update the level and recycle storage.
    Order* order = found->second;
    const auto side = order->side;
    const auto price = order->price;

    if (side == Side::Buy) {
        // Find the bid level and unlink the raw order node in constant time.
        auto level = bids_.find(price);
        if (level != bids_.end()) {
            level->second.remove(order);
            // Drop empty levels so best-price lookup stays clean.
            if (level->second.empty()) {
                bids_.erase(level);
            }
        }
    } else {
        // Find the ask level and unlink the raw order node in constant time.
        auto level = asks_.find(price);
        if (level != asks_.end()) {
            level->second.remove(order);
            // Drop empty levels so best-price lookup stays clean.
            if (level->second.empty()) {
                asks_.erase(level);
            }
        }
    }

    // Remove the id index and recycle the order slot after the book is updated.
    orders_by_id_.erase(found);
    order_pool_.release(order);
    return {CanceledEvent{.order_id = order_id}};
}

/**
 * @brief Checks whether an order id is currently live in this book.
 */
bool OrderBook::contains_order(std::uint64_t order_id) const {
    // Ask the book-local id index because it is the source of truth for live orders.
    return orders_by_id_.contains(order_id);
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    // Start with the live-order count for a quick summary.
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    // Print bids in book priority order: highest price, then FIFO within level.
    for (const auto& [_, level] : bids_) {
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    // Print asks in book priority order: lowest price, then FIFO within level.
    for (const auto& [_, level] : asks_) {
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    // Return the accumulated single-line representation.
    return output.str();
}

/**
 * @brief Reserves live order-id lookup capacity.
 */
void OrderBook::reserve_order_capacity(std::size_t expected_order_capacity) {
    // Keep the requested capacity with the id index because cancels start here.
    orders_by_id_.reserve(expected_order_capacity);
}

/**
 * @brief Updates the max load factor used by the order-id map.
 */
void OrderBook::set_order_id_max_load_factor(float order_id_max_load_factor) {
    // Forward the tuning knob to the dense hash map before future reservations.
    orders_by_id_.max_load_factor(order_id_max_load_factor);
}

/**
 * @brief Appends an order to the appropriate price level.
 */
void OrderBook::add_resting_order(const Order& order) {
    // Store the order once, then use its embedded links for FIFO membership.
    Order* stored_order = order_pool_.create(order);
    if (order.side == Side::Buy) {
        // Append to the bid level tail so older same-price orders stay ahead.
        auto& level = bids_[order.price];
        level.push_back(stored_order);
    } else {
        // Append to the ask level tail so older same-price orders stay ahead.
        auto& level = asks_[order.price];
        level.push_back(stored_order);
    }

    // Remember the exact order node so future cancels can unlink without scans.
    orders_by_id_.emplace(stored_order->id, stored_order);
}

/**
 * @brief Clears all price levels, indexes, and arena storage.
 */
void OrderBook::clear() noexcept {
    // Drop index and levels before destroying the order slots they point into.
    bids_.clear();
    asks_.clear();
    orders_by_id_.clear();

    // Reset pooled storage after all raw pointers have been removed.
    order_pool_.clear();
}

/**
 * @brief Copies live resting orders from another book.
 */
void OrderBook::copy_from(const OrderBook& other) {
    // Preserve the source lookup density before sizing this book's id index.
    set_order_id_max_load_factor(other.orders_by_id_.max_load_factor());

    // Reserve the same live-id count so cloning does not rehash mid-copy.
    orders_by_id_.reserve(other.orders_by_id_.size());

    // Copy bids in price-priority order and preserve FIFO links within levels.
    for (const auto& [price, source_level] : other.bids_) {
        auto& target_level = bids_[price];
        for (const Order* source = source_level.head; source != nullptr; source = source->next) {
            Order* clone = order_pool_.create(*source);
            target_level.push_back(clone);
            orders_by_id_.emplace(clone->id, clone);
        }
    }

    // Copy asks in price-priority order and preserve FIFO links within levels.
    for (const auto& [price, source_level] : other.asks_) {
        auto& target_level = asks_[price];
        for (const Order* source = source_level.head; source != nullptr; source = source->next) {
            Order* clone = order_pool_.create(*source);
            target_level.push_back(clone);
            orders_by_id_.emplace(clone->id, clone);
        }
    }
}

/**
 * @brief Consumes resting asks while their price is at or below the buy limit.
 *
 * The front pointer is the oldest order at that price, so matching from front
 * to back preserves FIFO price-time priority.
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
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // Trades execute at the resting order's price.
        events.emplace_back(TradeEvent{.resting_order_id = resting->id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting->price,
                                       .quantity = trade_quantity});

        // Reduce both orders and the aggregate level volume by the fill.
        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        // Fully filled resting orders leave the id index, FIFO queue, and arena.
        if (resting->quantity == 0) {
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
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
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // Trades execute at the resting order's price.
        events.emplace_back(TradeEvent{.resting_order_id = resting->id,
                                       .incoming_order_id = incoming.id,
                                       .price = resting->price,
                                       .quantity = trade_quantity});

        // Reduce both orders and the aggregate level volume by the fill.
        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        // Fully filled resting orders leave the id index, FIFO queue, and arena.
        if (resting->quantity == 0) {
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
        }

        // Remove the price level when its queue has been exhausted.
        if (resting_orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine
