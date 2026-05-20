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
using matching_engine::PriceLevelMode;
using matching_engine::PriceTick;
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
    return {.id = id,
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

void expect_accepted(const Event& event) {
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(event));
}

void expect_rejected(const Event& event) {
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(event));
}

void expect_rejected(const Event& event, RejectReason reason, std::uint64_t order_id) {
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(event));
    const auto& rejected = std::get<RejectedEvent>(event);
    EXPECT_EQ(rejected.reason, reason);
    EXPECT_EQ(rejected.order_id, order_id);
}

void expect_rejected(const matching_engine::CancelResult& result) {
    EXPECT_TRUE(std::holds_alternative<RejectedEvent>(result));
}

void expect_canceled(const Event& event, std::uint64_t order_id) {
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(event));
    EXPECT_EQ(std::get<CanceledEvent>(event).order_id, order_id);
}

void expect_canceled(const matching_engine::CancelResult& result, std::uint64_t order_id) {
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(result));
    EXPECT_EQ(std::get<CanceledEvent>(result).order_id, order_id);
}

void expect_modified(const Event& event,
                     std::uint64_t order_id,
                     std::int64_t old_price,
                     std::int64_t new_price,
                     std::uint64_t old_quantity,
                     std::uint64_t new_quantity) {
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
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(event));

    const auto& trade = std::get<TradeEvent>(event);
    EXPECT_EQ(trade.resting_order_id, resting_order_id);
    EXPECT_EQ(trade.incoming_order_id, incoming_order_id);
    EXPECT_EQ(trade.price, price);
    EXPECT_EQ(trade.quantity, quantity);
}

void submit_accepted(OrderBook& book, Order order) {
    std::vector<Event> events;
    book.submit(std::move(order), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
}

[[nodiscard]] std::size_t position_of(const std::string& snapshot, const std::string& token) {
    const auto position = snapshot.find(token);
    EXPECT_NE(position, std::string::npos);
    return position;
}

} // namespace

TEST(OrderBookTest, DefaultConstructorUsesTreePriceLevelMode) {
    OrderBook book;

    EXPECT_EQ(book.price_level_mode(), PriceLevelMode::Tree);
    EXPECT_EQ(book.base_tick(), 0);
    EXPECT_EQ(book.tick_range(), 0);
    EXPECT_EQ(book.min_tick(), 0);
    EXPECT_EQ(book.max_tick(), 0);
}

TEST(OrderBookTest, ReserveConstructorUsesTreePriceLevelMode) {
    OrderBook book{1024};

    EXPECT_EQ(book.price_level_mode(), PriceLevelMode::Tree);
    EXPECT_EQ(book.base_tick(), 0);
    EXPECT_EQ(book.tick_range(), 0);
    EXPECT_EQ(book.min_tick(), 0);
    EXPECT_EQ(book.max_tick(), 0);
}

TEST(OrderBookTest, ReservedLadderConstructorStoresMetadataAndComputesBounds) {
    OrderBook book{1024, PriceTick{18500}, PriceTick{5000}};

    EXPECT_EQ(book.price_level_mode(), PriceLevelMode::Ladder);
    EXPECT_EQ(book.base_tick(), 18500);
    EXPECT_EQ(book.tick_range(), 5000);
    EXPECT_EQ(book.min_tick(), 13500);
    EXPECT_EQ(book.max_tick(), 23500);
    EXPECT_EQ(book.ladder_size(), 10001U);
}

TEST(OrderBookTest, LadderIndexingUsesMinTickAsZeroSlot) {
    OrderBook book{128, PriceTick{100}, PriceTick{2}};
    std::vector<Event> events;

    book.submit(make_order(1, Side::Buy, 98, 10), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    book.submit(make_order(2, Side::Sell, 102, 12), events);
    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());

    const auto snapshot = book.debug_snapshot();
    ASSERT_EQ(snapshot.bids.size(), 1U);
    ASSERT_EQ(snapshot.asks.size(), 1U);
    EXPECT_EQ(snapshot.bids.front().price, 98);
    EXPECT_EQ(snapshot.asks.front().price, 102);
}

