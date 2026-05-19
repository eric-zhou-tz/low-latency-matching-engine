#include "book/order_book.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace matching_engine {

/**
 * @brief Creates an empty book and applies a reserve-capacity tuning hint.
 */
OrderBook::OrderBook(std::size_t reserve_order_capacity) {
    this->reserve_order_capacity(reserve_order_capacity);
}

/**
 * @brief Creates an empty book with tuned lookup density and reserve capacity.
 */
OrderBook::OrderBook(std::size_t reserve_order_capacity, float order_id_max_load_factor) {
    set_order_id_max_load_factor(order_id_max_load_factor);
    this->reserve_order_capacity(reserve_order_capacity);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook::OrderBook(const OrderBook& other) {
    copy_from(other);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook& OrderBook::operator=(const OrderBook& other) {
    if (this == &other) {
        return *this;
    }

    clear();
    copy_from(other);

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
}

/**
 * @brief Moves a book and preserves pointers into transferred order blocks.
 */
OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    bids_ = std::move(other.bids_);
    asks_ = std::move(other.asks_);
    orders_by_id_ = std::move(other.orders_by_id_);
    order_pool_ = std::move(other.order_pool_);

    return *this;
}

/**
 * @brief Destroys all owned order storage.
 */
OrderBook::~OrderBook() {
    clear();
}

/**
 * @brief Adds an order to the book, matching any immediately executable volume.
 *
 * Limit orders trade against the best opposite-side prices while they cross the
 * incoming limit. Any unfilled quantity is appended to the relevant resting
 * queue, preserving FIFO priority at that price level.
 */
void OrderBook::submit(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out)) {
        return;
    }

    // fok must prove full execution before acceptance so partial fills cannot leak.
    if (order.time_in_force == TimeInForce::FillOrKill && !can_fully_fill(order)) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::InsufficientLiquidity, .order_id = order.id});
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});

    execute_incoming_order(order, out);
}

/**
 * @brief Matches a market order and expires any leftover quantity.
 *
 * Market orders reuse the limit matching loops by giving the incoming order an
 * unbounded crossing price. The only market-specific behavior is that an
 * unmatched remainder produces a rejection event instead of resting.
 */
void OrderBook::submit_market(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out)) {
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});

    // market orders use sentinel prices to reuse the same crossing loops but never rest.
    if (order.side == Side::Buy) {
        order.price = std::numeric_limits<Price>::max();
        match_buy_order(order, out);
    } else {
        order.price = std::numeric_limits<Price>::min();
        match_sell_order(order, out);
    }

    if (order.quantity > 0) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::InsufficientLiquidity, .order_id = order.id});
    }
}

/**
 * @brief Removes an order from the current book by id.
 *
 * The id index points directly at the intrusive order node. Cancellation still
 * finds the price level for aggregate cleanup, but FIFO removal is just pointer
 * rewiring and does not allocate or scan through same-price orders.
 */
CancelResult OrderBook::cancel(OrderId order_id) {
    Order* order = find_resting_order(order_id);
    if (order == nullptr) {
        return RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id};
    }

    remove_resting_order(order);
    return CanceledEvent{.order_id = order_id};
}

/**
 * @brief Modifies a live resting order.
 */
