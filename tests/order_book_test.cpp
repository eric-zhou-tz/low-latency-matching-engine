#include "order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::CanceledEvent;
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
    // Use one symbol so each test focuses on order-book behavior.
    return {.id = id, .symbol = "AAPL", .side = side, .price = price, .quantity = quantity};
}

void expect_accepted(const Event& event) {
    // Verify the event variant represents a successful action.
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(event));
}

void expect_rejected(const Event& event) {
    // Verify the event variant represents a rejected action.
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(event));
}

void expect_canceled(const Event& event, std::uint64_t order_id) {
    // Check both the event type and the id that was canceled.
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(event));
    EXPECT_EQ(std::get<CanceledEvent>(event).order_id, order_id);
}

void expect_trade(const Event& event,
                  std::uint64_t resting_order_id,
                  std::uint64_t incoming_order_id,
                  std::int64_t price,
                  std::uint64_t quantity) {
    // A trade assertion needs the event to hold a TradeEvent before field checks.
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(event));

    // Compare every observable trade field.
    const auto& trade = std::get<TradeEvent>(event);
    EXPECT_EQ(trade.resting_order_id, resting_order_id);
    EXPECT_EQ(trade.incoming_order_id, incoming_order_id);
    EXPECT_EQ(trade.price, price);
    EXPECT_EQ(trade.quantity, quantity);
}

void submit_accepted(OrderBook& book, Order order) {
    // Submit setup liquidity and require it to rest without trading.
    const auto events = book.submit(std::move(order));
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
}

[[nodiscard]] std::size_t position_of(const std::string& snapshot, const std::string& token) {
    // Locate a snapshot token so tests can compare book ordering.
    const auto position = snapshot.find(token);
    EXPECT_NE(position, std::string::npos);
    return position;
}

} // namespace

