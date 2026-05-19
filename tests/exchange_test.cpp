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
using matching_engine::ModifyOrderAction;
using matching_engine::ModifiedEvent;
using matching_engine::PrintBookAction;
using matching_engine::RejectedEvent;
using matching_engine::ReplacedEvent;
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
 * @brief Builds a test market action with explicit symbol and side.
 */
[[nodiscard]] MarketOrderAction make_market(std::uint64_t id,
                                            std::string symbol,
                                            Side side,
                                            std::uint64_t quantity) {
    // Keep market construction compact so tests focus on immediate execution behavior.
    return {.id = id, .symbol = std::move(symbol), .side = side, .quantity = quantity};
}

/**
 * @brief Submits an order through the public exchange API.
 */
void submit(Exchange& exchange, SubmitOrderAction action, std::vector<Event>& out) {
    // Wrap the submit action in the same variant path used by production callers.
    exchange.process(Action{std::move(action)}, out);
}

/**
 * @brief Submits a market order through the public exchange API.
 */
void submit_market(Exchange& exchange, MarketOrderAction action, std::vector<Event>& out) {
    // Wrap the market action in the same variant path used by production callers.
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
 * @brief Modifies an order through the public exchange API.
 */
void modify(Exchange& exchange,
            std::uint64_t order_id,
            std::int64_t new_price,
            std::uint64_t new_quantity,
            std::vector<Event>& out) {
    // Route modification through Exchange so the exchange-level index is exercised.
    exchange.process(Action{ModifyOrderAction{order_id, new_price, new_quantity}}, out);
}

/**
 * @brief Requests snapshots for all known symbol books.
 */
void print_book(Exchange& exchange, std::vector<Event>& out) {
    // Route through the public variant API so snapshot event emission is covered.
    exchange.process(Action{PrintBookAction{}}, out);
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

/**
 * @brief Verifies an exchange trade event.
 */
void expect_trade(const Event& event,
                  std::uint64_t resting_order_id,
                  std::uint64_t incoming_order_id,
                  std::int64_t price,
                  std::uint64_t quantity) {
    // Pull out the trade payload before comparing execution details.
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(event));
    const auto& trade = std::get<TradeEvent>(event);
    EXPECT_EQ(trade.resting_order_id, resting_order_id);
    EXPECT_EQ(trade.incoming_order_id, incoming_order_id);
    EXPECT_EQ(trade.price, price);
    EXPECT_EQ(trade.quantity, quantity);
}

/**
 * @brief Verifies a one-event in-place modify response.
 */
void expect_modified(const std::vector<Event>& events, std::uint64_t order_id) {
    // Safe quantity reductions should surface as ModifiedEvent values.
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<ModifiedEvent>(events.front()));
    EXPECT_EQ(std::get<ModifiedEvent>(events.front()).order_id, order_id);
}

/**
 * @brief Verifies a replace response starts with ReplacedEvent.
 */
void expect_replaced(const std::vector<Event>& events, std::uint64_t order_id) {
    // Replacement may be followed by trades, so only the first event is fixed.
    ASSERT_FALSE(events.empty());
    ASSERT_TRUE(std::holds_alternative<ReplacedEvent>(events.front()));
    EXPECT_EQ(std::get<ReplacedEvent>(events.front()).old_order_id, order_id);
    EXPECT_EQ(std::get<ReplacedEvent>(events.front()).new_order_id, order_id);
}

/**
 * @brief Finds a snapshot event containing a token.
 */
