#include "book/order_book.hpp"
#include "exchange.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::Action;
using matching_engine::BookSnapshotEvent;
using matching_engine::CancelOrderAction;
using matching_engine::CanceledEvent;
using matching_engine::Event;
using matching_engine::Exchange;
using matching_engine::MarketOrderAction;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::PrintBookAction;
using matching_engine::RejectedEvent;
using matching_engine::RejectReason;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;
using matching_engine::TimeInForce;
using matching_engine::TradeEvent;

/**
 * @brief Builds a single-symbol order for direct book tests.
 */
[[nodiscard]] Order make_order(std::uint64_t id,
                               Side side,
                               std::int64_t price,
                               std::uint64_t quantity,
                               TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Keep test setup explicit so each regression shows only the behavior under test.
    return {.id = id,
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

/**
 * @brief Builds a submit action for exchange-level symbol routing tests.
 */
[[nodiscard]] SubmitOrderAction make_submit(
    std::uint64_t id,
    std::string symbol,
    Side side,
    std::int64_t price,
    std::uint64_t quantity,
    TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Include symbol only at the exchange boundary because OrderBook is single-symbol.
    return {.id = id,
            .symbol = std::move(symbol),
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

/**
 * @brief Builds a market action for exchange-level tests.
 */
[[nodiscard]] MarketOrderAction make_market(std::uint64_t id,
                                            std::string symbol,
                                            Side side,
                                            std::uint64_t quantity) {
    // Market orders intentionally omit price because the book supplies crossing sentinels.
    return {.id = id, .symbol = std::move(symbol), .side = side, .quantity = quantity};
}

/**
 * @brief Verifies an accepted event.
 */
void expect_accepted(const Event& event, std::uint64_t order_id) {
    // Check the variant type before reading the event payload.
    ASSERT_TRUE(std::holds_alternative<AcceptedEvent>(event));
    EXPECT_EQ(std::get<AcceptedEvent>(event).order_id, order_id);
}

/**
 * @brief Verifies a rejected event.
 */
void expect_rejected(const Event& event, RejectReason reason, std::uint64_t order_id) {
    // Check the exact reason so regressions cannot pass with a generic rejection.
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(event));
    const auto& rejected = std::get<RejectedEvent>(event);
    EXPECT_EQ(rejected.reason, reason);
    EXPECT_EQ(rejected.order_id, order_id);
}

/**
 * @brief Verifies a trade event.
 */
void expect_trade(const Event& event,
                  std::uint64_t resting_order_id,
                  std::uint64_t incoming_order_id,
                  std::int64_t price,
                  std::uint64_t quantity) {
    // Read the trade fields that define matching order, price, and quantity.
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(event));
    const auto& trade = std::get<TradeEvent>(event);
    EXPECT_EQ(trade.resting_order_id, resting_order_id);
    EXPECT_EQ(trade.incoming_order_id, incoming_order_id);
    EXPECT_EQ(trade.price, price);
    EXPECT_EQ(trade.quantity, quantity);
}

/**
 * @brief Verifies a direct book cancellation result.
 */
void expect_canceled(const matching_engine::CancelResult& result, std::uint64_t order_id) {
    // Cancellation returns a single result variant instead of using an event buffer.
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(result));
    EXPECT_EQ(std::get<CanceledEvent>(result).order_id, order_id);
}

/**
 * @brief Submits a resting order and verifies it was accepted.
 */
void submit_accepted(OrderBook& book, Order order) {
    // Clear setup noise by asserting the one-event accepted path at insertion time.
    const auto order_id = order.id;
    std::vector<Event> events;
    book.submit(std::move(order), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front(), order_id);
}

/**
 * @brief Processes an exchange action and replaces the event buffer contents.
 */
void process(Exchange& exchange, Action action, std::vector<Event>& events) {
    // Reuse one buffer so every assertion reads only events from the latest action.
    exchange.process(action, events);
}

/**
 * @brief Finds a printed book snapshot containing a symbol token.
 */
[[nodiscard]] const BookSnapshotEvent* find_snapshot(const std::vector<Event>& events,
                                                     const std::string& symbol) {
    // Scan public print output because symbol books are private exchange state.
    for (const auto& event : events) {
        const auto* snapshot = std::get_if<BookSnapshotEvent>(&event);
        if (snapshot != nullptr &&
            snapshot->message.find("book " + symbol + ":") != std::string::npos) {
            return snapshot;
        }
    }

    return nullptr;
}

} // namespace

TEST(OrderBookRegressionTest, FokRejectDoesNotMutateBook) {
    // Rest one ask, then attempt an all-or-nothing buy that cannot complete.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Sell, 100, 5));
    std::vector<Event> events;
    book.submit(make_order(2, Side::Buy, 100, 8, TimeInForce::FillOrKill), events);

    // The failed FOK leaves the original resting liquidity and index untouched.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InsufficientLiquidity, 2);
    const auto snapshot = book.debug_snapshot();
    ASSERT_EQ(snapshot.asks.size(), 1U);
    EXPECT_EQ(snapshot.asks.front().price, 100);
    EXPECT_EQ(snapshot.asks.front().total_volume, 5U);
    ASSERT_EQ(snapshot.asks.front().orders.size(), 1U);
    EXPECT_EQ(snapshot.asks.front().orders.front().id, 1U);
    EXPECT_EQ(snapshot.asks.front().orders.front().quantity, 5U);
    EXPECT_TRUE(book.contains_order(1));
    EXPECT_FALSE(book.contains_order(2));
}

