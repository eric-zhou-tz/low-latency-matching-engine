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
    std::vector<matching_engine::Event> events;
    book.submit(std::move(order), events);
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
                                 .side = matching_engine::Side::Buy,
                                 .price = 100,
                                 .quantity = 10};
    std::vector<matching_engine::Event> events;

    book.submit(order, events);
    assert(events.size() == 1);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(events.front()));

    book.submit(order, events);
    assert(events.size() == 1);
    assert(std::holds_alternative<matching_engine::RejectedEvent>(events.front()));

    const auto cancelled = book.cancel(order.id);
    assert(std::holds_alternative<matching_engine::CanceledEvent>(cancelled));

    // Build a mixed book to verify price priority and FIFO ordering in snapshots.
    matching_engine::OrderBook ranked_book;
    submit_accepted(ranked_book, {.id = 1,
                                  .side = matching_engine::Side::Buy,
                                  .price = 100,
                                  .quantity = 10});
    submit_accepted(ranked_book, {.id = 2,
                                  .side = matching_engine::Side::Buy,
                                  .price = 105,
                                  .quantity = 10});
    submit_accepted(ranked_book, {.id = 3,
                                  .side = matching_engine::Side::Buy,
                                  .price = 105,
                                  .quantity = 5});
    submit_accepted(ranked_book, {.id = 4,
                                  .side = matching_engine::Side::Sell,
                                  .price = 106,
                                  .quantity = 7});
    submit_accepted(ranked_book, {.id = 5,
                                  .side = matching_engine::Side::Sell,
                                  .price = 110,
                                  .quantity = 8});

    const auto snapshot = ranked_book.snapshot();
    // Higher bids, lower asks, and older orders at the same price should appear first.
    assert(position_of(snapshot, "[2 BUY 105x10]") <
           position_of(snapshot, "[3 BUY 105x5]"));
    assert(position_of(snapshot, "[2 BUY 105x10]") <
           position_of(snapshot, "[1 BUY 100x10]"));
    assert(position_of(snapshot, "[4 SELL 106x7]") <
           position_of(snapshot, "[5 SELL 110x8]"));

    const auto cancelled_best_bid = ranked_book.cancel(2);
    assert(std::holds_alternative<matching_engine::CanceledEvent>(cancelled_best_bid));

    // After canceling the best bid, the next same-price order keeps priority.
    const auto after_cancel = ranked_book.snapshot();
    assert(after_cancel.find("[2 BUY 105x10]") == std::string::npos);
    assert(position_of(after_cancel, "[3 BUY 105x5]") <
           position_of(after_cancel, "[1 BUY 100x10]"));

    // A crossing buy should trade against the resting sell and leave sell remainder.
    matching_engine::OrderBook buy_match_book;
    submit_accepted(buy_match_book, {.id = 10,
                                     .side = matching_engine::Side::Sell,
                                     .price = 100,
                                     .quantity = 10});

    buy_match_book.submit({.id = 11,
                           .side = matching_engine::Side::Buy,
                           .price = 105,
                           .quantity = 4},
                          events);
    assert(events.size() == 2);
    assert(std::holds_alternative<matching_engine::AcceptedEvent>(events.front()));
    assert_trade(events[1], 10, 11, 100, 4);
    assert(buy_match_book.snapshot().find("[10 SELL 100x6]") != std::string::npos);
    assert(buy_match_book.snapshot().find("[11 BUY 105x4]") == std::string::npos);

    // Same-price sells should fill in FIFO order.
    matching_engine::OrderBook fifo_book;
    submit_accepted(fifo_book, {.id = 20,
                                .side = matching_engine::Side::Sell,
                                .price = 100,
                                .quantity = 5});
    submit_accepted(fifo_book, {.id = 21,
                                .side = matching_engine::Side::Sell,
                                .price = 100,
                                .quantity = 5});

    fifo_book.submit({.id = 22,
                      .side = matching_engine::Side::Buy,
                      .price = 100,
                      .quantity = 7},
                     events);
    assert(events.size() == 3);
    assert_trade(events[1], 20, 22, 100, 5);
    assert_trade(events[2], 21, 22, 100, 2);
    assert(fifo_book.snapshot().find("[20 SELL 100x5]") == std::string::npos);
    assert(fifo_book.snapshot().find("[21 SELL 100x3]") != std::string::npos);

    // A crossing sell should trade against the resting buy and rest its remainder.
    matching_engine::OrderBook sell_match_book;
    submit_accepted(sell_match_book, {.id = 30,
                                      .side = matching_engine::Side::Buy,
                                      .price = 101,
                                      .quantity = 5});

    sell_match_book.submit({.id = 31,
                            .side = matching_engine::Side::Sell,
                            .price = 100,
                            .quantity = 8},
                           events);
    assert(events.size() == 2);
    assert_trade(events[1], 30, 31, 101, 5);
    assert(sell_match_book.snapshot().find("[30 BUY 101x5]") == std::string::npos);
    assert(sell_match_book.snapshot().find("[31 SELL 100x3]") != std::string::npos);

    // All smoke checks passed.
    return 0;
}
