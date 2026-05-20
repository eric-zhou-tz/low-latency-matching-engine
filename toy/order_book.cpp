#include "toy/order_book.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace matching_engine::toy {

/**
 * @brief Adds a limit order and emits accept/trade events.
 */
void OrderBook::submit(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out)) {
        return;
    }

    // FOK preflight keeps the toy baseline from emitting partial fills on reject.
    if (order.time_in_force == TimeInForce::FillOrKill && !can_fully_fill(order)) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::InsufficientLiquidity, .order_id = order.id});
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});
    execute_incoming_order(order, out);
}

/**
 * @brief Matches a market order against all currently available opposite liquidity.
 */
void OrderBook::submit_market(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out)) {
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});

    // Sentinel prices let the simple limit matching loops consume all opposite prices.
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
 * @brief Cancels a resting order with straightforward queue removal.
 */
CancelResult OrderBook::cancel(OrderId order_id) {
    if (!contains_order(order_id)) {
        return RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id};
    }

    // The returned order is not needed for public cancel output.
    static_cast<void>(remove_resting_order(order_id));
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
        // Reducing size in place preserves FIFO priority and only updates aggregate volume.
        const Quantity reduced_by = old_quantity - new_quantity;
        auto& level = side == Side::Buy ? bids_.find(old_price)->second : asks_.find(old_price)->second;
        level.total_volume -= reduced_by;
        existing->quantity = new_quantity;
        out.push_back(ModifiedEvent{.order_id = order_id,
                                    .old_price = old_price,
                                    .new_price = new_price,
                                    .old_quantity = old_quantity,
                                    .new_quantity = new_quantity});
        return;
    }

    // Price changes and size increases lose FIFO priority through cancel-replace semantics.
    // Capture old fields before removal; the removed value is not otherwise needed.
    static_cast<void>(remove_resting_order(order_id));
    out.push_back(ReplacedEvent{.old_order_id = order_id,
                                .new_order_id = order_id,
                                .old_price = old_price,
                                .new_price = new_price,
                                .old_quantity = old_quantity,
                                .new_quantity = new_quantity});

    execute_incoming_order(Order{.id = order_id,
                                 .side = side,
                                 .price = new_price,
                                 .quantity = new_quantity,
                                 .time_in_force = TimeInForce::GoodTilCancel},
                           out);
}

/**
 * @brief Checks whether the toy id index contains a live order.
 */
bool OrderBook::contains_order(OrderId order_id) const {
    return orders_by_id_.contains(order_id);
}

/**
 * @brief Produces the same public snapshot text as the optimized book.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    for (const auto& [_, level] : bids_) {
        for (const Order& order : level.orders) {
            output << " [" << order.id << ' ' << to_string(order.side) << ' ' << order.price
                   << 'x' << order.quantity << ']';
        }
    }

    for (const auto& [_, level] : asks_) {
        for (const Order& order : level.orders) {
            output << " [" << order.id << ' ' << to_string(order.side) << ' ' << order.price
                   << 'x' << order.quantity << ']';
        }
    }

    return output.str();
}

/**
 * @brief Finds a mutable order by scanning the recorded price level.
 */
Order* OrderBook::find_resting_order(OrderId order_id) {
    const auto found = orders_by_id_.find(order_id);
    if (found == orders_by_id_.end()) {
        return nullptr;
    }

    auto& level = found->second.side == Side::Buy ? bids_.find(found->second.price)->second
                                                  : asks_.find(found->second.price)->second;
    for (Order& order : level.orders) {
        // The toy baseline uses a simple queue scan instead of direct node pointers.
        if (order.id == order_id) {
            return &order;
        }
    }

    return nullptr;
}

/**
 * @brief Finds an immutable order by scanning the recorded price level.
 */
const Order* OrderBook::find_resting_order(OrderId order_id) const {
    const auto found = orders_by_id_.find(order_id);
    if (found == orders_by_id_.end()) {
        return nullptr;
    }

    const auto& level = found->second.side == Side::Buy ? bids_.find(found->second.price)->second
                                                        : asks_.find(found->second.price)->second;
    for (const Order& order : level.orders) {
        // The toy baseline intentionally pays linear lookup cost inside the level.
        if (order.id == order_id) {
            return &order;
        }
    }

    return nullptr;
}