TEST(OrderBookTest, AcceptedOrderRestsOnBookWhenNotCrossing) {
    // Submit a passive buy order to an empty book.
    OrderBook book;

    const auto events = book.submit(make_order(1, Side::Buy, 100, 10));

    // The order should be accepted and visible in the snapshot.
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("[1 AAPL BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, DuplicateOrderIdIsRejected) {
    // Seed the book with an order id.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));

    // Reusing the same id must fail even with different order details.
    const auto events = book.submit(make_order(1, Side::Sell, 101, 5));

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, AggressiveBuyMatchesRestingSell) {
    // Put sell liquidity on the book, then submit a crossing buy.
    OrderBook book;
    submit_accepted(book, make_order(10, Side::Sell, 100, 10));

    const auto events = book.submit(make_order(11, Side::Buy, 105, 4));

    // The buy should trade at the resting sell price and leave sell remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 10, 11, 100, 4);
    EXPECT_NE(book.snapshot().find("[10 AAPL SELL 100x6]"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[11 AAPL BUY 105x4]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellMatchesRestingBuy) {
    // Put buy liquidity on the book, then submit a crossing sell.
    OrderBook book;
    submit_accepted(book, make_order(30, Side::Buy, 101, 5));

    const auto events = book.submit(make_order(31, Side::Sell, 100, 8));

    // The sell should trade at the resting bid price and rest the remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 30, 31, 101, 5);
    EXPECT_EQ(book.snapshot().find("[30 AAPL BUY 101x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[31 AAPL SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PriceTimePriorityOlderRestingOrderAtSamePriceFillsFirst) {
    // Two sells at the same price should be matched in insertion order.
    OrderBook book;
    submit_accepted(book, make_order(20, Side::Sell, 100, 5));
    submit_accepted(book, make_order(21, Side::Sell, 100, 5));

    const auto events = book.submit(make_order(22, Side::Buy, 100, 7));

    // The older order fills first, then the newer order receives the leftover fill.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 20, 22, 100, 5);
    expect_trade(events[2], 21, 22, 100, 2);
    EXPECT_EQ(book.snapshot().find("[20 AAPL SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[21 AAPL SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PartialFillLeavesRemainingQuantityCorrectly) {
    // A larger crossing buy should consume the ask and rest the unfilled quantity.
    OrderBook book;
    submit_accepted(book, make_order(40, Side::Sell, 100, 5));

    const auto events = book.submit(make_order(41, Side::Buy, 101, 8));

    // The resting ask disappears and the remaining buy quantity becomes visible.
    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 40, 41, 100, 5);
    EXPECT_EQ(book.snapshot().find("[40 AAPL SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[41 AAPL BUY 101x3]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveBuyWalksAskLevelsAndRestsRemainder) {
    // Seed two ask levels so the incoming order must preserve price priority.
    OrderBook book;
    submit_accepted(book, make_order(42, Side::Sell, 100, 4));
    submit_accepted(book, make_order(43, Side::Sell, 101, 3));

    const auto events = book.submit(make_order(44, Side::Buy, 102, 10));

    // The buy consumes cheaper asks first and only rests its unfilled quantity.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 42, 44, 100, 4);
    expect_trade(events[2], 43, 44, 101, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[42 AAPL SELL 100x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[43 AAPL SELL 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[44 AAPL BUY 102x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellWalksBidLevelsAndRestsRemainder) {
    // Seed two bid levels so the incoming order must preserve price priority.
    OrderBook book;
    submit_accepted(book, make_order(45, Side::Buy, 102, 4));
    submit_accepted(book, make_order(46, Side::Buy, 101, 3));

    const auto events = book.submit(make_order(47, Side::Sell, 100, 10));

    // The sell consumes higher bids first and only rests its unfilled quantity.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 45, 47, 102, 4);
    expect_trade(events[2], 46, 47, 101, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[45 AAPL BUY 102x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[46 AAPL BUY 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[47 AAPL SELL 100x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingBuyOrderSucceeds) {
    // Submit a buy that can be canceled.
    OrderBook book;
    submit_accepted(book, make_order(50, Side::Buy, 100, 10));

    const auto events = book.cancel(50);

    // The cancel event should reference the id and remove it from the snapshot.
    ASSERT_EQ(events.size(), 1U);
    expect_canceled(events.front(), 50);
    EXPECT_EQ(book.snapshot().find("[50 AAPL BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingSellOrderSucceeds) {
    // Submit a sell that can be canceled.
    OrderBook book;
    submit_accepted(book, make_order(51, Side::Sell, 102, 7));

    const auto events = book.cancel(51);

    // The cancel event should reference the id and remove it from the snapshot.
    ASSERT_EQ(events.size(), 1U);
    expect_canceled(events.front(), 51);
    EXPECT_EQ(book.snapshot().find("[51 AAPL SELL 102x7]"), std::string::npos);
}

TEST(OrderBookTest, CancelUnknownOrderIsRejected) {
    // Canceling an id that was never submitted should fail cleanly.
    OrderBook book;

    const auto events = book.cancel(60);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, CancelRemovesEmptyPriceLevel) {
    // Create two bid levels so canceling one level should leave the other intact.
    OrderBook book;
    submit_accepted(book, make_order(70, Side::Buy, 100, 10));
    submit_accepted(book, make_order(71, Side::Buy, 99, 5));

    const auto events = book.cancel(70);

    // The canceled level should disappear while the lower bid remains.
    ASSERT_EQ(events.size(), 1U);
    expect_canceled(events.front(), 70);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[70 AAPL BUY 100x10]"), std::string::npos);
    EXPECT_NE(snapshot.find("[71 AAPL BUY 99x5]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, CancelPreservesFifoPriorityOfRemainingOrders) {
    // Three orders at one price make it easy to remove the middle queue entry.
    OrderBook book;
    submit_accepted(book, make_order(80, Side::Buy, 100, 5));
    submit_accepted(book, make_order(81, Side::Buy, 100, 5));
    submit_accepted(book, make_order(82, Side::Buy, 100, 5));

    const auto cancel_events = book.cancel(81);
    ASSERT_EQ(cancel_events.size(), 1U);
    expect_canceled(cancel_events.front(), 81);

    // Matching should skip the canceled order and preserve the remaining FIFO order.
    const auto match_events = book.submit(make_order(83, Side::Sell, 100, 8));

    ASSERT_EQ(match_events.size(), 3U);
    expect_accepted(match_events.front());
    expect_trade(match_events[1], 80, 83, 100, 5);
    expect_trade(match_events[2], 82, 83, 100, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[80 AAPL BUY 100x5]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[81 AAPL BUY 100x5]"), std::string::npos);
    EXPECT_NE(snapshot.find("[82 AAPL BUY 100x2]"), std::string::npos);
}

TEST(OrderBookTest, CancelAfterPartialFillRemovesOnlyRemainingQuantity) {
    // First create a partial fill that leaves the incoming buy resting.
    OrderBook book;
    submit_accepted(book, make_order(90, Side::Sell, 100, 5));

    const auto submit_events = book.submit(make_order(91, Side::Buy, 101, 8));
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 90, 91, 100, 5);
    EXPECT_NE(book.snapshot().find("[91 AAPL BUY 101x3]"), std::string::npos);

    // Canceling the incoming order should remove only its remaining quantity.
    const auto cancel_events = book.cancel(91);

    ASSERT_EQ(cancel_events.size(), 1U);
    expect_canceled(cancel_events.front(), 91);
    EXPECT_EQ(book.snapshot().find("[91 AAPL BUY 101x3]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, CopiedBookRebuildsCancelLocations) {
    // Seed a book with multiple same-price orders so copied pointers matter.
    OrderBook original;
    submit_accepted(original, make_order(92, Side::Buy, 100, 5));
    submit_accepted(original, make_order(93, Side::Buy, 100, 5));

    // Copying should rebuild the id index to point at the copy's order nodes.
    OrderBook copy = original;
    const auto copy_cancel = copy.cancel(93);

    // Canceling in the copy should succeed without touching the original book.
    ASSERT_EQ(copy_cancel.size(), 1U);
    expect_canceled(copy_cancel.front(), 93);
    EXPECT_EQ(copy.snapshot().find("[93 AAPL BUY 100x5]"), std::string::npos);
    EXPECT_NE(original.snapshot().find("[93 AAPL BUY 100x5]"), std::string::npos);
}

TEST(OrderBookTest, FullyFilledOrdersCannotBeCanceled) {
    // Submit two orders that fully trade and leave no resting quantity.
    OrderBook book;
    submit_accepted(book, make_order(100, Side::Sell, 100, 5));

    const auto submit_events = book.submit(make_order(101, Side::Buy, 100, 5));
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 100, 101, 100, 5);

    // Neither side should be cancelable after the full fill.
    const auto resting_cancel = book.cancel(100);
    ASSERT_EQ(resting_cancel.size(), 1U);
    expect_rejected(resting_cancel.front());

    const auto incoming_cancel = book.cancel(101);
    ASSERT_EQ(incoming_cancel.size(), 1U);
    expect_rejected(incoming_cancel.front());
}

TEST(OrderBookTest, SnapshotPreservesBookOrdering) {
    // Seed multiple prices and multiple orders at one price.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));
    submit_accepted(book, make_order(2, Side::Buy, 105, 10));
    submit_accepted(book, make_order(3, Side::Buy, 105, 5));
    submit_accepted(book, make_order(4, Side::Sell, 106, 7));
    submit_accepted(book, make_order(5, Side::Sell, 110, 8));

    const auto snapshot = book.snapshot();

    // Snapshot order should follow price priority, then FIFO within each level.
    EXPECT_LT(position_of(snapshot, "[2 AAPL BUY 105x10]"),
              position_of(snapshot, "[3 AAPL BUY 105x5]"));
    EXPECT_LT(position_of(snapshot, "[2 AAPL BUY 105x10]"),
              position_of(snapshot, "[1 AAPL BUY 100x10]"));
    EXPECT_LT(position_of(snapshot, "[4 AAPL SELL 106x7]"),
              position_of(snapshot, "[5 AAPL SELL 110x8]"));
}
