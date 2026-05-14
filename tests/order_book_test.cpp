#include "order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::Event;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::RejectedEvent;
using matching_engine::Side;
using matching_engine::TradeEvent;

[[nodiscard]] Order make_order(std::uint64_t id,
                               Side side,
                               std::int64_t price,
                               std::uint64_t quantity) {
    return {.id = id, .symbol = "AAPL", .side = side, .price = price, .quantity = quantity};
}

void expect_accepted(const Event& event) {
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(event));
}

void expect_rejected(const Event& event) {
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(event));
}

void expect_trade(const Event& event,
                  std::uint64_t resting_order_id,
                  std::uint64_t incoming_order_id,
                  std::int64_t price,
                  std::uint64_t quantity) {
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(event));

    const auto& trade = std::get<TradeEvent>(event);
    EXPECT_EQ(trade.resting_order_id, resting_order_id);
    EXPECT_EQ(trade.incoming_order_id, incoming_order_id);
    EXPECT_EQ(trade.price, price);
    EXPECT_EQ(trade.quantity, quantity);
}

void submit_accepted(OrderBook& book, Order order) {
    const auto events = book.submit(std::move(order));
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
}

[[nodiscard]] std::size_t position_of(const std::string& snapshot, const std::string& token) {
    const auto position = snapshot.find(token);
    EXPECT_NE(position, std::string::npos);
    return position;
}

} // namespace

TEST(OrderBookTest, AcceptedOrderRestsOnBookWhenNotCrossing) {
    OrderBook book;

    const auto events = book.submit(make_order(1, Side::Buy, 100, 10));

    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("[1 AAPL BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, DuplicateOrderIdIsRejected) {
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));

    const auto events = book.submit(make_order(1, Side::Sell, 101, 5));

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, AggressiveBuyMatchesRestingSell) {
    OrderBook book;
    submit_accepted(book, make_order(10, Side::Sell, 100, 10));

    const auto events = book.submit(make_order(11, Side::Buy, 105, 4));

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 10, 11, 100, 4);
    EXPECT_NE(book.snapshot().find("[10 AAPL SELL 100x6]"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[11 AAPL BUY 105x4]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellMatchesRestingBuy) {
    OrderBook book;
    submit_accepted(book, make_order(30, Side::Buy, 101, 5));

    const auto events = book.submit(make_order(31, Side::Sell, 100, 8));

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 30, 31, 101, 5);
    EXPECT_EQ(book.snapshot().find("[30 AAPL BUY 101x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[31 AAPL SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PriceTimePriorityOlderRestingOrderAtSamePriceFillsFirst) {
    OrderBook book;
    submit_accepted(book, make_order(20, Side::Sell, 100, 5));
    submit_accepted(book, make_order(21, Side::Sell, 100, 5));

    const auto events = book.submit(make_order(22, Side::Buy, 100, 7));

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 20, 22, 100, 5);
    expect_trade(events[2], 21, 22, 100, 2);
    EXPECT_EQ(book.snapshot().find("[20 AAPL SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[21 AAPL SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PartialFillLeavesRemainingQuantityCorrectly) {
    OrderBook book;
    submit_accepted(book, make_order(40, Side::Sell, 100, 5));

    const auto events = book.submit(make_order(41, Side::Buy, 101, 8));

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 40, 41, 100, 5);
    EXPECT_EQ(book.snapshot().find("[40 AAPL SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[41 AAPL BUY 101x3]"), std::string::npos);
}

TEST(OrderBookTest, CancelExistingOrderSucceeds) {
    OrderBook book;
    submit_accepted(book, make_order(50, Side::Buy, 100, 10));

    const auto events = book.cancel(50);

    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_EQ(book.snapshot().find("[50 AAPL BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, CancelUnknownOrderIsRejected) {
    OrderBook book;

    const auto events = book.cancel(60);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, SnapshotPreservesBookOrdering) {
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));
    submit_accepted(book, make_order(2, Side::Buy, 105, 10));
    submit_accepted(book, make_order(3, Side::Buy, 105, 5));
    submit_accepted(book, make_order(4, Side::Sell, 106, 7));
    submit_accepted(book, make_order(5, Side::Sell, 110, 8));

    const auto snapshot = book.snapshot();

    EXPECT_LT(position_of(snapshot, "[2 AAPL BUY 105x10]"),
              position_of(snapshot, "[3 AAPL BUY 105x5]"));
    EXPECT_LT(position_of(snapshot, "[2 AAPL BUY 105x10]"),
              position_of(snapshot, "[1 AAPL BUY 100x10]"));
    EXPECT_LT(position_of(snapshot, "[4 AAPL SELL 106x7]"),
              position_of(snapshot, "[5 AAPL SELL 110x8]"));
}
