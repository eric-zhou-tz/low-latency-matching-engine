#include "exchange.hpp"

#include <variant>

namespace matching_engine {

/**
 * @brief Applies an action and returns the events produced by the exchange.
 *
 * The exchange currently supports one book per symbol. Cancel routing is a
 * placeholder search across books until a global order-id index is introduced.
 */
std::vector<Event> Exchange::process(const Action& action) {
    // Dispatch the variant to the matching process_action overload.
    return std::visit(
        [this](const auto& concrete_action) {
            // Forward the concrete action to the matching overload.
            return process_action(concrete_action);
        },
        action);
}

/**
 * @brief Routes a submit action to the book for its symbol.
 *
 * TODO: Track a global order-id index when cancellation support becomes
 * symbol-aware and matching logic is implemented.
 */
std::vector<Event> Exchange::process_action(const SubmitOrderAction& action) {
    // Create the symbol book on first use, then submit to that book.
    return books_by_symbol_[action.order.symbol].submit(action.order);
}

/**
 * @brief Searches all books for an order to cancel.
 *
 * This is intentionally simple scaffold logic. A production exchange would
 * maintain an id-to-symbol index to avoid scanning every book.
 */
std::vector<Event> Exchange::process_action(const CancelOrderAction& action) {
    // Search books until one recognizes and cancels the order id.
    for (auto& [_, book] : books_by_symbol_) {
        auto events = book.cancel(action.order_id);
        if (!std::holds_alternative<RejectedEvent>(events.front())) {
            return events;
        }
    }

    // No book owned the id, so report one exchange-level rejection.
    return {RejectedEvent{"unknown order id " + std::to_string(action.order_id)}};
}

/**
 * @brief Emits one snapshot event per known book.
 */
std::vector<Event> Exchange::process_action(const PrintBookAction&) const {
    // Build snapshot messages as events so output formatting stays outside Exchange.
    std::vector<Event> events;
    if (books_by_symbol_.empty()) {
        // Preserve a useful response even before any book has been created.
        events.emplace_back(AcceptedEvent{"book: empty"});
        return events;
    }

    // Emit one compact snapshot per symbol book.
    for (const auto& [symbol, book] : books_by_symbol_) {
        events.emplace_back(AcceptedEvent{"book " + symbol + ": " + book.snapshot()});
    }

    // Return snapshots in the unordered_map iteration order for now.
    return events;
}

} // namespace matching_engine
