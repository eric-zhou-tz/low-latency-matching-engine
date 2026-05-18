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
using matching_engine::TradeEvent;

/**
 * @brief Builds a test submit action with explicit symbol and side.
 */
[[nodiscard]] SubmitOrderAction make_submit(std::uint64_t id,
                                            std::string symbol,
                                            Side side,
                                            std::int64_t price,
                                            std::uint64_t quantity) {
    // Keep submit construction compact so each test highlights exchange behavior.
    return {.id = id,
            .symbol = std::move(symbol),
            .side = side,
            .price = price,
            .quantity = quantity};
}

/**
 * @brief Submits an order through the public exchange API.
 */
[[nodiscard]] std::vector<Event> submit(Exchange& exchange, SubmitOrderAction action) {
    // Wrap the submit action in the same variant path used by production callers.
    return exchange.process(Action{std::move(action)});
}

/**
 * @brief Cancels an order through the public exchange API.
 */
[[nodiscard]] std::vector<Event> cancel(Exchange& exchange, std::uint64_t order_id) {
    // Route cancellation through Exchange so the exchange-level index is exercised.
    return exchange.process(Action{CancelOrderAction{order_id}});
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
    expect_accepted(submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10)));
    expect_accepted(submit(exchange, make_submit(2, "MSFT", Side::Buy, 200, 20)));

    const auto canceled = cancel(exchange, 2);

    // The MSFT order should cancel without disturbing the AAPL order.
    expect_canceled(canceled, 2);
    expect_canceled(cancel(exchange, 1), 1);
}

TEST(ExchangeTest, UnknownCancelStillReturnsRejectedEvent) {
    // Leave the exchange empty so the cancel id is unknown at the exchange layer.
    Exchange exchange;

    const auto events = cancel(exchange, 404);

    // Preserve the existing unknown-order rejection message format.
    expect_rejected(events, "unknown order id 404");
}

TEST(ExchangeTest, CancelRemovesOrderFromExchangeIndex) {
    // Submit a resting order so the exchange-level live index has an entry.
    Exchange exchange;
    expect_accepted(submit(exchange, make_submit(10, "AAPL", Side::Sell, 105, 7)));

    expect_canceled(cancel(exchange, 10), 10);
    const auto reused_id = submit(exchange, make_submit(10, "MSFT", Side::Buy, 205, 3));

    // Reusing the id after a successful cancel proves the live index was erased.
    expect_accepted(reused_id);
}

TEST(ExchangeTest, SameOrderIdCannotBeCanceledTwiceSuccessfully) {
    // Submit one live order and remove it once.
    Exchange exchange;
    expect_accepted(submit(exchange, make_submit(11, "AAPL", Side::Buy, 101, 4)));
    expect_canceled(cancel(exchange, 11), 11);

    const auto second_cancel = cancel(exchange, 11);

    // The second cancel should miss the exchange-level index and reject.
    expect_rejected(second_cancel, "unknown order id 11");
}

TEST(ExchangeTest, MultiSymbolCancelDoesNotInvalidateUnrelatedBook) {
    // Seed separate books and then cancel only one symbol's order.
    Exchange exchange;
    expect_accepted(submit(exchange, make_submit(20, "AAPL", Side::Sell, 110, 5)));
    expect_accepted(submit(exchange, make_submit(21, "MSFT", Side::Sell, 210, 6)));

    expect_canceled(cancel(exchange, 20), 20);

    // The unrelated symbol's order should still be live and cancelable afterward.
    expect_canceled(cancel(exchange, 21), 21);
}

TEST(ExchangeTest, FilledRestingOrdersLeaveExchangeIndex) {
    // Rest a sell, then fully consume it with an aggressive buy.
    Exchange exchange;
    expect_accepted(submit(exchange, make_submit(30, "AAPL", Side::Sell, 100, 5)));
    const auto match = submit(exchange, make_submit(31, "AAPL", Side::Buy, 101, 5));

    // Confirm the setup actually filled the resting order.
    ASSERT_EQ(match.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(match[1]));
    EXPECT_EQ(std::get<TradeEvent>(match[1]).resting_order_id, 30);

    const auto reused_id = submit(exchange, make_submit(30, "MSFT", Side::Buy, 200, 1));

    // A new order can reuse the fully filled id because it is no longer live.
    expect_accepted(reused_id);
    expect_canceled(cancel(exchange, 30), 30);
}
