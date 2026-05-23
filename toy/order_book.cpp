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

    // FOK preflight scans visible liquidity so partial fills cannot leak on reject.
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
 * @brief Cancels a resting order by scanning the naive std-container book.
 */
CancelResult OrderBook::cancel(OrderId order_id) {
    auto found = find_resting_order(order_id);
    if (!found) {
        return RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id};
    }

    // Removal is intentionally scan/cursor based to model the old simple baseline.
    static_cast<void>(remove_resting_order(*found));
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

    auto found = find_resting_order(order_id);
    if (!found) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id});
        return;
    }

    if (new_price <= 0 || new_quantity == 0) {
        out.push_back(RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = order_id});
        return;
    }

    const Price old_price = found->order->price;
    const Quantity old_quantity = found->order->quantity;
    const Side side = found->order->side;

    if (new_price == old_price && new_quantity < old_quantity) {
        // Same-price quantity reductions preserve FIFO position in the old baseline too.
        found->order->quantity = new_quantity;
        out.push_back(ModifiedEvent{.order_id = order_id,
                                    .old_price = old_price,
                                    .new_price = new_price,
                                    .old_quantity = old_quantity,
                                    .new_quantity = new_quantity});
        return;
    }

    // Price changes and size increases are cancel-replace operations that lose FIFO priority.
    static_cast<void>(remove_resting_order(*found));
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
 * @brief Checks whether a live order exists by scanning every resting queue.
 */
bool OrderBook::contains_order(OrderId order_id) const {
    return find_resting_order(order_id).has_value();
}

/**
 * @brief Produces the same public snapshot text as the optimized book.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    std::size_t live_orders = 0;

    for (const auto& [_, level] : bids_) {
        live_orders += level.orders.size();
    }
    for (const auto& [_, level] : asks_) {
        live_orders += level.orders.size();
    }

    // The count is derived from queues because this baseline has no book-local id index.
    output << "orders=" << live_orders;

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
 * @brief Finds a mutable order by scanning bid queues and then ask queues.
 */
std::optional<OrderBook::OrderCursor> OrderBook::find_resting_order(OrderId order_id) {
    for (auto& [price, level] : bids_) {
        for (auto order = level.orders.begin(); order != level.orders.end(); ++order) {
            // The old std baseline intentionally pays a queue scan for id lookup.
            if (order->id == order_id) {
                return OrderCursor{.side = Side::Buy, .price = price, .order = order};
            }
        }
    }

    for (auto& [price, level] : asks_) {
        for (auto order = level.orders.begin(); order != level.orders.end(); ++order) {
            // Ask queues use the same scan path as bid queues.
            if (order->id == order_id) {
                return OrderCursor{.side = Side::Sell, .price = price, .order = order};
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Finds an immutable order by scanning bid queues and then ask queues.
 */
std::optional<OrderBook::ConstOrderCursor> OrderBook::find_resting_order(OrderId order_id) const {
    for (const auto& [price, level] : bids_) {
        for (auto order = level.orders.begin(); order != level.orders.end(); ++order) {
            // Const scans keep contains_order simple and intentionally linear.
            if (order->id == order_id) {
                return ConstOrderCursor{.side = Side::Buy, .price = price, .order = order};
            }
        }
    }

    for (const auto& [price, level] : asks_) {
        for (auto order = level.orders.begin(); order != level.orders.end(); ++order) {
            // Missing ids scan all visible ask orders before rejecting.
            if (order->id == order_id) {
                return ConstOrderCursor{.side = Side::Sell, .price = price, .order = order};
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Appends remaining quantity to a std::deque at the order's price.
 */
void OrderBook::add_resting_order(const Order& order) {
    auto& level = order.side == Side::Buy ? bids_[order.price] : asks_[order.price];

    // Appending to the deque tail preserves FIFO priority within the price level.
    level.orders.push_back(order);
}

/**
 * @brief Removes an order at a previously discovered cursor.
 */
Order OrderBook::remove_resting_order(const OrderCursor& cursor) {
    if (cursor.side == Side::Buy) {
        auto level = bids_.find(cursor.price);
        if (level == bids_.end()) {
            return {};
        }

        // Copy before erase so callers can inspect the removed logical fields if needed.
        Order removed = *cursor.order;
        level->second.orders.erase(cursor.order);
        if (level->second.orders.empty()) {
            bids_.erase(level);
        }
        return removed;
    }

    auto level = asks_.find(cursor.price);
    if (level == asks_.end()) {
        return {};
    }

    // Ask-side removal mirrors bid-side queue erasure.
    Order removed = *cursor.order;
    level->second.orders.erase(cursor.order);
    if (level->second.orders.empty()) {
        asks_.erase(level);
    }
    return removed;
}

/**
 * @brief Clears output and rejects duplicate resting ids through a full scan.
 */
bool OrderBook::prepare_incoming_order(const Order& order, std::vector<Event>& out) const {
    out.clear();

    if (contains_order(order.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = order.id});
        return false;
    }

    return true;
}

/**
 * @brief Checks crossing visible quantity before a FOK order is accepted.
 */
bool OrderBook::can_fully_fill(const Order& order) const {
    Quantity remaining = order.quantity;

    if (order.side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (price > order.price) {
                break;
            }

            // Sum the queue directly because this baseline keeps no aggregate level volume.
            for (const Order& resting : level.orders) {
                if (resting.quantity >= remaining) {
                    return true;
                }
                remaining -= resting.quantity;
            }
        }
    } else {
        for (const auto& [price, level] : bids_) {
            if (price < order.price) {
                break;
            }

            // Walk bid queues in priority order until enough quantity is visible.
            for (const Order& resting : level.orders) {
                if (resting.quantity >= remaining) {
                    return true;
                }
                remaining -= resting.quantity;
            }
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

        if (resting.quantity == 0) {
            // Filled resting orders leave the front of the naive FIFO queue.
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

        if (resting.quantity == 0) {
            // Filled resting orders leave the front of the naive FIFO queue.
            best_bid->second.orders.pop_front();
        }

        if (best_bid->second.orders.empty()) {
            bids_.erase(best_bid);
        }
    }
}

} // namespace matching_engine::toy