void OrderBook::modify(OrderId order_id,
                       Price new_price,
                       Quantity new_quantity,
                       std::vector<Event>& out) {
    out.clear();

    Order* existing = find_resting_order(order_id);
    if (existing == nullptr) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id});
        return;
    }

    if (new_price <= 0 || new_quantity == 0) {
        out.push_back(RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = order_id});
        return;
    }

    const Price old_price = existing->price;
    const Quantity old_quantity = existing->quantity;
    const Side side = existing->side;

    if (new_price == old_price && new_quantity < old_quantity) {
        // a pure size reduction preserves the order's FIFO priority.
        const Quantity reduced_by = old_quantity - new_quantity;
        if (side == Side::Buy) {
            bids_.find(old_price)->second.total_volume -= reduced_by;
        } else {
            asks_.find(old_price)->second.total_volume -= reduced_by;
        }

        existing->quantity = new_quantity;
        out.push_back(ModifiedEvent{.order_id = order_id,
                                    .old_price = old_price,
                                    .new_price = new_price,
                                    .old_quantity = old_quantity,
                                    .new_quantity = new_quantity});
        return;
    }

    // size increases and price changes are cancel-replace operations that lose FIFO priority.
    Order replacement{.id = order_id,
                      .side = side,
                      .price = new_price,
                      .quantity = new_quantity,
                      .time_in_force = TimeInForce::GoodTilCancel};
    remove_resting_order(existing);

    out.push_back(ReplacedEvent{.old_order_id = order_id,
                                .new_order_id = order_id,
                                .old_price = old_price,
                                .new_price = new_price,
                                .old_quantity = old_quantity,
                                .new_quantity = new_quantity});

    execute_incoming_order(replacement, out);
}

/**
 * @brief Checks whether an order id is currently live in this book.
 */
bool OrderBook::contains_order(std::uint64_t order_id) const {
    return orders_by_id_.contains(order_id);
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    for (const auto& [_, level] : bids_) {
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    for (const auto& [_, level] : asks_) {
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    return output.str();
}

/**
 * @brief Copies internal book structure for invariant tests.
 */
OrderBook::DebugSnapshot OrderBook::debug_snapshot() const {
    DebugSnapshot snapshot;
    snapshot.bids.reserve(bids_.size());
    snapshot.asks.reserve(asks_.size());
    snapshot.indexed_order_ids.reserve(orders_by_id_.size());

    for (const auto& [order_id, _] : orders_by_id_) {
        // The id set lets tests compare queue contents against the live lookup index.
        snapshot.indexed_order_ids.insert(order_id);
    }

    for (const auto& [price, source_level] : bids_) {
        DebugPriceLevel level{.price = price, .total_volume = source_level.total_volume};

        for (const Order* order = source_level.head; order != nullptr; order = order->next) {
            // Copy only stable logical fields so tests cannot mutate intrusive links.
            level.orders.push_back(DebugOrder{.id = order->id,
                                              .side = order->side,
                                              .price = order->price,
                                              .quantity = order->quantity});
        }

        snapshot.bids.push_back(std::move(level));
    }

    for (const auto& [price, source_level] : asks_) {
        DebugPriceLevel level{.price = price, .total_volume = source_level.total_volume};

        for (const Order* order = source_level.head; order != nullptr; order = order->next) {
            // Copy only stable logical fields so tests cannot mutate intrusive links.
            level.orders.push_back(DebugOrder{.id = order->id,
                                              .side = order->side,
                                              .price = order->price,
                                              .quantity = order->quantity});
        }

        snapshot.asks.push_back(std::move(level));
    }

    return snapshot;
}

/**
 * @brief Reserves lookup and pool storage from a caller-provided tuning hint.
 */
void OrderBook::reserve_order_capacity(std::size_t reserve_order_capacity) {
    // Reserve sizing is a cache/locality tuning knob, not a count of operations processed.
    orders_by_id_.reserve(reserve_order_capacity);

    // Over-reserving can spread hot structures over more memory and slow steady-state matching.
    order_pool_.reserve(reserve_order_capacity);
}

/**
 * @brief Updates the max load factor used by the order-id map.
 */
void OrderBook::set_order_id_max_load_factor(float order_id_max_load_factor) {
    orders_by_id_.max_load_factor(order_id_max_load_factor);
}

/**
 * @brief Appends an order to the appropriate price level.
 */
void OrderBook::add_resting_order(const Order& order) {
    Order* stored_order = order_pool_.create(order);
    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.push_back(stored_order);
    } else {
        auto& level = asks_[order.price];
        level.push_back(stored_order);
    }

    orders_by_id_.emplace(stored_order->id, stored_order);
}

/**
 * @brief Finds the live resting order for an id.
 */
Order* OrderBook::find_resting_order(OrderId order_id) const {
    const auto found = orders_by_id_.find(order_id);
    if (found == orders_by_id_.end()) {
        return nullptr;
    }

    return found->second;
}

/**
 * @brief Unlinks and releases one resting order.
 */
void OrderBook::remove_resting_order(Order* order) {
    const auto side = order->side;
    const auto price = order->price;

    if (side == Side::Buy) {
        auto level = bids_.find(price);
        if (level != bids_.end()) {
            level->second.remove(order);
            if (level->second.empty()) {
                bids_.erase(level);
            }
        }
    } else {
        auto level = asks_.find(price);
        if (level != asks_.end()) {
            level->second.remove(order);
            if (level->second.empty()) {
                asks_.erase(level);
            }
        }
    }

    orders_by_id_.erase(order->id);
    order_pool_.release(order);
}

/**
 * @brief Resets incoming order links and rejects duplicate live ids.
 */
bool OrderBook::prepare_incoming_order(Order& order, std::vector<Event>& out) const {
    out.clear();

    order.prev = nullptr;
    order.next = nullptr;

    if (orders_by_id_.contains(order.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = order.id});
        return false;
    }

    return true;
}

