#include "exchange.hpp"

#include <memory>
#include <string>
#include <variant>

namespace matching_engine {
namespace {

/**
 * @brief Builds the compact book order from a routed submit action.
 */
[[nodiscard]] Order make_book_order(const SubmitOrderAction& action) {
    // Drop the symbol after routing because the destination book already implies it.
    return {.id = action.id,
            .side = action.side,
            .price = action.price,
            .quantity = action.quantity};
}

} // namespace

/**
 * @brief Applies an action and writes the events produced by the exchange.
 *
 * The exchange currently supports one book per symbol and keeps a live order
 * index so cancels can route directly to the owning book.
 */
void Exchange::process(const Action& action, std::vector<Event>& out) {
    // Reuse caller-owned storage across commands to avoid per-action vector churn.
    out.clear();

    // Dispatch the variant to the matching process_action overload.
    std::visit(
        [this, &out](const auto& concrete_action) {
            // Forward the concrete action to the matching overload.
            process_action(concrete_action, out);
        },
        action);
}

/**
 * @brief Routes a submit action to the book for its symbol.
 */
void Exchange::process_action(const SubmitOrderAction& action, std::vector<Event>& out) {
    // Enforce one live owner per order id so the exchange-level cancel index is unambiguous.
    if (order_to_book_.contains(action.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = action.id});
        return;
    }

    // Create the symbol book on first use, then submit to that stable book pointer.
    OrderBook* book = get_or_create_book(action.symbol);
    book->submit(make_book_order(action), out);

    // Trades may have removed previously resting orders from their owning books.
    remove_filled_resting_orders_from_index(out);

    // Rejections leave no live state and should not be added to the cancel index.
    if (std::holds_alternative<RejectedEvent>(out.front())) {
        return;
    }

    // Work out whether the incoming order has remaining quantity after its trades.
    std::uint64_t filled_quantity = 0;
    for (const auto& event : out) {
        if (const auto* trade = std::get_if<TradeEvent>(&event);
            trade != nullptr && trade->incoming_order_id == action.id) {
            filled_quantity += trade->quantity;
        }
    }

    // Only resting remainders can be canceled later, so fully filled takers are not indexed.
    if (filled_quantity < action.quantity) {
        order_to_book_.emplace(action.id, book);
    }
}

/**
 * @brief Routes cancellation directly to the book that owns the order.
 */
void Exchange::process_action(const CancelOrderAction& action, std::vector<Event>& out) {
    // Missing ids are rejected at the exchange boundary without probing symbol books.
    const auto found = order_to_book_.find(action.order_id);
    if (found == order_to_book_.end()) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = action.order_id});
        return;
    }

    // Route directly to the saved owning book.
    const auto result = found->second->cancel(action.order_id);
    out.push_back(std::visit(
        [](const auto& event) {
            // Convert the concrete cancel payload into the wider event stream type.
            return Event{event};
        },
        result));

    // A successful cancel removes the order from both the book and exchange indexes.
    if (!std::holds_alternative<RejectedEvent>(result)) {
        order_to_book_.erase(found);
        return;
    }

    // If the book rejects, the exchange index was stale; erase it to restore consistency.
    order_to_book_.erase(found);
}

/**
 * @brief Emits one snapshot event per known book.
 */
void Exchange::process_action(const PrintBookAction&, std::vector<Event>& out) const {
    // Build snapshot messages as events so output formatting stays outside Exchange.
    if (books_by_symbol_.empty()) {
        // Preserve a useful response even before any book has been created.
        out.push_back(BookSnapshotEvent{"book: empty"});
        return;
    }

    // Emit one compact snapshot per symbol book.
    for (const auto& [symbol, book] : books_by_symbol_) {
        out.push_back(BookSnapshotEvent{"book " + symbol + ": " + book->snapshot()});
    }
}

/**
 * @brief Returns the stable book pointer for a symbol.
 */
OrderBook* Exchange::get_or_create_book(const std::string& symbol) {
    // Look for an existing symbol book before allocating a new one.
    const auto found = books_by_symbol_.find(symbol);
    if (found != books_by_symbol_.end()) {
        return found->second.get();
    }

    // Allocate the book separately so stored OrderBook* values survive symbol-map rehashes.
    auto book = std::make_unique<OrderBook>();
    OrderBook* raw_book = book.get();
    books_by_symbol_.emplace(symbol, std::move(book));
    return raw_book;
}

/**
 * @brief Removes resting order ids that a submit fully consumed.
 */
void Exchange::remove_filled_resting_orders_from_index(const std::vector<Event>& events) {
    // Trade events identify the resting id that may have changed liveness.
    for (const auto& event : events) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (trade == nullptr) {
            continue;
        }

        // The owning book remains the source of truth for partial versus full fills.
        const auto found = order_to_book_.find(trade->resting_order_id);
        if (found != order_to_book_.end() &&
            !found->second->contains_order(trade->resting_order_id)) {
            order_to_book_.erase(found);
        }
    }
}

} // namespace matching_engine
