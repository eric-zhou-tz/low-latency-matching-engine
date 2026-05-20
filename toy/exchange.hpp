#pragma once

#include "core/action.hpp"
#include "core/event.hpp"
#include "toy/order_book.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine::toy {

/**
 * @brief Simple std-container exchange used by the toy baseline model.
 *
 * The toy exchange owns one toy order book per symbol and routes parsed actions
 * without depending on optimized book internals.
 */
class Exchange {
public:
    /**
     * @brief Applies one parsed action to the toy exchange.
     *
     * @param action Parsed command to execute.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process(const Action& action, std::vector<Event>& out);

private:
    /**
     * @brief Handles a new limit-order submission.
     *
     * @param action Submit action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const SubmitOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles a new market-order submission.
     *
     * @param action Market action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const MarketOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles a cancel request.
     *
     * @param action Cancel action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const CancelOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Handles a modify request.
     *
     * @param action Modify action to apply.
     * @param out Caller-owned event buffer filled with emitted events.
     */
    void process_action(const ModifyOrderAction& action, std::vector<Event>& out);

    /**
     * @brief Emits snapshots for all known symbol books.
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
     * @brief Removes filled resting orders from the exchange-level route index.
     *
     * @param events Events emitted by matching.
     */
    void remove_filled_resting_orders_from_index(const std::vector<Event>& events);

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_by_symbol_;
    std::vector<std::string> symbol_order_;
    std::unordered_map<OrderId, OrderBook*> order_to_book_;
};

} // namespace matching_engine::toy
