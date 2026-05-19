#pragma once

#include "book/order_book.hpp"
#include "core/action.hpp"
#include "core/event.hpp"

#include <ankerl/unordered_dense.h>

#include <memory>
#include <string>
#include <vector>

namespace matching_engine {

/**
 * @brief Routes parsed actions to the appropriate order book.
 *
 * Exchange is the application boundary for command processing. It owns books by
 * symbol and writes events to caller-owned buffers instead of printing directly.
 */
class Exchange {
public:
    /**
     * @brief Applies one action to the exchange.
     *
     * @param action Parsed command to execute.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process(const Action& action, std::vector<Event>& out);

private:
    /**
     * @brief Handles a new-order submission.
     *
     * @param action Submit action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const SubmitOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles a market-order submission.
     *
     * @param action Market action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const MarketOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles an order cancellation.
     *
     * @param action Cancel action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const CancelOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles an order modification.
     *
     * @param action Modify action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const ModifyOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles a book snapshot request.
     *
     * @param action Print action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const PrintBookAction& action, std::vector<Event>& out) const;

    /**
     * @brief Returns the existing book for a symbol or creates it.
     *
     * @param symbol Symbol whose book should receive an action.
     * @return Stable pointer to the owned book.
     */
    [[nodiscard]] OrderBook* get_or_create_book(const std::string& symbol);

    /**
     * @brief Removes filled resting orders from the exchange-level live index.
     *
     * @param events Events emitted by a submit.
     */
    void remove_filled_resting_orders_from_index(const std::vector<Event>& events);

    // Books are heap-owned so OrderBook* values stored in order_to_book_ remain
    // stable even when the symbol map rehashes or moves unique_ptr elements.
    ankerl::unordered_dense::map<std::string, std::unique_ptr<OrderBook>> books_by_symbol_;

    // Live cancel routing starts here: order id to the single-symbol book that
    // owns the resting order.
    ankerl::unordered_dense::map<OrderId, OrderBook*> order_to_book_;
};

} // namespace matching_engine
