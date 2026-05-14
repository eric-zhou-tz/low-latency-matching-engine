#pragma once

#include "action.hpp"
#include "event.hpp"
#include "order_book.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine {

/**
 * @brief Routes parsed actions to the appropriate order book.
 *
 * Exchange is the application boundary for command processing. It owns books by
 * symbol and returns events to callers instead of printing directly.
 */
class Exchange {
public:
    /**
     * @brief Applies one action to the exchange.
     *
     * @param action Parsed command to execute.
     * @return Events emitted by the command.
     */
    [[nodiscard]] std::vector<Event> process(const Action& action);

private:
    /**
     * @brief Handles a new-order submission.
     *
     * @param action Submit action to apply.
     * @return Events emitted while applying the action.
     */
    [[nodiscard]] std::vector<Event> process_action(const SubmitOrderAction& action);

    /**
     * @brief Handles an order cancellation.
     *
     * @param action Cancel action to apply.
     * @return Events emitted while applying the action.
     */
    [[nodiscard]] std::vector<Event> process_action(const CancelOrderAction& action);

    /**
     * @brief Handles a book snapshot request.
     *
     * @param action Print action to apply.
     * @return Events emitted while applying the action.
     */
    [[nodiscard]] std::vector<Event> process_action(const PrintBookAction& action) const;

    std::unordered_map<std::string, OrderBook> books_by_symbol_;
};

} // namespace matching_engine
