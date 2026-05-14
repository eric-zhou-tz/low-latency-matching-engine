#include "order_book.hpp"

#include <cassert>
#include <string>
#include <variant>

namespace {

/**
 * @brief Submits an order and verifies that it was accepted.
 */
void submit_accepted(matching_engine::OrderBook& book, matching_engine::Order order) {
    const auto events = book.submit(std::move(order));
    assert(events.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(events.front()));
}

/**
 * @brief Finds the position of a token in a snapshot string.
 */
[[nodiscard]] std::size_t position_of(const std::string& snapshot, const std::string& token) {
    const auto position = snapshot.find(token);
    assert(position != std::string::npos);
    return position;
}

} // namespace

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

    matching_engine::OrderBook ranked_book;
    submit_accepted(ranked_book, {.id = 1,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Buy,
                                  .price = 100,
                                  .quantity = 10});
    submit_accepted(ranked_book, {.id = 2,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Buy,
                                  .price = 105,
                                  .quantity = 10});
    submit_accepted(ranked_book, {.id = 3,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Buy,
                                  .price = 105,
                                  .quantity = 5});
    submit_accepted(ranked_book, {.id = 4,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Sell,
                                  .price = 99,
                                  .quantity = 7});
    submit_accepted(ranked_book, {.id = 5,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Sell,
                                  .price = 101,
                                  .quantity = 8});

    const auto snapshot = ranked_book.snapshot();
    assert(position_of(snapshot, "[2 AAPL BUY 105x10]") <
           position_of(snapshot, "[3 AAPL BUY 105x5]"));
    assert(position_of(snapshot, "[2 AAPL BUY 105x10]") <
           position_of(snapshot, "[1 AAPL BUY 100x10]"));
    assert(position_of(snapshot, "[4 AAPL SELL 99x7]") <
           position_of(snapshot, "[5 AAPL SELL 101x8]"));

    const auto cancelled_best_bid = ranked_book.cancel(2);
    assert(cancelled_best_bid.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(cancelled_best_bid.front()));

    const auto after_cancel = ranked_book.snapshot();
    assert(after_cancel.find("[2 AAPL BUY 105x10]") == std::string::npos);
    assert(position_of(after_cancel, "[3 AAPL BUY 105x5]") <
           position_of(after_cancel, "[1 AAPL BUY 100x10]"));

    return 0;
}
