#include "toy/exchange.hpp"

#include <memory>
#include <utility>
#include <variant>

namespace matching_engine::toy {
namespace {

/**
 * @brief Builds a symbol-free toy book order from a submit action.
 */
[[nodiscard]] Order make_book_order(const SubmitOrderAction& action) {
    return {.id = action.id,
            .side = action.side,
            .price = action.price,
            .quantity = action.quantity,
            .time_in_force = action.time_in_force};
}

/**
 * @brief Builds a symbol-free toy book order from a market action.
 */
[[nodiscard]] Order make_book_order(const MarketOrderAction& action) {
    return {.id = action.id, .side = action.side, .price = 0, .quantity = action.quantity};
}

} // namespace

/**
 * @brief Applies one parsed action to the toy exchange.
 */
void Exchange::process(const Action& action, std::vector<Event>& out) {
    out.clear();

    std::visit(
        [this, &out](const auto& concrete_action) {
            process_action(concrete_action, out);
        },
        action);
}

/**
 * @brief Routes a limit submit to the owning toy symbol book.
 */
void Exchange::process_action(const SubmitOrderAction& action, std::vector<Event>& out) {
    if (order_to_book_.contains(action.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = action.id});
        return;
    }

    OrderBook* book = get_or_create_book(action.symbol);
    book->submit(make_book_order(action), out);

    remove_filled_resting_orders_from_index(out);

    if (!out.empty() && std::holds_alternative<RejectedEvent>(out.front())) {
        return;
    }

    Quantity filled_quantity = 0;
    for (const Event& event : out) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (trade != nullptr && trade->incoming_order_id == action.id) {
            // Only incoming fills determine whether the new GTC order has a resting remainder.
            filled_quantity += trade->quantity;
        }
    }

    if (filled_quantity < action.quantity && action.time_in_force == TimeInForce::GoodTilCancel) {
        order_to_book_.emplace(action.id, book);
    }
}

/**
 * @brief Routes a market submit to the owning toy symbol book.
 */
void Exchange::process_action(const MarketOrderAction& action, std::vector<Event>& out) {
    if (order_to_book_.contains(action.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = action.id});
        return;
    }

    OrderBook* book = get_or_create_book(action.symbol);
    book->submit_market(make_book_order(action), out);

    remove_filled_resting_orders_from_index(out);
}

/**
 * @brief Routes cancellation through the toy exchange live-order index.
 */
void Exchange::process_action(const CancelOrderAction& action, std::vector<Event>& out) {
    const auto found = order_to_book_.find(action.order_id);
    if (found == order_to_book_.end()) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = action.order_id});
        return;
    }

    const CancelResult result = found->second->cancel(action.order_id);
    out.push_back(std::visit(
        [](const auto& event) {
            return Event{event};
        },
        result));

    if (std::holds_alternative<CanceledEvent>(result)) {
        // Successful cancels remove the dead order from the toy route index.
        order_to_book_.erase(found);
    }
}

/**
 * @brief Routes modification through the toy exchange live-order index.
 */
void Exchange::process_action(const ModifyOrderAction& action, std::vector<Event>& out) {
    const auto found = order_to_book_.find(action.order_id);
    if (found == order_to_book_.end()) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = action.order_id});
        return;
    }

    OrderBook* book = found->second;
    book->modify(action.order_id, action.new_price, action.new_quantity, out);
    remove_filled_resting_orders_from_index(out);

    if (book->contains_order(action.order_id)) {
        // Replacement modifies may rest again after trading, so refresh the route.
        order_to_book_[action.order_id] = book;
    } else {
        order_to_book_.erase(action.order_id);
    }
}

/**
 * @brief Emits toy book snapshots in symbol creation order.
 */
void Exchange::process_action(const PrintBookAction&, std::vector<Event>& out) const {
    if (books_by_symbol_.empty()) {
        out.push_back(BookSnapshotEvent{"book: empty"});
        return;
    }

    for (const std::string& symbol : symbol_order_) {
        const auto found = books_by_symbol_.find(symbol);
        if (found != books_by_symbol_.end()) {
            // Symbol order keeps PRINT deterministic even though the index is unordered_map.
            out.push_back(BookSnapshotEvent{"book " + symbol + ": " + found->second->snapshot()});
        }
    }
}

/**
 * @brief Returns an existing toy book or creates one for a new symbol.
 */
OrderBook* Exchange::get_or_create_book(const std::string& symbol) {
    const auto found = books_by_symbol_.find(symbol);
    if (found != books_by_symbol_.end()) {
        return found->second.get();
    }

    auto book = std::make_unique<OrderBook>();
    OrderBook* raw_book = book.get();
    books_by_symbol_.emplace(symbol, std::move(book));
    symbol_order_.push_back(symbol);
    return raw_book;
}

/**
 * @brief Removes filled resting order ids after trade events.
 */
void Exchange::remove_filled_resting_orders_from_index(const std::vector<Event>& events) {
    for (const Event& event : events) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (trade == nullptr) {
            continue;
        }

        const auto found = order_to_book_.find(trade->resting_order_id);
        if (found != order_to_book_.end() &&
            !found->second->contains_order(trade->resting_order_id)) {
            // A fully-filled resting order is no longer cancelable or modifiable.
            order_to_book_.erase(found);
        }
    }
}

} // namespace matching_engine::toy