/**
 * @brief Appends remaining quantity to a std::deque at the order's price.
 */
void OrderBook::add_resting_order(const Order& order) {
    auto& level = order.side == Side::Buy ? bids_[order.price] : asks_[order.price];
    level.orders.push_back(order);
    level.total_volume += order.quantity;
    orders_by_id_.emplace(order.id, OrderLocation{.side = order.side, .price = order.price});
}

/**
 * @brief Removes an order by scanning its recorded price queue.
 */
Order OrderBook::remove_resting_order(OrderId order_id) {
    const auto location = orders_by_id_.at(order_id);
    if (location.side == Side::Buy) {
        auto level = bids_.find(location.price);
        for (auto order = level->second.orders.begin(); order != level->second.orders.end(); ++order) {
            if (order->id != order_id) {
                continue;
            }

            // Keep aggregate volume in sync with the erased queue entry.
            Order removed = *order;
            level->second.total_volume -= order->quantity;
            level->second.orders.erase(order);
            orders_by_id_.erase(order_id);

            if (level->second.orders.empty()) {
                bids_.erase(level);
            }

            return removed;
        }
    } else {
        auto level = asks_.find(location.price);
        for (auto order = level->second.orders.begin(); order != level->second.orders.end(); ++order) {
            if (order->id != order_id) {
                continue;
            }

            // Keep aggregate volume in sync with the erased queue entry.
            Order removed = *order;
            level->second.total_volume -= order->quantity;
            level->second.orders.erase(order);
            orders_by_id_.erase(order_id);

            if (level->second.orders.empty()) {
                asks_.erase(level);
            }

            return removed;
        }
    }

    return {};
}

/**
 * @brief Clears output and rejects duplicate resting ids.
 */
bool OrderBook::prepare_incoming_order(const Order& order, std::vector<Event>& out) const {
    out.clear();

    if (orders_by_id_.contains(order.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = order.id});
        return false;
    }

    return true;
}

/**
 * @brief Checks crossing aggregate volume before a FOK order is accepted.
 */
bool OrderBook::can_fully_fill(const Order& order) const {
    Quantity remaining = order.quantity;

    if (order.side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (price > order.price) {
                break;
            }

            // Aggregate level volume is enough for correctness even in the toy baseline.
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

            // Walk price levels in priority order until enough quantity is visible.
            if (level.total_volume >= remaining) {
                return true;
            }
            remaining -= level.total_volume;
        }
    }

    return false;
}

/**
 * @brief Executes limit matching and rests any GTC remainder.
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
 * @brief Consumes resting asks while their prices cross the buy limit.
 */
void OrderBook::match_buy_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !asks_.empty()) {
        auto best_ask = asks_.begin();
        if (best_ask->first > incoming.price) {
            break;
        }

        Order& resting = best_ask->second.orders.front();
        const Quantity trade_quantity = std::min(incoming.quantity, resting.quantity);

        // Trades price at the resting order, matching fast-mode maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting.id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting.price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;
        best_ask->second.total_volume -= trade_quantity;

        if (resting.quantity == 0) {
            // Filled resting orders leave both the queue and the toy id index.
            orders_by_id_.erase(resting.id);
            best_ask->second.orders.pop_front();
        }

        if (best_ask->second.orders.empty()) {
            asks_.erase(best_ask);
        }
    }
}

/**
 * @brief Consumes resting bids while their prices cross the sell limit.
 */
void OrderBook::match_sell_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !bids_.empty()) {
        auto best_bid = bids_.begin();
        if (best_bid->first < incoming.price) {
            break;
        }

        Order& resting = best_bid->second.orders.front();
        const Quantity trade_quantity = std::min(incoming.quantity, resting.quantity);

        // Trades price at the resting order, matching fast-mode maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting.id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting.price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting.quantity -= trade_quantity;
        best_bid->second.total_volume -= trade_quantity;

        if (resting.quantity == 0) {
            // Filled resting orders leave both the queue and the toy id index.
            orders_by_id_.erase(resting.id);
            best_bid->second.orders.pop_front();
        }

        if (best_bid->second.orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine::toy
