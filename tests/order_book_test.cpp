#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::CanceledEvent;
using matching_engine::Event;
using matching_engine::ModifiedEvent;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::RejectedEvent;
using matching_engine::RejectReason;
using matching_engine::ReplacedEvent;
using matching_engine::Side;
using matching_engine::TimeInForce;
using matching_engine::TradeEvent;

[[nodiscard]] Order make_order(std::uint64_t id,
                               Side side,
                               std::int64_t price,
                               std::uint64_t quantity,
                               TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Keep order construction compact so each test focuses on book behavior.
    return {.id = id,
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

void expect_accepted(const Event& event) {
    // Verify the event variant represents a successful action.
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(event));
}

void expect_rejected(const Event& event) {
    // Verify the event variant represents a rejected action.
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(event));
}

void expect_rejected(const Event& event, RejectReason reason, std::uint64_t order_id) {
    // Check both the rejection type and the machine-readable reason.
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(event));
    const auto& rejected = std::get<RejectedEvent>(event);
    EXPECT_EQ(rejected.reason, reason);
    EXPECT_EQ(rejected.order_id, order_id);
}

void expect_rejected(const matching_engine::CancelResult& result) {
    // Cancel returns one direct result instead of using an event buffer.
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(result));
}

void expect_canceled(const Event& event, std::uint64_t order_id) {
    // Check both the event type and the id that was canceled.
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(event));
    EXPECT_EQ(std::get<CanceledEvent>(event).order_id, order_id);
}

void expect_canceled(const matching_engine::CancelResult& result, std::uint64_t order_id) {
    // Cancel success materializes one direct result.
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(result));
    EXPECT_EQ(std::get<CanceledEvent>(result).order_id, order_id);
}

void expect_modified(const Event& event,
                     std::uint64_t order_id,
                     std::int64_t old_price,
                     std::int64_t new_price,
                     std::uint64_t old_quantity,
                     std::uint64_t new_quantity) {
    // A safe quantity reduction should be reported as an in-place modification.
    ASSERT_TRUE(std::holds_alternative<ModifiedEvent>(event));
    const auto& modified = std::get<ModifiedEvent>(event);
    EXPECT_EQ(modified.order_id, order_id);
    EXPECT_EQ(modified.old_price, old_price);
    EXPECT_EQ(modified.new_price, new_price);
    EXPECT_EQ(modified.old_quantity, old_quantity);
    EXPECT_EQ(modified.new_quantity, new_quantity);
}