/**
 * @brief Checks whether crossing liquidity can completely fill an order.
 */
bool OrderBook::can_fully_fill(const Order& order) const {
    std::uint64_t remaining = order.quantity;

    if (order.side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (price > order.price) {
                break;
            }

            // aggregate level volume avoids scanning FIFO orders during fok preflight.
            if (level.total_volume >= remaining) {
                return true;
            }
            remaining -= level.total_volume;
        }
    } else {
        for (const auto& [price, level] : bids_) {
            if (price < order.price) {
                break;
            }

            // aggregate level volume avoids scanning FIFO orders during fok preflight.
            if (level.total_volume >= remaining) {
                return true;
            }
            remaining -= level.total_volume;
        }
    }

    return false;
}

/**
 * @brief Executes the shared limit-order match and rest path.
 */
void OrderBook::execute_incoming_order(Order order, std::vector<Event>& out) {
    if (order.side == Side::Buy) {
        match_buy_order(order, out);
    } else {
        match_sell_order(order, out);
    }

    if (order.quantity > 0 && order.time_in_force == TimeInForce::GoodTilCancel) {
        add_resting_order(order);
    }
}

/**
 * @brief Clears all price levels, indexes, and arena storage.
 */
void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    orders_by_id_.clear();

    order_pool_.clear();
}

/**
 * @brief Copies live resting orders from another book.
 */
void OrderBook::copy_from(const OrderBook& other) {
    set_order_id_max_load_factor(other.orders_by_id_.max_load_factor());

    reserve_order_capacity(other.orders_by_id_.size());

    for (const auto& [price, source_level] : other.bids_) {
        auto& target_level = bids_[price];
        for (const Order* source = source_level.head; source != nullptr; source = source->next) {
            Order* clone = order_pool_.create(*source);
            target_level.push_back(clone);
            orders_by_id_.emplace(clone->id, clone);
        }
    }

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
void OrderBook::match_buy_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !asks_.empty()) {
        auto best_ask = asks_.begin();
        if (best_ask->first > incoming.price) {
            break;
        }

        auto& resting_orders = best_ask->second;
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // executions price at the resting order, preserving exchange-style maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting->id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting->price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        if (resting->quantity == 0) {
            // filled resting orders leave the id index before their pool slot is recycled.
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
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
void OrderBook::match_sell_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !bids_.empty()) {
        auto best_bid = bids_.begin();
        if (best_bid->first < incoming.price) {
            break;
        }

        auto& resting_orders = best_bid->second;
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // executions price at the resting order, preserving exchange-style maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting->id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting->price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        if (resting->quantity == 0) {
            // filled resting orders leave the id index before their pool slot is recycled.
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
        }

        if (resting_orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine
