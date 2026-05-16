#include "book/order_book.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <variant>

namespace {

/**
 * @brief Submits an order and verifies that it was accepted.
 */
void submit_accepted(matching_engine::OrderBook& book, matching_engine::Order order) {
    // Submit setup liquidity that should rest without matching.
    const auto events = book.submit(std::move(order));
    // The helper only accepts the simple one-event success path.
    assert(events.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(events.front()));
}

/**
 * @brief Finds the position of a token in a snapshot string.
 */
[[nodiscard]] std::size_t position_of(const std::string& snapshot, const std::string& token) {
    // Find a rendered order token so ordering can be compared by position.
    const auto position = snapshot.find(token);
    assert(position != std::string::npos);
    return position;
}

/**
 * @brief Verifies the fields of a trade event.
 */
void assert_trade(const matching_engine::Event& event,
                  std::uint64_t resting_order_id,
                  std::uint64_t incoming_order_id,
                  std::int64_t price,
                  std::uint64_t quantity) {
    // Pull out the trade payload and compare all observable fields.
    const auto& trade = std::get<matching_engine::TradeEvent>(event);
    assert(trade.resting_order_id == resting_order_id);
    assert(trade.incoming_order_id == incoming_order_id);
    assert(trade.price == price);
    assert(trade.quantity == quantity);
}

} // namespace

/**
 * @brief Minimal order book smoke tests.
 */
int main() {
    // Start with basic accept, duplicate reject, and cancel behavior.
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
    assert(std::holds_alternative<matching_engine::CanceledEvent>(cancelled.front()));

    // Build a mixed book to verify price priority and FIFO ordering in snapshots.
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
                                  .price = 106,
                                  .quantity = 7});
    submit_accepted(ranked_book, {.id = 5,
                                  .symbol = "AAPL",
                                  .side = matching_engine::Side::Sell,
                                  .price = 110,
                                  .quantity = 8});

    const auto snapshot = ranked_book.snapshot();
    // Higher bids, lower asks, and older orders at the same price should appear first.
    assert(position_of(snapshot, "[2 AAPL BUY 105x10]") <
           position_of(snapshot, "[3 AAPL BUY 105x5]"));
    assert(position_of(snapshot, "[2 AAPL BUY 105x10]") <
           position_of(snapshot, "[1 AAPL BUY 100x10]"));
    assert(position_of(snapshot, "[4 AAPL SELL 106x7]") <
           position_of(snapshot, "[5 AAPL SELL 110x8]"));

    const auto cancelled_best_bid = ranked_book.cancel(2);
    assert(cancelled_best_bid.size() == 1);
    assert(std::holds_alternative<matching_engine::CanceledEvent>(cancelled_best_bid.front()));

    // After canceling the best bid, the next same-price order keeps priority.
    const auto after_cancel = ranked_book.snapshot();
    assert(after_cancel.find("[2 AAPL BUY 105x10]") == std::string::npos);
    assert(position_of(after_cancel, "[3 AAPL BUY 105x5]") <
           position_of(after_cancel, "[1 AAPL BUY 100x10]"));

    // A crossing buy should trade against the resting sell and leave sell remainder.
    matching_engine::OrderBook buy_match_book;
    submit_accepted(buy_match_book, {.id = 10,
                                     .symbol = "AAPL",
                                     .side = matching_engine::Side::Sell,
                                     .price = 100,
                                     .quantity = 10});

    const auto buy_match = buy_match_book.submit({.id = 11,
                                                  .symbol = "AAPL",
                                                  .side = matching_engine::Side::Buy,
                                                  .price = 105,
                                                  .quantity = 4});
    assert(buy_match.size() == 2);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(buy_match.front()));
    assert_trade(buy_match[1], 10, 11, 100, 4);
    assert(buy_match_book.snapshot().find("[10 AAPL SELL 100x6]") != std::string::npos);
    assert(buy_match_book.snapshot().find("[11 AAPL BUY 105x4]") == std::string::npos);

    // Same-price sells should fill in FIFO order.
    matching_engine::OrderBook fifo_book;
    submit_accepted(fifo_book, {.id = 20,
                                .symbol = "AAPL",
                                .side = matching_engine::Side::Sell,
                                .price = 100,
                                .quantity = 5});
    submit_accepted(fifo_book, {.id = 21,
                                .symbol = "AAPL",
                                .side = matching_engine::Side::Sell,
                                .price = 100,
                                .quantity = 5});

    const auto fifo_match = fifo_book.submit({.id = 22,
                                              .symbol = "AAPL",
                                              .side = matching_engine::Side::Buy,
                                              .price = 100,
                                              .quantity = 7});
    assert(fifo_match.size() == 3);
    assert_trade(fifo_match[1], 20, 22, 100, 5);
    assert_trade(fifo_match[2], 21, 22, 100, 2);
    assert(fifo_book.snapshot().find("[20 AAPL SELL 100x5]") == std::string::npos);
    assert(fifo_book.snapshot().find("[21 AAPL SELL 100x3]") != std::string::npos);

    // A crossing sell should trade against the resting buy and rest its remainder.
    matching_engine::OrderBook sell_match_book;
    submit_accepted(sell_match_book, {.id = 30,
                                      .symbol = "AAPL",
                                      .side = matching_engine::Side::Buy,
                                      .price = 101,
                                      .quantity = 5});

    const auto sell_match = sell_match_book.submit({.id = 31,
                                                    .symbol = "AAPL",
                                                    .side = matching_engine::Side::Sell,
                                                    .price = 100,
                                                    .quantity = 8});
    assert(sell_match.size() == 2);
    assert_trade(sell_match[1], 30, 31, 101, 5);
    assert(sell_match_book.snapshot().find("[30 AAPL BUY 101x5]") == std::string::npos);
    assert(sell_match_book.snapshot().find("[31 AAPL SELL 100x3]") != std::string::npos);

    // All smoke checks passed.
    return 0;
}