[[nodiscard]] const BookSnapshotEvent* find_snapshot_containing(
    const std::vector<Event>& events,
    const std::string& token) {
    // Scan all emitted book snapshots because symbol map iteration order is not contractual.
    for (const auto& event : events) {
        const auto* snapshot = std::get_if<BookSnapshotEvent>(&event);
        if (snapshot != nullptr && snapshot->message.find(token) != std::string::npos) {
            return snapshot;
        }
    }

    // Return null so callers can make a clear assertion.
    return nullptr;
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

TEST(ExchangeTest, DuplicateOrderIdAcrossSymbolsIsRejectedUntilOriginalLeavesIndex) {
    // Rest an AAPL order so the exchange-level id index owns id 1.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    submit(exchange, make_submit(1, "MSFT", Side::Sell, 200, 5), events);

    // Duplicate detection should happen across symbols, not just within one book.
    expect_rejected(events, "duplicate order id 1");

    cancel(exchange, 1, events);
    expect_canceled(events, 1);
    submit(exchange, make_submit(1, "MSFT", Side::Sell, 200, 5), events);

    // Once the original id leaves the live index, the id can be reused.
    expect_accepted(events);
}

TEST(ExchangeTest, MarketOrderDuplicateIdIsRejectedAgainstRestingOrder) {
    // Rest a limit order and then attempt to reuse its id as a market order.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(2, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    submit_market(exchange, make_market(2, "MSFT", Side::Sell, 5), events);

    // Market orders share the same live-id namespace as resting limit orders.
    expect_rejected(events, "duplicate order id 2");
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

TEST(ExchangeTest, CrossSymbolOrdersDoNotMatchOrAffectEachOther) {
    // Create a sell on AAPL and a crossing buy on MSFT with overlapping prices.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(22, "AAPL", Side::Sell, 100, 5), events);
    expect_accepted(events);

    submit(exchange, make_submit(23, "MSFT", Side::Buy, 101, 5), events);

    // Matching must be isolated per symbol, so the MSFT order rests instead of trading AAPL.
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events);

    cancel(exchange, 22, events);
    expect_canceled(events, 22);
    cancel(exchange, 23, events);
    expect_canceled(events, 23);
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

TEST(ExchangeTest, PartialRestingFillKeepsRemainingOrderIndexed) {
    // Partially fill a resting sell so it remains visible and cancelable.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(32, "AAPL", Side::Sell, 100, 7), events);
    expect_accepted(events);
    submit(exchange, make_submit(33, "AAPL", Side::Buy, 101, 3), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 32, 33, 100, 3);

    // The resting order has quantity left, so the exchange route must remain live.
    cancel(exchange, 32, events);
    expect_canceled(events, 32);
    cancel(exchange, 33, events);
    expect_rejected(events, "unknown order id 33");
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

TEST(ExchangeTest, FokRejectDoesNotEnterCancelIndex) {
    // Seed insufficient crossing liquidity for a FOK buy.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(41, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange, make_submit(42, "AAPL", Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    // The FOK order rejects before acceptance and should not become cancelable.
    expect_rejected(events, "insufficient liquidity 42");
    cancel(exchange, 42, events);
    expect_rejected(events, "unknown order id 42");
}

TEST(ExchangeTest, MarketOrderTradesAndDoesNotEnterCancelIndex) {
    // Seed less ask liquidity than the market buy wants.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(50, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit_market(exchange, make_market(51, "AAPL", Side::Buy, 8), events);

    // The market order executes available liquidity and rejects the unfilled remainder.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events);
    expect_trade(events[1], 50, 51, 100, 3);
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(events[2]));
    EXPECT_EQ(matching_engine::format_event(events[2]), "REJECTED insufficient liquidity 51");

    // Neither the filled resting order nor the market order should remain cancelable.
    cancel(exchange, 50, events);
    expect_rejected(events, "unknown order id 50");
    cancel(exchange, 51, events);
    expect_rejected(events, "unknown order id 51");
}

TEST(ExchangeTest, IocOrderTradesAndDoesNotRestRemainder) {
    // Seed less ask liquidity than the IOC buy wants.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(60, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange,
           make_submit(61, "AAPL", Side::Buy, 101, 8, TimeInForce::ImmediateOrCancel),
           events);

    // The IOC order trades what is available and expires the leftover five.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events);
    expect_trade(events[1], 60, 61, 100, 3);

    // The incoming IOC id should be reusable because no remainder rested.
    cancel(exchange, 61, events);
    expect_rejected(events, "unknown order id 61");
    submit(exchange, make_submit(61, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}

TEST(ExchangeTest, FullyFilledIocOrderRemovesRestingIndexAndDoesNotIndexIncoming) {
    // Give the IOC exactly enough opposite-side liquidity to fill.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(62, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange,
           make_submit(63, "AAPL", Side::Buy, 100, 3, TimeInForce::ImmediateOrCancel),
           events);

    // Exact IOC fills should emit accept then trade, with no leftover rejection.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events);
    expect_trade(events[1], 62, 63, 100, 3);

    cancel(exchange, 62, events);
    expect_rejected(events, "unknown order id 62");
    cancel(exchange, 63, events);
    expect_rejected(events, "unknown order id 63");
}

TEST(ExchangeTest, ModifyReductionKeepsExchangeIndexCancelable) {
    // Rest a GTC order, reduce it in place, then cancel it through the exchange index.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(70, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    modify(exchange, 70, 100, 4, events);
    expect_modified(events, 70);

    cancel(exchange, 70, events);
    expect_canceled(events, 70);
}

TEST(ExchangeTest, ReplacementModifyKeepsIndexWhenRemainderRests) {
    // Increase quantity so the order is cancel-replaced but remains resting.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(80, "AAPL", Side::Buy, 100, 5), events);
    expect_accepted(events);

    modify(exchange, 80, 100, 7, events);
    expect_replaced(events, 80);

    cancel(exchange, 80, events);
    expect_canceled(events, 80);
}

TEST(ExchangeTest, ReplacementModifyFullyExecutedLeavesNoIndex) {
    // Modify a passive bid to cross exactly all available ask liquidity.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(90, "AAPL", Side::Sell, 99, 4), events);
    expect_accepted(events);
    submit(exchange, make_submit(91, "AAPL", Side::Buy, 98, 4), events);
    expect_accepted(events);

    modify(exchange, 91, 100, 4, events);

    // A fully executed replacement should remove both the modified and resting ids.
    ASSERT_EQ(events.size(), 2U);
    expect_replaced(events, 91);
    expect_trade(events[1], 90, 91, 99, 4);
    cancel(exchange, 91, events);
    expect_rejected(events, "unknown order id 91");
    submit(exchange, make_submit(91, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}

TEST(ExchangeTest, ReplacementModifyEmitsReplaceBeforeTradesInPriceTimeOrder) {
    // Modify a passive buy into multiple ask levels so event ordering is observable.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(92, "AAPL", Side::Sell, 99, 2), events);
    expect_accepted(events);
    submit(exchange, make_submit(93, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);
    submit(exchange, make_submit(94, "AAPL", Side::Buy, 98, 6), events);
    expect_accepted(events);

    modify(exchange, 94, 100, 6, events);

    // Replace is the first public event, followed by trades in book priority order.
    ASSERT_EQ(events.size(), 3U);
    expect_replaced(events, 94);
    expect_trade(events[1], 92, 94, 99, 2);
    expect_trade(events[2], 93, 94, 100, 3);

    // The modified order only partially filled, so its remaining share is still cancelable.
    cancel(exchange, 94, events);
    expect_canceled(events, 94);
}

TEST(ExchangeTest, ModifyUnknownAndNonRestingOrdersReject) {
    // Unknown ids should be rejected before touching any symbol book.
    Exchange exchange;
    std::vector<Event> events;

    modify(exchange, 404, 100, 5, events);
    expect_rejected(events, "unknown order id 404");

    submit(exchange,
           make_submit(100, "AAPL", Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel),
           events);
    expect_accepted(events);
    modify(exchange, 100, 100, 4, events);
    expect_rejected(events, "unknown order id 100");
}

TEST(ExchangeTest, ReplacementModifyDoesNotEmitCancelAcceptPair) {
    // Replacement should be atomic from the event stream perspective.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(110, "AAPL", Side::Buy, 100, 5), events);
    expect_accepted(events);

    modify(exchange, 110, 101, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events, 110);
    EXPECT_FALSE(std::holds_alternative<CanceledEvent>(events.front()));
    EXPECT_FALSE(std::holds_alternative<AcceptedEvent>(events.front()));
}

TEST(ExchangeTest, PrintEmptyExchangeEmitsSingleEmptySnapshot) {
    // Snapshot an exchange before any symbol books have been created.
    Exchange exchange;
    std::vector<Event> events;

    print_book(exchange, events);

    // Empty state still returns one printable event for command-line callers.
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<BookSnapshotEvent>(events.front()));
    EXPECT_EQ(std::get<BookSnapshotEvent>(events.front()).message, "book: empty");
}

TEST(ExchangeTest, PrintBookReportsEachSymbolWithoutCrossContamination) {
    // Seed two symbols with different sides and prices.
    Exchange exchange;
    std::vector<Event> events;
    submit(exchange, make_submit(120, "AAPL", Side::Buy, 100, 4), events);
    expect_accepted(events);
    submit(exchange, make_submit(121, "MSFT", Side::Sell, 200, 6), events);
    expect_accepted(events);

    print_book(exchange, events);

    // Each symbol should get its own snapshot and contain only its own orders.
    ASSERT_EQ(events.size(), 2U);
    const auto* aapl = find_snapshot_containing(events, "book AAPL:");
    const auto* msft = find_snapshot_containing(events, "book MSFT:");
    ASSERT_NE(aapl, nullptr);
    ASSERT_NE(msft, nullptr);
    EXPECT_NE(aapl->message.find("[120 BUY 100x4]"), std::string::npos);
    EXPECT_EQ(aapl->message.find("[121 SELL 200x6]"), std::string::npos);
    EXPECT_NE(msft->message.find("[121 SELL 200x6]"), std::string::npos);
    EXPECT_EQ(msft->message.find("[120 BUY 100x4]"), std::string::npos);
}
