#include "order_book.hpp"

#include <cassert>
#include <variant>

/**
 * @brief Minimal order book smoke tests.
 */
int main() {
    matching_engine::OrderBook book;
    matching_engine::Order order{.id = 1,
                                 .symbol = "AAPL",
                                 .side = matching_engine::Side::Buy,
                                 .price = 100,
                                 .quantity = 10};

    const auto accepted = book.submit(order);
    assert(accepted.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(accepted.front()));

    const auto duplicate = book.submit(order);
    assert(duplicate.size() == 1);
    assert(std::holds_alternative<matching_engine::RejectedEvent>(duplicate.front()));

    const auto cancelled = book.cancel(order.id);
    assert(cancelled.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(cancelled.front()));

    return 0;
}
