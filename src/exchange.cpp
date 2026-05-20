#include "exchange.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <variant>

namespace matching_engine {
namespace {

/**
 * @brief Builds the compact book order from a routed submit action.
 */
[[nodiscard]] Order make_book_order(const SubmitOrderAction& action) {
    return {.id = action.id,
            .side = action.side,
            .price = action.price,
            .quantity = action.quantity,
            .time_in_force = action.time_in_force};
}

/**
 * @brief Builds the compact book order from a routed market action.
 */
[[nodiscard]] Order make_book_order(const MarketOrderAction& action) {
    return {.id = action.id, .side = action.side, .price = 0, .quantity = action.quantity};
}

} // namespace

/**
 * @brief Creates an exchange that reserves capacity for each new symbol book.
 */
Exchange::Exchange(std::size_t reserve_order_capacity)
    : reserve_order_capacity_{reserve_order_capacity} {
    // Store the per-book hint so each symbol book can reserve on first use.
}

/**
 * @brief Applies an action and writes the events produced by the exchange.
 *
 * The exchange currently supports one book per symbol and keeps a live order
 * index so cancels can route directly to the owning book.
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
 * @brief Registers a symbol with tree mode metadata.
 */
bool Exchange::add_symbol(const std::string& symbol, PriceLevelMode price_level_mode) {
    if (symbol.empty() || books_by_symbol_.contains(symbol) ||
        price_level_mode != PriceLevelMode::Tree) {
        return false;
    }

    // Tree mode is the active implementation and keeps the existing map-backed book.
    auto book = reserve_order_capacity_ > 0 ? std::make_unique<OrderBook>(reserve_order_capacity_)
                                            : std::make_unique<OrderBook>();
    books_by_symbol_.emplace(symbol, std::move(book));
    return true;
}

/**
 * @brief Registers a symbol with vector-backed ladder price storage.
 */
bool Exchange::add_symbol(const std::string& symbol, PriceTick base_tick, PriceTick tick_range) {
    if (symbol.empty() || tick_range < 0 || books_by_symbol_.contains(symbol)) {
        return false;
    }

    // Ladder construction fixes the valid price window for this symbol.
    auto book = std::make_unique<OrderBook>(reserve_order_capacity_, base_tick, tick_range);
    books_by_symbol_.emplace(symbol, std::move(book));
    return true;
}

/**
 * @brief Creates a book from an explicit symbol action.
 */
void Exchange::process_action(const AddSymbolAction& action, std::vector<Event>& out) {
    const bool added = action.price_level_mode == PriceLevelMode::Ladder
                           ? add_symbol(action.symbol, action.base_tick, action.tick_range)
                           : add_symbol(action.symbol, action.price_level_mode);
    if (!added) {
        // ADD_SYMBOL has no order id, so zero is a stable placeholder in the rejection payload.
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateSymbol, .order_id = 0});
    }
}

/**
 * @brief Routes a submit action to the book for its symbol.
 */
void Exchange::process_action(const SubmitOrderAction& action, std::vector<Event>& out) {
    OrderBook* book = find_book(action.symbol);
    if (book == nullptr) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownSymbol, .order_id = action.id});
        return;
    }

    if (order_to_book_.contains(action.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = action.id});
        return;
    }

    book->submit(make_book_order(action), out);

    remove_filled_resting_orders_from_index(out);

    if (std::holds_alternative<RejectedEvent>(out.front())) {
        return;
    }

    std::uint64_t filled_quantity = 0;
    for (const auto& event : out) {
        if (const auto* trade = std::get_if<TradeEvent>(&event);
            trade != nullptr && trade->incoming_order_id == action.id) {
            filled_quantity += trade->quantity;
        }
    }

    if (filled_quantity < action.quantity && action.time_in_force == TimeInForce::GoodTilCancel) {
        order_to_book_.emplace(action.id, book);
    }
}

/**
 * @brief Routes a market action to the book for its symbol.
 */
void Exchange::process_action(const MarketOrderAction& action, std::vector<Event>& out) {
    OrderBook* book = find_book(action.symbol);
    if (book == nullptr) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownSymbol, .order_id = action.id});
        return;
    }

    if (order_to_book_.contains(action.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = action.id});
        return;
    }

    book->submit_market(make_book_order(action), out);

    remove_filled_resting_orders_from_index(out);
}

/**
 * @brief Routes cancellation directly to the book that owns the order.
 */
void Exchange::process_action(const CancelOrderAction& action, std::vector<Event>& out) {
    const auto found = order_to_book_.find(action.order_id);
    if (found == order_to_book_.end()) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = action.order_id});
        return;
    }

    const auto result = found->second->cancel(action.order_id);
    out.push_back(std::visit(
        [](const auto& event) {
            return Event{event};
        },
        result));

    if (std::holds_alternative<RejectedEvent>(result)) {
        // Keep the route in place so a broken exchange/book invariant is not hidden.
        assert(false && "order_to_book_ pointed at a book that rejected the cancel");
        return;
    }

    // Successful cancels remove the now-dead order from the exchange-level route index.
    order_to_book_.erase(found);
}

/**
 * @brief Routes modification directly to the book that owns the order.
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

    const auto current = order_to_book_.find(action.order_id);
    if (book->contains_order(action.order_id)) {
        order_to_book_[action.order_id] = book;
        return;
    }

    if (current != order_to_book_.end()) {
        order_to_book_.erase(current);
    }
}

/**
 * @brief Emits one snapshot event per known book.
 */
void Exchange::process_action(const PrintBookAction&, std::vector<Event>& out) const {
    if (books_by_symbol_.empty()) {
        out.push_back(BookSnapshotEvent{"book: empty"});
        return;
    }

    for (const auto& [symbol, book] : books_by_symbol_) {
        out.push_back(BookSnapshotEvent{"book " + symbol + ": " + book->snapshot()});
    }
}

/**
 * @brief Returns the stable book pointer for a symbol.
 */
OrderBook* Exchange::find_book(const std::string& symbol) const {
    const auto found = books_by_symbol_.find(symbol);
    if (found != books_by_symbol_.end()) {
        return found->second.get();
    }

    return nullptr;
}

/**
 * @brief Removes resting order ids that a submit fully consumed.
 */
void Exchange::remove_filled_resting_orders_from_index(const std::vector<Event>& events) {
    for (const auto& event : events) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (trade == nullptr) {
            continue;
        }

        const auto found = order_to_book_.find(trade->resting_order_id);
        if (found != order_to_book_.end() &&
            !found->second->contains_order(trade->resting_order_id)) {
            order_to_book_.erase(found);
        }
    }
}

} // namespace matching_engine
