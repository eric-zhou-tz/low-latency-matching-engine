#include "exchange.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::Action;
using matching_engine::CancelOrderAction;
using matching_engine::CanceledEvent;
using matching_engine::Event;
using matching_engine::Exchange;
using matching_engine::RejectedEvent;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;
using matching_engine::TimeInForce;
using matching_engine::TradeEvent;

/**
 * @brief Builds a test submit action with explicit symbol and side.
 */
[[nodiscard]] SubmitOrderAction make_submit(std::uint64_t id,
                                            std::string symbol,
                                            Side side,
                                            std::int64_t price,
                                            std::uint64_t quantity,
                                            TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Keep submit construction compact so each test highlights exchange behavior.
    return {.id = id,
            .symbol = std::move(symbol),
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

/**
 * @brief Submits an order through the public exchange API.
 */
void submit(Exchange& exchange, SubmitOrderAction action, std::vector<Event>& out) {
    // Wrap the submit action in the same variant path used by production callers.
    exchange.process(Action{std::move(action)}, out);
}

/**
 * @brief Cancels an order through the public exchange API.
 */
void cancel(Exchange& exchange, std::uint64_t order_id, std::vector<Event>& out) {
    // Route cancellation through Exchange so the exchange-level index is exercised.
    exchange.process(Action{CancelOrderAction{order_id}}, out);
}

/**
 * @brief Verifies the first event is an acceptance.
 */
void expect_accepted(const std::vector<Event>& events) {
    // Successful submissions should report acceptance before any trades.
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(events.front()));
}

/**
 * @brief Verifies a one-event cancel response.
 */
void expect_canceled(const std::vector<Event>& events, std::uint64_t order_id) {
    // Exchange cancel should preserve the book's public cancel event.
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(events.front()));
    EXPECT_EQ(std::get<CanceledEvent>(events.front()).order_id, order_id);
}

/**
 * @brief Verifies a one-event rejection response.
 */
void expect_rejected(const std::vector<Event>& events, const std::string& reason) {
    // Rejection text is formatted at the presentation boundary now.
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(events.front()));
    EXPECT_EQ(matching_engine::format_event(events.front()), "REJECTED " + reason);
}

} // namespace

TEST(ExchangeTest, CancelRoutesDirectlyToOwningSymbolBook) {
    // Create two symbol books so the cancel must use the owning order id route.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);
    submit(exchange, make_submit(2, "MSFT", Side::Buy, 200, 20), events);
    expect_accepted(events);

    cancel(exchange, 2, events);

    // The MSFT order should cancel without disturbing the AAPL order.
    expect_canceled(events, 2);
    cancel(exchange, 1, events);
    expect_canceled(events, 1);
}

TEST(ExchangeTest, UnknownCancelStillReturnsRejectedEvent) {
    // Leave the exchange empty so the cancel id is unknown at the exchange layer.
    Exchange exchange;
    std::vector<Event> events;

    cancel(exchange, 404, events);

    // Preserve the existing unknown-order rejection message format.
    expect_rejected(events, "unknown order id 404");
}

TEST(ExchangeTest, CancelRemovesOrderFromExchangeIndex) {
    // Submit a resting order so the exchange-level live index has an entry.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(10, "AAPL", Side::Sell, 105, 7), events);
    expect_accepted(events);

    cancel(exchange, 10, events);
    expect_canceled(events, 10);
    submit(exchange, make_submit(10, "MSFT", Side::Buy, 205, 3), events);

    // Reusing the id after a successful cancel proves the live index was erased.
    expect_accepted(events);
}

TEST(ExchangeTest, SameOrderIdCannotBeCanceledTwiceSuccessfully) {
    // Submit one live order and remove it once.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(11, "AAPL", Side::Buy, 101, 4), events);
    expect_accepted(events);
    cancel(exchange, 11, events);
    expect_canceled(events, 11);

    cancel(exchange, 11, events);

    // The second cancel should miss the exchange-level index and reject.
    expect_rejected(events, "unknown order id 11");
}

TEST(ExchangeTest, MultiSymbolCancelDoesNotInvalidateUnrelatedBook) {
    // Seed separate books and then cancel only one symbol's order.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(20, "AAPL", Side::Sell, 110, 5), events);
    expect_accepted(events);
    submit(exchange, make_submit(21, "MSFT", Side::Sell, 210, 6), events);
    expect_accepted(events);

    cancel(exchange, 20, events);
    expect_canceled(events, 20);

    // The unrelated symbol's order should still be live and cancelable afterward.
    cancel(exchange, 21, events);
    expect_canceled(events, 21);
}

TEST(ExchangeTest, FilledRestingOrdersLeaveExchangeIndex) {
    // Rest a sell, then fully consume it with an aggressive buy.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(30, "AAPL", Side::Sell, 100, 5), events);
    expect_accepted(events);
    submit(exchange, make_submit(31, "AAPL", Side::Buy, 101, 5), events);

    // Confirm the setup actually filled the resting order.
    ASSERT_EQ(events.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(events[1]));
    EXPECT_EQ(std::get<TradeEvent>(events[1]).resting_order_id, 30);

    submit(exchange, make_submit(30, "MSFT", Side::Buy, 200, 1), events);

    // A new order can reuse the fully filled id because it is no longer live.
    expect_accepted(events);
    cancel(exchange, 30, events);
    expect_canceled(events, 30);
}

TEST(ExchangeTest, IocRemainderDoesNotEnterCancelIndex) {
    // Submit an IOC order that cannot rest because the opposite side is empty.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange,
           make_submit(40, "AAPL", Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel),
           events);

    // The id should be reusable immediately because IOC leftovers expire.
    expect_accepted(events);
    cancel(exchange, 40, events);
    expect_rejected(events, "unknown order id 40");

    submit(exchange, make_submit(40, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}