TEST(OrderBookTest, LadderRejectsOutOfRangeLimitPriceSafely) {
    OrderBook book{128, PriceTick{100}, PriceTick{2}};
    std::vector<Event> events;

    book.submit(make_order(1, Side::Buy, 103, 10), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 1);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, LadderPreservesPriceTimePriorityAtSamePrice) {
    OrderBook book{128, PriceTick{100}, PriceTick{10}};
    submit_accepted(book, make_order(10, Side::Sell, 100, 5));
    submit_accepted(book, make_order(11, Side::Sell, 100, 5));
    std::vector<Event> events;

    book.submit(make_order(12, Side::Buy, 100, 7), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 10, 12, 100, 5);
    expect_trade(events[2], 11, 12, 100, 2);
    EXPECT_NE(book.snapshot().find("[11 SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, LadderMatchesBestAskAndBestBid) {
    OrderBook book{128, PriceTick{100}, PriceTick{10}};
    submit_accepted(book, make_order(20, Side::Sell, 105, 3));
    submit_accepted(book, make_order(21, Side::Sell, 103, 4));
    std::vector<Event> events;

    book.submit(make_order(22, Side::Buy, 110, 4), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 21, 22, 103, 4);
    submit_accepted(book, make_order(23, Side::Buy, 99, 6));
    submit_accepted(book, make_order(24, Side::Buy, 101, 7));

    book.submit(make_order(25, Side::Sell, 90, 7), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 24, 25, 101, 7);
}

TEST(OrderBookTest, LadderCancelsAndRemovesEmptyPriceLevel) {
    OrderBook book{128, PriceTick{100}, PriceTick{10}};
    submit_accepted(book, make_order(30, Side::Buy, 99, 6));

    const auto cancel_result = book.cancel(30);

    expect_canceled(cancel_result, 30);
    const auto snapshot = book.debug_snapshot();
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, LadderModifiesRestingOrdersAndRejectsOutOfRangeReplace) {
    OrderBook book{128, PriceTick{100}, PriceTick{10}};
    submit_accepted(book, make_order(40, Side::Buy, 99, 10));
    std::vector<Event> events;

    book.modify(40, 99, 4, events);

    ASSERT_EQ(events.size(), 1U);
    expect_modified(events.front(), 40, 99, 99, 10, 4);
    EXPECT_NE(book.snapshot().find("[40 BUY 99x4]"), std::string::npos);

    book.modify(40, 111, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 40);
    EXPECT_NE(book.snapshot().find("[40 BUY 99x4]"), std::string::npos);

    book.modify(40, 98, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 40, 99, 98, 4, 5);
    EXPECT_NE(book.snapshot().find("[40 BUY 98x5]"), std::string::npos);
}

TEST(OrderBookTest, AcceptedOrderRestsOnBookWhenNotCrossing) {
    OrderBook book;
    std::vector<Event> events;

    book.submit(make_order(1, Side::Buy, 100, 10), events);

    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("[1 BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, DuplicateOrderIdIsRejected) {
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));
    std::vector<Event> events;

    book.submit(make_order(1, Side::Sell, 101, 5), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front());
}

TEST(OrderBookTest, DuplicateOrderIdRejectsWithReasonAndPreservesOriginalOrder) {
    OrderBook book;
    submit_accepted(book, make_order(2, Side::Buy, 100, 10));
    submit_accepted(book, make_order(3, Side::Sell, 105, 4));
    std::vector<Event> events;

    book.submit(make_order(2, Side::Buy, 110, 4), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::DuplicateOrderId, 2);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[2 BUY 100x10]"), std::string::npos);
    EXPECT_NE(snapshot.find("[3 SELL 105x4]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveBuyMatchesRestingSell) {
    OrderBook book;
    submit_accepted(book, make_order(10, Side::Sell, 100, 10));
    std::vector<Event> events;

    book.submit(make_order(11, Side::Buy, 105, 4), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 10, 11, 100, 4);
    EXPECT_NE(book.snapshot().find("[10 SELL 100x6]"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[11 BUY 105x4]"), std::string::npos);
}

TEST(OrderBookTest, AggressiveSellMatchesRestingBuy) {
    OrderBook book;
    submit_accepted(book, make_order(30, Side::Buy, 101, 5));
    std::vector<Event> events;

    book.submit(make_order(31, Side::Sell, 100, 8), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 30, 31, 101, 5);
    EXPECT_EQ(book.snapshot().find("[30 BUY 101x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[31 SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PriceTimePriorityOlderRestingOrderAtSamePriceFillsFirst) {
    OrderBook book;
    submit_accepted(book, make_order(20, Side::Sell, 100, 5));
    submit_accepted(book, make_order(21, Side::Sell, 100, 5));
    std::vector<Event> events;

    book.submit(make_order(22, Side::Buy, 100, 7), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 20, 22, 100, 5);
    expect_trade(events[2], 21, 22, 100, 2);
    EXPECT_EQ(book.snapshot().find("[20 SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[21 SELL 100x3]"), std::string::npos);
}

TEST(OrderBookTest, PartialFillLeavesRemainingQuantityCorrectly) {
    OrderBook book;
    submit_accepted(book, make_order(40, Side::Sell, 100, 5));
    std::vector<Event> events;

    book.submit(make_order(41, Side::Buy, 101, 8), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 40, 41, 100, 5);
    EXPECT_EQ(book.snapshot().find("[40 SELL 100x5]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("[41 BUY 101x3]"), std::string::npos);
}

TEST(OrderBookTest, PartialFillLeavesRestingOrderCancelableWithReducedQuantity) {
    OrderBook book;
    submit_accepted(book, make_order(410, Side::Sell, 100, 8));
    std::vector<Event> events;

    book.submit(make_order(411, Side::Buy, 100, 3), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 410, 411, 100, 3);
    EXPECT_NE(book.snapshot().find("[410 SELL 100x5]"), std::string::npos);

    const auto cancel_result = book.cancel(410);
    expect_canceled(cancel_result, 410);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, AggressiveBuyWalksAskLevelsAndRestsRemainder) {
    OrderBook book;
    submit_accepted(book, make_order(42, Side::Sell, 100, 4));
    submit_accepted(book, make_order(43, Side::Sell, 101, 3));
    std::vector<Event> events;

    book.submit(make_order(44, Side::Buy, 102, 10), events);

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
    OrderBook book;
    submit_accepted(book, make_order(45, Side::Buy, 102, 4));
    submit_accepted(book, make_order(46, Side::Buy, 101, 3));
    std::vector<Event> events;

    book.submit(make_order(47, Side::Sell, 100, 10), events);

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
    OrderBook book;
    std::vector<Event> events;

    book.submit(
        make_order(480, Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel), events);

    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events.front());
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
    EXPECT_EQ(book.snapshot().find("[480 BUY"), std::string::npos);
    expect_rejected(book.cancel(480));
}

TEST(OrderBookTest, IocLimitOrderTradesAndCancelsRemainder) {
    OrderBook book;
    submit_accepted(book, make_order(481, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit(
        make_order(482, Side::Buy, 101, 8, TimeInForce::ImmediateOrCancel), events);

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
    OrderBook book;
    submit_accepted(book, make_order(483, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit(make_order(484, Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InsufficientLiquidity, 484);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[483 SELL 100x3]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[484 BUY"), std::string::npos);
}

TEST(OrderBookTest, FokLimitOrderFullyFillsAcrossPriceLevels) {
    OrderBook book;
    submit_accepted(book, make_order(485, Side::Sell, 100, 3));
    submit_accepted(book, make_order(486, Side::Sell, 101, 5));
    std::vector<Event> events;

    book.submit(make_order(487, Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 485, 487, 100, 3);
    expect_trade(events[2], 486, 487, 101, 5);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("orders=0"), std::string::npos);
    EXPECT_EQ(snapshot.find("[487 BUY"), std::string::npos);
}

TEST(OrderBookTest, FokExactFillEmitsAcceptThenTradesAndLeavesNoRemainder) {
    OrderBook book;
    submit_accepted(book, make_order(491, Side::Sell, 100, 2));
    submit_accepted(book, make_order(492, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit(make_order(493, Side::Buy, 100, 5, TimeInForce::FillOrKill), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 491, 493, 100, 2);
    expect_trade(events[2], 492, 493, 100, 3);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("orders=0"), std::string::npos);
    EXPECT_EQ(snapshot.find("[493 BUY"), std::string::npos);
}

TEST(OrderBookTest, FokSellRejectsWhenBestBidVolumeBelowLimitIsNotCrossing) {
    OrderBook book;
    submit_accepted(book, make_order(488, Side::Buy, 101, 3));
    submit_accepted(book, make_order(489, Side::Buy, 99, 10));
    std::vector<Event> events;

    book.submit(make_order(490, Side::Sell, 100, 8, TimeInForce::FillOrKill), events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InsufficientLiquidity, 490);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[488 BUY 101x3]"), std::string::npos);
    EXPECT_NE(snapshot.find("[489 BUY 99x10]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[490 SELL"), std::string::npos);
}

TEST(OrderBookTest, MarketBuyFullyFillsAgainstBestAsk) {
    OrderBook book;
    submit_accepted(book, make_order(500, Side::Sell, 100, 6));
    std::vector<Event> events;

    book.submit_market(make_order(501, Side::Buy, 0, 4), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 500, 501, 100, 4);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[500 SELL 100x2]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[501 BUY"), std::string::npos);
}

TEST(OrderBookTest, MarketBuySweepsMultipleAskPriceLevels) {
    OrderBook book;
    submit_accepted(book, make_order(510, Side::Sell, 100, 4));
    submit_accepted(book, make_order(511, Side::Sell, 101, 3));
    submit_accepted(book, make_order(512, Side::Sell, 102, 5));
    std::vector<Event> events;

    book.submit_market(make_order(513, Side::Buy, 0, 7), events);

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
    OrderBook book;
    submit_accepted(book, make_order(520, Side::Buy, 105, 6));
    std::vector<Event> events;

    book.submit_market(make_order(521, Side::Sell, 0, 4), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_trade(events[1], 520, 521, 105, 4);

    const auto snapshot = book.snapshot();
    EXPECT_NE(snapshot.find("[520 BUY 105x2]"), std::string::npos);
    EXPECT_EQ(snapshot.find("[521 SELL"), std::string::npos);
}

TEST(OrderBookTest, MarketSellSweepsMultipleBidPriceLevels) {
    OrderBook book;
    submit_accepted(book, make_order(530, Side::Buy, 105, 4));
    submit_accepted(book, make_order(531, Side::Buy, 104, 3));
    submit_accepted(book, make_order(532, Side::Buy, 103, 5));
    std::vector<Event> events;

    book.submit_market(make_order(533, Side::Sell, 0, 7), events);

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
    OrderBook book;
    submit_accepted(book, make_order(540, Side::Sell, 100, 3));
    std::vector<Event> events;

    book.submit_market(make_order(541, Side::Buy, 0, 8), events);

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
    OrderBook book;
    std::vector<Event> events;

    book.submit_market(make_order(550, Side::Sell, 0, 5), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events.front());
    expect_rejected(events[1], RejectReason::InsufficientLiquidity, 550);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, FullyFilledMarketOrderDoesNotEmitRemainderRejection) {
    OrderBook book;
    submit_accepted(book, make_order(555, Side::Buy, 105, 2));
    submit_accepted(book, make_order(556, Side::Buy, 104, 3));
    std::vector<Event> events;

    book.submit_market(make_order(557, Side::Sell, 0, 5), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 555, 557, 105, 2);
    expect_trade(events[2], 556, 557, 104, 3);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingBuyOrderSucceeds) {
    OrderBook book;
    submit_accepted(book, make_order(50, Side::Buy, 100, 10));

    const auto result = book.cancel(50);

    expect_canceled(result, 50);
    EXPECT_EQ(book.snapshot().find("[50 BUY 100x10]"), std::string::npos);
}

TEST(OrderBookTest, CancelRestingSellOrderSucceeds) {
    OrderBook book;
    submit_accepted(book, make_order(51, Side::Sell, 102, 7));

    const auto result = book.cancel(51);

    expect_canceled(result, 51);
    EXPECT_EQ(book.snapshot().find("[51 SELL 102x7]"), std::string::npos);
}

TEST(OrderBookTest, CancelUnknownOrderIsRejected) {
    OrderBook book;

    const auto result = book.cancel(60);

    expect_rejected(result);
}

TEST(OrderBookTest, CancelRemovesEmptyPriceLevel) {
    OrderBook book;
    submit_accepted(book, make_order(70, Side::Buy, 100, 10));
    submit_accepted(book, make_order(71, Side::Buy, 99, 5));

    const auto result = book.cancel(70);

    expect_canceled(result, 70);

    const auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.find("[70 BUY 100x10]"), std::string::npos);
    EXPECT_NE(snapshot.find("[71 BUY 99x5]"), std::string::npos);
    EXPECT_NE(snapshot.find("orders=1"), std::string::npos);
}

TEST(OrderBookTest, CancelPreservesFifoPriorityOfRemainingOrders) {
    OrderBook book;
    submit_accepted(book, make_order(80, Side::Buy, 100, 5));
    submit_accepted(book, make_order(81, Side::Buy, 100, 5));
    submit_accepted(book, make_order(82, Side::Buy, 100, 5));
    std::vector<Event> match_events;

    const auto cancel_result = book.cancel(81);
    expect_canceled(cancel_result, 81);

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
    OrderBook book;
    submit_accepted(book, make_order(90, Side::Sell, 100, 5));
    std::vector<Event> submit_events;

    book.submit(make_order(91, Side::Buy, 101, 8), submit_events);
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 90, 91, 100, 5);
    EXPECT_NE(book.snapshot().find("[91 BUY 101x3]"), std::string::npos);

    const auto cancel_result = book.cancel(91);

    expect_canceled(cancel_result, 91);
    EXPECT_EQ(book.snapshot().find("[91 BUY 101x3]"), std::string::npos);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
}

TEST(OrderBookTest, CopiedBookRebuildsCancelLocations) {
    OrderBook original;
    submit_accepted(original, make_order(92, Side::Buy, 100, 5));
    submit_accepted(original, make_order(93, Side::Buy, 100, 5));

    OrderBook copy = original;
    const auto copy_cancel = copy.cancel(93);

    expect_canceled(copy_cancel, 93);
    EXPECT_EQ(copy.snapshot().find("[93 BUY 100x5]"), std::string::npos);
    EXPECT_NE(original.snapshot().find("[93 BUY 100x5]"), std::string::npos);
}

TEST(OrderBookTest, FullyFilledOrdersCannotBeCanceled) {
    OrderBook book;
    submit_accepted(book, make_order(100, Side::Sell, 100, 5));
    std::vector<Event> submit_events;

    book.submit(make_order(101, Side::Buy, 100, 5), submit_events);
    ASSERT_EQ(submit_events.size(), 2U);
    expect_accepted(submit_events.front());
    expect_trade(submit_events[1], 100, 101, 100, 5);

    const auto resting_cancel = book.cancel(100);
    expect_rejected(resting_cancel);

    const auto incoming_cancel = book.cancel(101);
    expect_rejected(incoming_cancel);
}

TEST(OrderBookTest, ReduceQuantityModifyPreservesFifoPriority) {
    OrderBook book;
    submit_accepted(book, make_order(600, Side::Buy, 100, 5));
    submit_accepted(book, make_order(601, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(600, 100, 3, events);

    ASSERT_EQ(events.size(), 1U);
    expect_modified(events.front(), 600, 100, 100, 5, 3);

    book.submit(make_order(602, Side::Sell, 100, 4), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 600, 602, 100, 3);
    expect_trade(events[2], 601, 602, 100, 1);
}

TEST(OrderBookTest, IncreaseQuantityModifyLosesFifoPriority) {
    OrderBook book;
    submit_accepted(book, make_order(610, Side::Buy, 100, 5));
    submit_accepted(book, make_order(611, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(610, 100, 7, events);

    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 610, 100, 100, 5, 7);

    book.submit(make_order(612, Side::Sell, 100, 6), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 611, 612, 100, 5);
    expect_trade(events[2], 610, 612, 100, 1);
}

TEST(OrderBookTest, PriceChangeModifyLosesFifoPriority) {
    OrderBook book;
    submit_accepted(book, make_order(620, Side::Buy, 100, 5));
    submit_accepted(book, make_order(621, Side::Buy, 101, 5));
    std::vector<Event> events;

    book.modify(620, 101, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 620, 100, 101, 5, 5);

    book.submit(make_order(622, Side::Sell, 101, 6), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events.front());
    expect_trade(events[1], 621, 622, 101, 5);
    expect_trade(events[2], 620, 622, 101, 1);
}

TEST(OrderBookTest, ModifyUnknownOrderRejects) {
    OrderBook book;
    std::vector<Event> events;

    book.modify(630, 100, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::UnknownOrderId, 630);
}

TEST(OrderBookTest, ModifyInvalidPriceOrQuantityRejectsWithoutChangingOrder) {
    OrderBook book;
    submit_accepted(book, make_order(635, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(635, 0, 4, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 635);
    EXPECT_NE(book.snapshot().find("[635 BUY 100x5]"), std::string::npos);

    book.modify(635, 100, 0, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::InvalidOrder, 635);
    EXPECT_NE(book.snapshot().find("[635 BUY 100x5]"), std::string::npos);
}

TEST(OrderBookTest, ModifyNonRestingOrderRejects) {
    OrderBook book;
    submit_accepted(book, make_order(640, Side::Sell, 100, 5));
    std::vector<Event> events;
    book.submit(make_order(641, Side::Buy, 100, 5), events);
    ASSERT_EQ(events.size(), 2U);

    book.modify(640, 101, 5, events);

    ASSERT_EQ(events.size(), 1U);
    expect_rejected(events.front(), RejectReason::UnknownOrderId, 640);
}

TEST(OrderBookTest, ReplacementModifyCanMatchAndRestRemainder) {
    OrderBook book;
    submit_accepted(book, make_order(650, Side::Sell, 99, 4));
    submit_accepted(book, make_order(651, Side::Buy, 98, 5));
    std::vector<Event> events;

    book.modify(651, 100, 7, events);

    ASSERT_EQ(events.size(), 2U);
    expect_replaced(events.front(), 651, 98, 100, 5, 7);
    expect_trade(events[1], 650, 651, 99, 4);
    EXPECT_NE(book.snapshot().find("[651 BUY 100x3]"), std::string::npos);
}

TEST(OrderBookTest, ReplacementModifyFullyFillsLeavesModifiedOrderNonResting) {
    OrderBook book;
    submit_accepted(book, make_order(655, Side::Sell, 99, 4));
    submit_accepted(book, make_order(656, Side::Buy, 98, 4));
    std::vector<Event> events;

    book.modify(656, 100, 4, events);

    ASSERT_EQ(events.size(), 2U);
    expect_replaced(events.front(), 656, 98, 100, 4, 4);
    expect_trade(events[1], 655, 656, 99, 4);
    EXPECT_NE(book.snapshot().find("orders=0"), std::string::npos);
    expect_rejected(book.cancel(656));
}

TEST(OrderBookTest, ReplacementModifyDoesNotEmitCancelAcceptPair) {
    OrderBook book;
    submit_accepted(book, make_order(660, Side::Buy, 100, 5));
    std::vector<Event> events;

    book.modify(660, 100, 6, events);

    ASSERT_EQ(events.size(), 1U);
    expect_replaced(events.front(), 660, 100, 100, 5, 6);
    EXPECT_FALSE(std::holds_alternative<CanceledEvent>(events.front()));
    EXPECT_FALSE(std::holds_alternative<AcceptedEvent>(events.front()));
}

TEST(OrderBookTest, SnapshotPreservesBookOrdering) {
    OrderBook book;
    submit_accepted(book, make_order(1, Side::Buy, 100, 10));
    submit_accepted(book, make_order(2, Side::Buy, 105, 10));
    submit_accepted(book, make_order(3, Side::Buy, 105, 5));
    submit_accepted(book, make_order(4, Side::Sell, 106, 7));
    submit_accepted(book, make_order(5, Side::Sell, 110, 8));

    const auto snapshot = book.snapshot();

    EXPECT_LT(position_of(snapshot, "[2 BUY 105x10]"),
              position_of(snapshot, "[3 BUY 105x5]"));
    EXPECT_LT(position_of(snapshot, "[2 BUY 105x10]"),
              position_of(snapshot, "[1 BUY 100x10]"));
    EXPECT_LT(position_of(snapshot, "[4 SELL 106x7]"),
              position_of(snapshot, "[5 SELL 110x8]"));
}