TEST(OrderBookRegressionTest, IocPartialFillDoesNotRestRemainder) {
    // Rest less liquidity than the incoming IOC order requests.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Sell, 100, 3));
    std::vector<Event> events;
    book.submit(make_order(2, Side::Buy, 100, 8, TimeInForce::ImmediateOrCancel), events);

    // The available shares trade, and the unfilled IOC remainder is expired.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front(), 2);
    expect_trade(events[1], 1, 2, 100, 3);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_TRUE(snapshot.indexed_order_ids.empty());
    EXPECT_FALSE(book.contains_order(2));
}

TEST(OrderBookRegressionTest, MarketOrderDoesNotRest) {
    // Submit a market order with no opposite-side liquidity.
    OrderBook book;
    std::vector<Event> events;
    book.submit_market(make_order(1, Side::Buy, 0, 5), events);

    // The remainder is rejected instead of becoming a synthetic resting limit order.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front(), 1);
    expect_rejected(events[1], RejectReason::InsufficientLiquidity, 1);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_TRUE(snapshot.indexed_order_ids.empty());
    EXPECT_FALSE(book.contains_order(1));
}

TEST(OrderBookRegressionTest, PartialFillThenCancelRemovesOnlyRemainder) {
    // Partially fill a resting ask so only its residual quantity remains cancelable.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Sell, 100, 10));
    std::vector<Event> events;
    book.submit(make_order(2, Side::Buy, 100, 4), events);
    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 1, 2, 100, 4);

    // Cancel removes the residual order and its live index entry.
    expect_canceled(book.cancel(1), 1);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_TRUE(snapshot.indexed_order_ids.empty());
    EXPECT_FALSE(book.contains_order(1));
    EXPECT_FALSE(book.contains_order(2));
}

TEST(OrderBookRegressionTest, LastCancelRemovesEmptyPriceLevel) {
    // Rest exactly one order at one bid price level.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 7));

    // Canceling the final order erases the now-empty level.
    expect_canceled(book.cancel(1), 1);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.indexed_order_ids.empty());
}

TEST(OrderBookRegressionTest, CrossingOrderPreservesPriceTimePriority) {
    // Rest two asks at the same price in FIFO order.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Sell, 100, 2));
    submit_accepted(book, make_order(2, Side::Sell, 100, 2));
    std::vector<Event> events;
    book.submit(make_order(3, Side::Buy, 100, 3), events);

    // The older order fills completely before the newer order is touched.
    ASSERT_EQ(events.size(), 3U);
    expect_trade(events[1], 1, 3, 100, 2);
    expect_trade(events[2], 2, 3, 100, 1);
    const auto snapshot = book.debug_snapshot();
    ASSERT_EQ(snapshot.asks.size(), 1U);
    ASSERT_EQ(snapshot.asks.front().orders.size(), 1U);
    EXPECT_EQ(snapshot.asks.front().orders.front().id, 2U);
    EXPECT_EQ(snapshot.asks.front().orders.front().quantity, 1U);
}

TEST(OrderBookRegressionTest, CrossingOrderConsumesBetterPricesFirst) {
    // Rest asks out of best-price order to catch traversal regressions.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Sell, 101, 1));
    submit_accepted(book, make_order(2, Side::Sell, 99, 1));
    std::vector<Event> events;
    book.submit(make_order(3, Side::Buy, 101, 2), events);

    // The lower ask trades before the higher ask regardless of insertion order.
    ASSERT_EQ(events.size(), 3U);
    expect_trade(events[1], 2, 3, 99, 1);
    expect_trade(events[2], 1, 3, 101, 1);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_TRUE(snapshot.indexed_order_ids.empty());
}

TEST(ExchangeRegressionTest, MultiSymbolBooksStayIndependent) {
    // Seed two symbols, then mutate only AAPL through a trade and cancel.
    Exchange exchange;
    std::vector<Event> events;
    process(exchange, make_submit(1, "AAPL", Side::Sell, 100, 5), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front(), 1);
    process(exchange, make_submit(2, "MSFT", Side::Sell, 200, 6), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front(), 2);
    process(exchange, make_submit(3, "AAPL", Side::Buy, 100, 2), events);
    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 1, 3, 100, 2);
    process(exchange, CancelOrderAction{1}, events);
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(events.front()));

    // Public snapshots prove MSFT retained its original order and quantity.
    process(exchange, PrintBookAction{}, events);
    const auto* msft = find_snapshot(events, "MSFT");
    ASSERT_NE(msft, nullptr);
    EXPECT_NE(msft->message.find("[2 SELL 200x6]"), std::string::npos);
    process(exchange, make_market(4, "MSFT", Side::Buy, 6), events);
    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 2, 4, 200, 6);
}