void expect_replaced(const Event& event,
                     std::uint64_t order_id,
                     std::int64_t old_price,
                     std::int64_t new_price,
                     std::uint64_t old_quantity,
                     std::uint64_t new_quantity) {
    // Cancel-replace modifies should be reported without public cancel/accept events.
    ASSERT_TRUE(std::holds_alternative<ReplacedEvent>(event));
    const auto& replaced = std::get<ReplacedEvent>(event);
    EXPECT_EQ(replaced.old_order_id, order_id);
    EXPECT_EQ(replaced.new_order_id, order_id);
    EXPECT_EQ(replaced.old_price, old_price);
    EXPECT_EQ(replaced.new_price, new_price);
    EXPECT_EQ(replaced.old_quantity, old_quantity);
    EXPECT_EQ(replaced.new_quantity, new_quantity);
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
    std::vector<Event> events;
    book.submit(std::move(order), events);
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
    std::vector<Event> events;

    book.submit(make_order(1, Side::Buy, 100, 10), events);

    // The order should be accepted and visible in the snapshot.
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("[1 BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, DuplicateOrderIdIsRejected) {
    // Seed the book with an order id.
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));
    std::vector<Event> events;

    // Reusing the same id must fail even with different order details.
    book.submit(make_order(1, Side::Sell, 101, 5), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, AggressiveBuyMatchesRestingSell) {
    // Put sell liquidity on the book, then submit a crossing buy.
    OrderBook book;
    submit_accepted(book, make_order(10, Side::Sell, 100, 10));
    std::vector<Event> events;

    book.submit(make_order(11, Side::Buy, 105, 4), events);

    // The buy should trade at the resting sell price and leave sell remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 10, 11, 100, 4);
    EXPECT_NE(book.snapshot().find("[10 SELL 100x6]"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[11 BUY 105x4]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellMatchesRestingBuy) {
    // Put buy liquidity on the book, then submit a crossing sell.
    OrderBook book;
    submit_accepted(book, make_order(30, Side::Buy, 101, 5));
    std::vector<Event> events;

    book.submit(make_order(31, Side::Sell, 100, 8), events);

    // The sell should trade at the resting bid price and rest the remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 30, 31, 101, 5);
    EXPECT_EQ(book.snapshot().find("[30 BUY 101x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[31 SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PriceTimePriorityOlderRestingOrderAtSamePriceFillsFirst) {
    // Two sells at the same price should be matched in insertion order.
    OrderBook book;
    submit_accepted(book, make_order(20, Side::Sell, 100, 5));
    submit_accepted(book, make_order(21, Side::Sell, 100, 5));
    std::vector<Event> events;

    book.submit(make_order(22, Side::Buy, 100, 7), events);

    // The older order fills first, then the newer order receives the leftover fill.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 20, 22, 100, 5);
    expect_trade(events[2], 21, 22, 100, 2);
    EXPECT_EQ(book.snapshot().find("[20 SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[21 SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PartialFillLeavesRemainingQuantityCorrectly) {
    // A larger crossing buy should consume the ask and rest the unfilled quantity.
    OrderBook book;
    submit_accepted(book, make_order(40, Side::Sell, 100, 5));
    std::vector<Event> events;

    book.submit(make_order(41, Side::Buy, 101, 8), events);

    // The resting ask disappears and the remaining buy quantity becomes visible.
    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 40, 41, 100, 5);
    EXPECT_EQ(book.snapshot().find("[40 SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[41 BUY 101x3]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveBuyWalksAskLevelsAndRestsRemainder) {
    // Seed two ask levels so the incoming order must preserve price priority.
    OrderBook book;
    submit_accepted(book, make_order(42, Side::Sell, 100, 4));
    submit_accepted(book, make_order(43, Side::Sell, 101, 3));
    std::vector<Event> events;

    book.submit(make_order(44, Side::Buy, 102, 10), events);

    // The buy consumes cheaper asks first and only rests its unfilled quantity.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 42, 44, 100, 4);
    expect_trade(events[2], 43, 44, 101, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[42 SELL 100x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[43 SELL 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[44 BUY 102x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellWalksBidLevelsAndRestsRemainder) {
    // Seed two bid levels so the incoming order must preserve price priority.
    OrderBook book;
    submit_accepted(book, make_order(45, Side::Buy, 102, 4));
    submit_accepted(book, make_order(46, Side::Buy, 101, 3));
    std::vector<Event> events;

    book.submit(make_order(47, Side::Sell, 100, 10), events);

    // The sell consumes higher bids first and only rests its unfilled quantity.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 45, 47, 102, 4);
    expect_trade(events[2], 46, 47, 101, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[45 BUY 102x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[46 BUY 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[47 SELL 100x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, IocLimitOrderDoesNotRestWhenUnfilled) {
    // Submit an IOC buy that cannot cross the empty ask side.
    OrderBook book;
    std::vector<Event> events;

    book.submit(
        make_order(480, Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel), events);

    // The order is accepted but its unfilled quantity expires instead of resting.
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[480 BUY"), std::string::npos);
    expect_rejected(book.cancel(480));
}

TEST(OrderBookTest, IocLimitOrderTradesAndCancelsRemainder) {
    // Seed less ask liquidity than the IOC buy wants.
    OrderBook book;
    submit_accepted(book, make_order(481, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit(
        make_order(482, Side::Buy, 101, 8, TimeInForce::ImmediateOrCancel), events);

    // The IOC buy executes available liquidity and does not rest its leftover five.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 481, 482, 100, 3);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("orders=0"), std::string::npos);
    EXPECT_EQ(snapshot.find("[481 SELL"), std::string::npos);
    EXPECT_EQ(snapshot.find("[482 BUY"), std::string::npos);
    expect_rejected(book.cancel(482));
}

TEST(OrderBookTest, FokLimitOrderRejectsWithoutPartialFillWhenInsufficientLiquidity) {
    // Seed less crossing ask liquidity than the FOK buy requires.
    OrderBook book;
    submit_accepted(book, make_order(483, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit(make_order(484, Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    // The FOK order should reject before acceptance and leave the book untouched.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InsufficientLiquidity, 484);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[483 SELL 100x3]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[484 BUY"), std::string::npos);
}

TEST(OrderBookTest, FokLimitOrderFullyFillsAcrossPriceLevels) {
    // Seed enough crossing ask liquidity across multiple prices for a FOK buy.
    OrderBook book;
    submit_accepted(book, make_order(485, Side::Sell, 100, 3));
    submit_accepted(book, make_order(486, Side::Sell, 101, 5));
    std::vector<Event> events;

    book.submit(make_order(487, Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    // The FOK order passes the preflight, accepts, and consumes all required liquidity.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 485, 487, 100, 3);
    expect_trade(events[2], 486, 487, 101, 5);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("orders=0"), std::string::npos);
    EXPECT_EQ(snapshot.find("[487 BUY"), std::string::npos);
}

TEST(OrderBookTest, FokSellRejectsWhenBestBidVolumeBelowLimitIsNotCrossing) {
    // Only the higher bid crosses the sell limit; lower-priced volume should not count.
    OrderBook book;
    submit_accepted(book, make_order(488, Side::Buy, 101, 3));
    submit_accepted(book, make_order(489, Side::Buy, 99, 10));
    std::vector<Event> events;

    book.submit(make_order(490, Side::Sell, 100, 8, TimeInForce::FillOrKill), events);

    // The FOK sell must reject without consuming the partial crossing bid.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InsufficientLiquidity, 490);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[488 BUY 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[489 BUY 99x10]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[490 SELL"), std::string::npos);
}

TEST(OrderBookTest, MarketBuyFullyFillsAgainstBestAsk) {
    // Seed one ask so the market buy can fill entirely at the best ask.
    OrderBook book;
    submit_accepted(book, make_order(500, Side::Sell, 100, 6));
    std::vector<Event> events;

    book.submit_market(make_order(501, Side::Buy, 0, 4), events);

    // The market buy trades immediately and leaves only the ask remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 500, 501, 100, 4);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[500 SELL 100x2]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[501 BUY"), std::string::npos);
}

TEST(OrderBookTest, MarketBuySweepsMultipleAskPriceLevels) {
    // Seed ascending ask levels so the market buy must consume best prices first.
    OrderBook book;
    submit_accepted(book, make_order(510, Side::Sell, 100, 4));
    submit_accepted(book, make_order(511, Side::Sell, 101, 3));
    submit_accepted(book, make_order(512, Side::Sell, 102, 5));
    std::vector<Event> events;

    book.submit_market(make_order(513, Side::Buy, 0, 7), events);

    // The market buy fills the lower ask before moving to the next price level.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 510, 513, 100, 4);
    expect_trade(events[2], 511, 513, 101, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[510 SELL 100x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[511 SELL 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[512 SELL 102x5]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[513 BUY"), std::string::npos);
}

TEST(OrderBookTest, MarketSellFullyFillsAgainstBestBid) {
    // Seed one bid so the market sell can fill entirely at the best bid.
    OrderBook book;
    submit_accepted(book, make_order(520, Side::Buy, 105, 6));
    std::vector<Event> events;

    book.submit_market(make_order(521, Side::Sell, 0, 4), events);

    // The market sell trades immediately and leaves only the bid remainder.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 520, 521, 105, 4);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[520 BUY 105x2]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[521 SELL"), std::string::npos);
}

TEST(OrderBookTest, MarketSellSweepsMultipleBidPriceLevels) {
    // Seed descending bid levels so the market sell must consume best prices first.
    OrderBook book;
    submit_accepted(book, make_order(530, Side::Buy, 105, 4));
    submit_accepted(book, make_order(531, Side::Buy, 104, 3));
    submit_accepted(book, make_order(532, Side::Buy, 103, 5));
    std::vector<Event> events;

    book.submit_market(make_order(533, Side::Sell, 0, 7), events);

    // The market sell fills the higher bid before moving down to the next price.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 530, 533, 105, 4);
    expect_trade(events[2], 531, 533, 104, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[530 BUY 105x4]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[531 BUY 104x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[532 BUY 103x5]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[533 SELL"), std::string::npos);
}

TEST(OrderBookTest, PartiallyFilledMarketOrderDoesNotRestOnBook) {
    // Seed less ask liquidity than the market buy wants.
    OrderBook book;
    submit_accepted(book, make_order(540, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit_market(make_order(541, Side::Buy, 0, 8), events);

    // The filled quantity trades and the unfilled remainder expires instead of resting.
    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 540, 541, 100, 3);
    expect_rejected(events[2], RejectReason::InsufficientLiquidity, 541);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("orders=0"), std::string::npos);
    EXPECT_EQ(snapshot.find("[541 BUY"), std::string::npos);
    expect_rejected(book.cancel(541));
}

TEST(OrderBookTest, MarketOrderOnEmptyOppositeBookExpiresRemainder) {
    // Submit a market sell when there are no bids to consume.
    OrderBook book;
    std::vector<Event> events;

    book.submit_market(make_order(550, Side::Sell, 0, 5), events);

    // The accepted market order cannot trade and emits an insufficient-liquidity rejection.
    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_rejected(events[1], RejectReason::InsufficientLiquidity, 550);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingBuyOrderSucceeds) {
    // Submit a buy that can be canceled.
    OrderBook book;
    submit_accepted(book, make_order(50, Side::Buy, 100, 10));

    const auto result = book.cancel(50);

    // The cancel event should reference the id and remove it from the snapshot.
    expect_canceled(result, 50);
    EXPECT_EQ(book.snapshot().find("[50 BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingSellOrderSucceeds) {
    // Submit a sell that can be canceled.
    OrderBook book;
    submit_accepted(book, make_order(51, Side::Sell, 102, 7));

    const auto result = book.cancel(51);

    // The cancel event should reference the id and remove it from the snapshot.
    expect_canceled(result, 51);
    EXPECT_EQ(book.snapshot().find("[51 SELL 102x7]"), std::string::npos);
}

TEST(OrderBookTest, CancelUnknownOrderIsRejected) {
    // Canceling an id that was never submitted should fail cleanly.
    OrderBook book;

    const auto result = book.cancel(60);

    expect_rejected(result);
}

TEST(OrderBookTest, CancelRemovesEmptyPriceLevel) {
    // Create two bid levels so canceling one level should leave the other intact.
    OrderBook book;
    submit_accepted(book, make_order(70, Side::Buy, 100, 10));
    submit_accepted(book, make_order(71, Side::Buy, 99, 5));

    const auto result = book.cancel(70);

    // The canceled level should disappear while the lower bid remains.
    expect_canceled(result, 70);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[70 BUY 100x10]"), std::string::npos);
    EXPECT_NE(snapshot.find("[71 BUY 99x5]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, CancelPreservesFifoPriorityOfRemainingOrders) {
    // Three orders at one price make it easy to remove the middle queue entry.
    OrderBook book;
    submit_accepted(book, make_order(80, Side::Buy, 100, 5));
    submit_accepted(book, make_order(81, Side::Buy, 100, 5));
    submit_accepted(book, make_order(82, Side::Buy, 100, 5));
    std::vector<Event> match_events;

    const auto cancel_result = book.cancel(81);
    expect_canceled(cancel_result, 81);

    // Matching should skip the canceled order and preserve the remaining FIFO order.
    book.submit(make_order(83, Side::Sell, 100, 8), match_events);

    ASSERT_EQ(match_events.size(), 3U);
    expect_accepted(match_events.front());
    expect_trade(match_events[1], 80, 83, 100, 5);
    expect_trade(match_events[2], 82, 83, 100, 3);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[80 BUY 100x5]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[81 BUY 100x5]"), std::string::npos);
    EXPECT_NE(snapshot.find("[82 BUY 100x2]"), std::string::npos);
}

TEST(OrderBookTest, CancelAfterPartialFillRemovesOnlyRemainingQuantity) {
    // First create a partial fill that leaves the incoming buy resting.
    OrderBook book;
    submit_accepted(book, make_order(90, Side::Sell, 100, 5));
    std::vector<Event> submit_events;

    book.submit(make_order(91, Side::Buy, 101, 8), submit_events);
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 90, 91, 100, 5);
    EXPECT_NE(book.snapshot().find("[91 BUY 101x3]"), std::string::npos);

    // Canceling the incoming order should remove only its remaining quantity.
    const auto cancel_result = book.cancel(91);

    expect_canceled(cancel_result, 91);
    EXPECT_EQ(book.snapshot().find("[91 BUY 101x3]"), std::string::npos);
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
    expect_canceled(copy_cancel, 93);
    EXPECT_EQ(copy.snapshot().find("[93 BUY 100x5]"), std::string::npos);
    EXPECT_NE(original.snapshot().find("[93 BUY 100x5]"), std::string::npos);
}

TEST(OrderBookTest, FullyFilledOrdersCannotBeCanceled) {
    // Submit two orders that fully trade and leave no resting quantity.
    OrderBook book;
    submit_accepted(book, make_order(100, Side::Sell, 100, 5));
    std::vector<Event> submit_events;

    book.submit(make_order(101, Side::Buy, 100, 5), submit_events);
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 100, 101, 100, 5);

    // Neither side should be cancelable after the full fill.
    const auto resting_cancel = book.cancel(100);
    expect_rejected(resting_cancel);

    const auto incoming_cancel = book.cancel(101);
    expect_rejected(incoming_cancel);
}

TEST(OrderBookTest, ReduceQuantityModifyPreservesFifoPriority) {
    // Seed two same-price bids so queue order is observable after modification.
    OrderBook book;
    submit_accepted(book, make_order(600, Side::Buy, 100, 5));
    submit_accepted(book, make_order(601, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(600, 100, 3, events);

    // Reducing quantity in place should not move the older order behind its peer.
    ASSERT_EQ(events.size(), 1U);
    expect_modified(events.front(), 600, 100, 100, 5, 3);

    book.submit(make_order(602, Side::Sell, 100, 4), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 600, 602, 100, 3);
    expect_trade(events[2], 601, 602, 100, 1);
}

TEST(OrderBookTest, IncreaseQuantityModifyLosesFifoPriority) {
    // Seed two same-price bids, then increase the older order's visible quantity.
    OrderBook book;
    submit_accepted(book, make_order(610, Side::Buy, 100, 5));
    submit_accepted(book, make_order(611, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(610, 100, 7, events);

    // Increasing quantity is cancel-replace and should move the order to the tail.
    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 610, 100, 100, 5, 7);

    book.submit(make_order(612, Side::Sell, 100, 6), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 611, 612, 100, 5);
    expect_trade(events[2], 610, 612, 100, 1);
}

TEST(OrderBookTest, PriceChangeModifyLosesFifoPriority) {
    // Move a lower bid up to a price level that already has resting liquidity.
    OrderBook book;
    submit_accepted(book, make_order(620, Side::Buy, 100, 5));
    submit_accepted(book, make_order(621, Side::Buy, 101, 5));
    std::vector<Event> events;

    book.modify(620, 101, 5, events);

    // Price changes are cancel-replace and should join the new level behind old orders.
    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 620, 100, 101, 5, 5);

    book.submit(make_order(622, Side::Sell, 101, 6), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 621, 622, 101, 5);
    expect_trade(events[2], 620, 622, 101, 1);
}

TEST(OrderBookTest, ModifyUnknownOrderRejects) {
    // Unknown ids cannot be modified because only resting orders have queue state.
    OrderBook book;
    std::vector<Event> events;

    book.modify(630, 100, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::UnknownOrderId, 630);
}

TEST(OrderBookTest, ModifyInvalidPriceOrQuantityRejectsWithoutChangingOrder) {
    // Seed a live order so invalid modify input can be rejected before mutation.
    OrderBook book;
    submit_accepted(book, make_order(635, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(635, 0, 4, events);

    // Invalid prices should reject and leave the original resting quantity intact.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 635);
    EXPECT_NE(book.snapshot().find("[635 BUY 100x5]"), std::string::npos);

    book.modify(635, 100, 0, events);

    // Empty replacement quantities should also leave the live order untouched.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 635);
    EXPECT_NE(book.snapshot().find("[635 BUY 100x5]"), std::string::npos);
}

TEST(OrderBookTest, ModifyNonRestingOrderRejects) {
    // Fill an order completely so neither side remains in the book-local index.
    OrderBook book;
    submit_accepted(book, make_order(640, Side::Sell, 100, 5));
    std::vector<Event> events;
    book.submit(make_order(641, Side::Buy, 100, 5), events);
    ASSERT_EQ(events.size(), 2U);

    book.modify(640, 101, 5, events);

    // Fully filled orders are no longer resting and should reject as unknown.
    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::UnknownOrderId, 640);
}

TEST(OrderBookTest, ReplacementModifyCanMatchAndRestRemainder) {
    // Make a passive bid, then modify it to cross the current best ask.
    OrderBook book;
    submit_accepted(book, make_order(650, Side::Sell, 99, 4));
    submit_accepted(book, make_order(651, Side::Buy, 98, 5));
    std::vector<Event> events;

    book.modify(651, 100, 7, events);

    // Replace emits replacement semantics, then reuses normal matching and resting.
    ASSERT_EQ(events.size(), 2U);
    expect_replaced(events.front(), 651, 98, 100, 5, 7);
    expect_trade(events[1], 650, 651, 99, 4);
    EXPECT_NE(book.snapshot().find("[651 BUY 100x3]"), std::string::npos);
}

TEST(OrderBookTest, ReplacementModifyDoesNotEmitCancelAcceptPair) {
    // Trigger cancel-replace with a same-price quantity increase.
    OrderBook book;
    submit_accepted(book, make_order(660, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(660, 100, 6, events);

    // The observable event stream should stay atomic from the client's perspective.
    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 660, 100, 100, 5, 6);
    EXPECT_FALSE(std::holds_alternative<CanceledEvent>(events.front()));
    EXPECT_FALSE(std::holds_alternative<AcceptedEvent>(events.front()));
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
    EXPECT_LT(position_of(snapshot, "[2 BUY 105x10]"),
              position_of(snapshot, "[3 BUY 105x5]"));
    EXPECT_LT(position_of(snapshot, "[2 BUY 105x10]"),
              position_of(snapshot, "[1 BUY 100x10]"));
    EXPECT_LT(position_of(snapshot, "[4 SELL 106x7]"),
              position_of(snapshot, "[5 SELL 110x8]"));
}
