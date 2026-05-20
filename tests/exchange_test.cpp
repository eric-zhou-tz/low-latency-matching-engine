#include "exchange.hpp"
#include "io/script_runner.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

using matching_engine::AcceptedEvent;
using matching_engine::Action;
using matching_engine::AddSymbolAction;
using matching_engine::BookSnapshotEvent;
using matching_engine::CancelOrderAction;
using matching_engine::CanceledEvent;
using matching_engine::Event;
using matching_engine::Exchange;
using matching_engine::MarketOrderAction;
using matching_engine::ModifyOrderAction;
using matching_engine::ModifiedEvent;
using matching_engine::PrintBookAction;
using matching_engine::PriceLevelMode;
using matching_engine::RejectedEvent;
using matching_engine::RejectReason;
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
    return {.id = id, .symbol = std::move(symbol), .side = side, .quantity = quantity};
}

/**
 * @brief Submits an order through the public exchange API.
 */
void submit(Exchange& exchange, SubmitOrderAction action, std::vector<Event>& out) {
    exchange.process(Action{std::move(action)}, out);
}

/**
 * @brief Submits a market order through the public exchange API.
 */
void submit_market(Exchange& exchange, MarketOrderAction action, std::vector<Event>& out) {
    exchange.process(Action{std::move(action)}, out);
}

/**
 * @brief Registers a tree-backed symbol through the public exchange action path.
 */
void add_symbol(Exchange& exchange, const std::string& symbol, std::vector<Event>& out) {
    exchange.process(Action{AddSymbolAction{.symbol = symbol,
                                            .price_level_mode = PriceLevelMode::Tree}},
                     out);
}

/**
 * @brief Builds an exchange with the requested tree-backed symbols already registered.
 */
[[nodiscard]] Exchange make_exchange_with_symbols(std::initializer_list<std::string> symbols) {
    Exchange exchange;
    std::vector<Event> events;

    for (const auto& symbol : symbols) {
        // Tests that focus on order flow start from explicit symbol registration.
        add_symbol(exchange, symbol, events);
        EXPECT_TRUE(events.empty());
    }

    return exchange;
}

/**
 * @brief Cancels an order through the public exchange API.
 */
void cancel(Exchange& exchange, std::uint64_t order_id, std::vector<Event>& out) {
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
    exchange.process(Action{ModifyOrderAction{order_id, new_price, new_quantity}}, out);
}

/**
 * @brief Requests snapshots for all known symbol books.
 */
void print_book(Exchange& exchange, std::vector<Event>& out) {
    exchange.process(Action{PrintBookAction{}}, out);
}

/**
 * @brief Verifies the first event is an acceptance.
 */
void expect_accepted(const std::vector<Event>& events) {
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(std::holds_alternative<AcceptedEvent>(events.front()));
}

/**
 * @brief Verifies a one-event cancel response.
 */
void expect_canceled(const std::vector<Event>& events, std::uint64_t order_id) {
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<CanceledEvent>(events.front()));
    EXPECT_EQ(std::get<CanceledEvent>(events.front()).order_id, order_id);
}

/**
 * @brief Verifies a one-event rejection response.
 */
void expect_rejected(const std::vector<Event>& events, const std::string& reason) {
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
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<ModifiedEvent>(events.front()));
    EXPECT_EQ(std::get<ModifiedEvent>(events.front()).order_id, order_id);
}

/**
 * @brief Verifies a replace response starts with ReplacedEvent.
 */
void expect_replaced(const std::vector<Event>& events, std::uint64_t order_id) {
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
    for (const auto& event : events) {
        const auto* snapshot = std::get_if<BookSnapshotEvent>(&event);
        if (snapshot != nullptr && snapshot->message.find(token) != std::string::npos) {
            return snapshot;
        }
    }

    return nullptr;
}

/**
 * @brief Runs raw command text through the same boundary used by the CLI.
 */
[[nodiscard]] std::string run_script_text(const std::string& script) {
    std::istringstream input{script};
    std::ostringstream output;

    matching_engine::run_script(input, output);

    return output.str();
}

/**
 * @brief Asserts the full formatted output for a command script.
 */
void expect_script_output(const std::string& script, const std::string& expected) {
    EXPECT_EQ(run_script_text(script), expected);
}

} // namespace

TEST(ExchangeTest, DuplicateAddSymbolIsRejected) {
    Exchange exchange;
    std::vector<Event> events;
    add_symbol(exchange, "AAPL", events);
    EXPECT_TRUE(events.empty());

    add_symbol(exchange, "AAPL", events);

    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(events.front()));
    const auto& rejected = std::get<RejectedEvent>(events.front());
    EXPECT_EQ(rejected.reason, RejectReason::DuplicateSymbol);
    EXPECT_EQ(rejected.order_id, 0U);
}

TEST(ExchangeTest, SubmitBeforeAddSymbolIsRejected) {
    Exchange exchange;
    std::vector<Event> events;

    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);

    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(events.front()));
    const auto& rejected = std::get<RejectedEvent>(events.front());
    EXPECT_EQ(rejected.reason, RejectReason::UnknownSymbol);
    EXPECT_EQ(rejected.order_id, 1U);
}

TEST(ExchangeTest, SubmitAfterAddSymbolWorks) {
    Exchange exchange;
    std::vector<Event> events;
    add_symbol(exchange, "AAPL", events);
    EXPECT_TRUE(events.empty());

    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);

    expect_accepted(events);
}

TEST(ExchangeTest, AddSymbolLadderThenSubmitWorks) {
    Exchange exchange;
    std::vector<Event> events;
    exchange.process(Action{AddSymbolAction{.symbol = "AAPL",
                                            .price_level_mode = PriceLevelMode::Ladder,
                                            .base_tick = 18500,
                                            .tick_range = 5000}},
                     events);
    EXPECT_TRUE(events.empty());

    submit(exchange, make_submit(1, "AAPL", Side::Sell, 18500, 5), events);
    expect_accepted(events);
    submit(exchange, make_submit(2, "AAPL", Side::Buy, 18500, 5), events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events);
    expect_trade(events[1], 1, 2, 18500, 5);
}

TEST(ExchangeTest, LadderSubmitOutsideRangeRejectsWithoutCreatingRoute) {
    Exchange exchange;
    std::vector<Event> events;
    exchange.process(Action{AddSymbolAction{.symbol = "AAPL",
                                            .price_level_mode = PriceLevelMode::Ladder,
                                            .base_tick = 100,
                                            .tick_range = 2}},
                     events);
    EXPECT_TRUE(events.empty());

    submit(exchange, make_submit(1, "AAPL", Side::Buy, 103, 5), events);

    expect_rejected(events, "invalid order 1");
    cancel(exchange, 1, events);
    expect_rejected(events, "unknown order id 1");
}

TEST(ExchangeTest, CancelRoutesDirectlyToOwningSymbolBook) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);
    submit(exchange, make_submit(2, "MSFT", Side::Buy, 200, 20), events);
    expect_accepted(events);

    cancel(exchange, 2, events);

    expect_canceled(events, 2);
    cancel(exchange, 1, events);
    expect_canceled(events, 1);
}

TEST(ExchangeTest, DuplicateOrderIdAcrossSymbolsIsRejectedUntilOriginalLeavesIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(1, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    submit(exchange, make_submit(1, "MSFT", Side::Sell, 200, 5), events);

    expect_rejected(events, "duplicate order id 1");

    cancel(exchange, 1, events);
    expect_canceled(events, 1);
    submit(exchange, make_submit(1, "MSFT", Side::Sell, 200, 5), events);

    expect_accepted(events);
}

TEST(ExchangeTest, MarketOrderDuplicateIdIsRejectedAgainstRestingOrder) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(2, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    submit_market(exchange, make_market(2, "MSFT", Side::Sell, 5), events);

    expect_rejected(events, "duplicate order id 2");
}

TEST(ExchangeTest, UnknownCancelStillReturnsRejectedEvent) {
    Exchange exchange;
    std::vector<Event> events;

    cancel(exchange, 404, events);

    expect_rejected(events, "unknown order id 404");
}

TEST(ExchangeTest, CancelRemovesOrderFromExchangeIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(10, "AAPL", Side::Sell, 105, 7), events);
    expect_accepted(events);

    cancel(exchange, 10, events);
    expect_canceled(events, 10);
    submit(exchange, make_submit(10, "MSFT", Side::Buy, 205, 3), events);

    expect_accepted(events);
}

TEST(ExchangeTest, SameOrderIdCannotBeCanceledTwiceSuccessfully) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(11, "AAPL", Side::Buy, 101, 4), events);
    expect_accepted(events);
    cancel(exchange, 11, events);
    expect_canceled(events, 11);

    cancel(exchange, 11, events);

    expect_rejected(events, "unknown order id 11");
}

TEST(ExchangeTest, MultiSymbolCancelDoesNotInvalidateUnrelatedBook) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(20, "AAPL", Side::Sell, 110, 5), events);
    expect_accepted(events);
    submit(exchange, make_submit(21, "MSFT", Side::Sell, 210, 6), events);
    expect_accepted(events);

    cancel(exchange, 20, events);
    expect_canceled(events, 20);

    cancel(exchange, 21, events);
    expect_canceled(events, 21);
}

TEST(ExchangeTest, CrossSymbolOrdersDoNotMatchOrAffectEachOther) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(22, "AAPL", Side::Sell, 100, 5), events);
    expect_accepted(events);

    submit(exchange, make_submit(23, "MSFT", Side::Buy, 101, 5), events);

    ASSERT_EQ(events.size(), 1U);
    expect_accepted(events);

    cancel(exchange, 22, events);
    expect_canceled(events, 22);
    cancel(exchange, 23, events);
    expect_canceled(events, 23);
}

TEST(ExchangeTest, FilledRestingOrdersLeaveExchangeIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(30, "AAPL", Side::Sell, 100, 5), events);
    expect_accepted(events);
    submit(exchange, make_submit(31, "AAPL", Side::Buy, 101, 5), events);

    ASSERT_EQ(events.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(events[1]));
    EXPECT_EQ(std::get<TradeEvent>(events[1]).resting_order_id, 30);

    submit(exchange, make_submit(30, "MSFT", Side::Buy, 200, 1), events);

    expect_accepted(events);
    cancel(exchange, 30, events);
    expect_canceled(events, 30);
}

TEST(ExchangeTest, PartialRestingFillKeepsRemainingOrderIndexed) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(32, "AAPL", Side::Sell, 100, 7), events);
    expect_accepted(events);
    submit(exchange, make_submit(33, "AAPL", Side::Buy, 101, 3), events);

    ASSERT_EQ(events.size(), 2U);
    expect_trade(events[1], 32, 33, 100, 3);

    cancel(exchange, 32, events);
    expect_canceled(events, 32);
    cancel(exchange, 33, events);
    expect_rejected(events, "unknown order id 33");
}

TEST(ExchangeTest, IocRemainderDoesNotEnterCancelIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange,
           make_submit(40, "AAPL", Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel),
           events);

    expect_accepted(events);
    cancel(exchange, 40, events);
    expect_rejected(events, "unknown order id 40");

    submit(exchange, make_submit(40, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}

TEST(ExchangeTest, FokRejectDoesNotEnterCancelIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(41, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange, make_submit(42, "AAPL", Side::Buy, 101, 8, TimeInForce::FillOrKill), events);

    expect_rejected(events, "insufficient liquidity 42");
    cancel(exchange, 42, events);
    expect_rejected(events, "unknown order id 42");
}

TEST(ExchangeTest, MarketOrderTradesAndDoesNotEnterCancelIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(50, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit_market(exchange, make_market(51, "AAPL", Side::Buy, 8), events);

    ASSERT_EQ(events.size(), 3U);
    expect_accepted(events);
    expect_trade(events[1], 50, 51, 100, 3);
    ASSERT_TRUE(std::holds_alternative<RejectedEvent>(events[2]));
    EXPECT_EQ(matching_engine::format_event(events[2]), "REJECTED insufficient liquidity 51");

    cancel(exchange, 50, events);
    expect_rejected(events, "unknown order id 50");
    cancel(exchange, 51, events);
    expect_rejected(events, "unknown order id 51");
}

TEST(ExchangeTest, IocOrderTradesAndDoesNotRestRemainder) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(60, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange,
           make_submit(61, "AAPL", Side::Buy, 101, 8, TimeInForce::ImmediateOrCancel),
           events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events);
    expect_trade(events[1], 60, 61, 100, 3);

    cancel(exchange, 61, events);
    expect_rejected(events, "unknown order id 61");
    submit(exchange, make_submit(61, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}

TEST(ExchangeTest, FullyFilledIocOrderRemovesRestingIndexAndDoesNotIndexIncoming) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(62, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);

    submit(exchange,
           make_submit(63, "AAPL", Side::Buy, 100, 3, TimeInForce::ImmediateOrCancel),
           events);

    ASSERT_EQ(events.size(), 2U);
    expect_accepted(events);
    expect_trade(events[1], 62, 63, 100, 3);

    cancel(exchange, 62, events);
    expect_rejected(events, "unknown order id 62");
    cancel(exchange, 63, events);
    expect_rejected(events, "unknown order id 63");
}

TEST(ExchangeTest, ModifyReductionKeepsExchangeIndexCancelable) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(70, "AAPL", Side::Buy, 100, 10), events);
    expect_accepted(events);

    modify(exchange, 70, 100, 4, events);
    expect_modified(events, 70);

    cancel(exchange, 70, events);
    expect_canceled(events, 70);
}

TEST(ExchangeTest, ReplacementModifyKeepsIndexWhenRemainderRests) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(80, "AAPL", Side::Buy, 100, 5), events);
    expect_accepted(events);

    modify(exchange, 80, 100, 7, events);
    expect_replaced(events, 80);

    cancel(exchange, 80, events);
    expect_canceled(events, 80);
}

TEST(ExchangeTest, ReplacementModifyFullyExecutedLeavesNoIndex) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(90, "AAPL", Side::Sell, 99, 4), events);
    expect_accepted(events);
    submit(exchange, make_submit(91, "AAPL", Side::Buy, 98, 4), events);
    expect_accepted(events);

    modify(exchange, 91, 100, 4, events);

    ASSERT_EQ(events.size(), 2U);
    expect_replaced(events, 91);
    expect_trade(events[1], 90, 91, 99, 4);
    cancel(exchange, 91, events);
    expect_rejected(events, "unknown order id 91");
    submit(exchange, make_submit(91, "MSFT", Side::Sell, 200, 1), events);
    expect_accepted(events);
}

TEST(ExchangeTest, ReplacementModifyEmitsReplaceBeforeTradesInPriceTimeOrder) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
    std::vector<Event> events;
    submit(exchange, make_submit(92, "AAPL", Side::Sell, 99, 2), events);
    expect_accepted(events);
    submit(exchange, make_submit(93, "AAPL", Side::Sell, 100, 3), events);
    expect_accepted(events);
    submit(exchange, make_submit(94, "AAPL", Side::Buy, 98, 6), events);
    expect_accepted(events);

    modify(exchange, 94, 100, 6, events);

    ASSERT_EQ(events.size(), 3U);
    expect_replaced(events, 94);
    expect_trade(events[1], 92, 94, 99, 2);
    expect_trade(events[2], 93, 94, 100, 3);

    cancel(exchange, 94, events);
    expect_canceled(events, 94);
}

TEST(ExchangeTest, ModifyUnknownAndNonRestingOrdersReject) {
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
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
    Exchange exchange = make_exchange_with_symbols({"AAPL"});
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
    Exchange exchange;
    std::vector<Event> events;

    print_book(exchange, events);

    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<BookSnapshotEvent>(events.front()));
    EXPECT_EQ(std::get<BookSnapshotEvent>(events.front()).message, "book: empty");
    EXPECT_EQ(matching_engine::format_event(events.front()), "book: empty");
}

TEST(ExchangeTest, PrintBookReportsEachSymbolWithoutCrossContamination) {
    Exchange exchange = make_exchange_with_symbols({"AAPL", "MSFT"});
    std::vector<Event> events;
    submit(exchange, make_submit(120, "AAPL", Side::Buy, 100, 4), events);
    expect_accepted(events);
    submit(exchange, make_submit(121, "MSFT", Side::Sell, 200, 6), events);
    expect_accepted(events);

    print_book(exchange, events);

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

TEST(IntegrationTest, BasicSubmitAndPrint) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 1 AAPL BUY 100 10
PRINT
)",
                         R"(ACCEPTED accepted order 1
book AAPL: orders=1 [1 BUY 100x10]
)");
}

TEST(IntegrationTest, CrossingBuySellProducesTrades) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 10 AAPL SELL 100 5
SUBMIT 11 AAPL BUY 100 5
PRINT
)",
                         R"(ACCEPTED accepted order 10
ACCEPTED accepted order 11
TRADE resting=10 incoming=11 price=100 quantity=5
book AAPL: orders=0
)");
}

TEST(IntegrationTest, PartialFillLeavesRemainderResting) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 20 AAPL SELL 100 5
SUBMIT 21 AAPL BUY 101 8
PRINT
)",
                         R"(ACCEPTED accepted order 20
ACCEPTED accepted order 21
TRADE resting=20 incoming=21 price=100 quantity=5
book AAPL: orders=1 [21 BUY 101x3]
)");
}

TEST(IntegrationTest, CancelExistingOrder) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 30 AAPL BUY 99 4
CANCEL 30
PRINT
)",
                         R"(ACCEPTED accepted order 30
CANCELED order_id=30
book AAPL: orders=0
)");
}

TEST(IntegrationTest, CancelUnknownOrderRejects) {
    expect_script_output(R"(CANCEL 404
)",
                         R"(REJECTED unknown order id 404
)");
}

TEST(IntegrationTest, DuplicateOrderIdRejects) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 40 AAPL BUY 100 10
SUBMIT 40 AAPL SELL 101 2
PRINT
)",
                         R"(ACCEPTED accepted order 40
REJECTED duplicate order id 40
book AAPL: orders=1 [40 BUY 100x10]
)");
}

TEST(IntegrationTest, ModifyExistingOrder) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 50 AAPL BUY 100 10
MODIFY 50 100 6
PRINT
)",
                         R"(ACCEPTED accepted order 50
MODIFIED order_id=50 old_price=100 new_price=100 old_quantity=10 new_quantity=6
book AAPL: orders=1 [50 BUY 100x6]
)");
}

TEST(IntegrationTest, ModifyUnknownOrderRejects) {
    expect_script_output(R"(MODIFY 999 100 5
)",
                         R"(REJECTED unknown order id 999
)");
}

TEST(IntegrationTest, MarketOrderFullFill) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 60 AAPL SELL 100 4
MARKET 61 AAPL BUY 4
PRINT
)",
                         R"(ACCEPTED accepted order 60
ACCEPTED accepted order 61
TRADE resting=60 incoming=61 price=100 quantity=4
book AAPL: orders=0
)");
}

TEST(IntegrationTest, MarketOrderInsufficientLiquidityRejectsRemainder) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 70 AAPL SELL 100 3
MARKET 71 AAPL BUY 5
PRINT
)",
                         R"(ACCEPTED accepted order 70
ACCEPTED accepted order 71
TRADE resting=70 incoming=71 price=100 quantity=3
REJECTED insufficient liquidity 71
book AAPL: orders=0
)");
}

TEST(IntegrationTest, IocPartialFillCancelsRemainder) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 80 AAPL SELL 100 3
SUBMIT 81 AAPL BUY 100 5 IOC
PRINT
)",
                         R"(ACCEPTED accepted order 80
ACCEPTED accepted order 81
TRADE resting=80 incoming=81 price=100 quantity=3
book AAPL: orders=0
)");
}

TEST(IntegrationTest, FokFullFillSucceeds) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 90 AAPL SELL 100 2
SUBMIT 91 AAPL SELL 101 3
SUBMIT 92 AAPL BUY 101 5 FOK
PRINT
)",
                         R"(ACCEPTED accepted order 90
ACCEPTED accepted order 91
ACCEPTED accepted order 92
TRADE resting=90 incoming=92 price=100 quantity=2
TRADE resting=91 incoming=92 price=101 quantity=3
book AAPL: orders=0
)");
}

TEST(IntegrationTest, FokRejectDoesNotMutateBook) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
SUBMIT 100 AAPL SELL 100 2
SUBMIT 101 AAPL BUY 100 3 FOK
PRINT
)",
                         R"(ACCEPTED accepted order 100
REJECTED insufficient liquidity 101
book AAPL: orders=1 [100 SELL 100x2]
)");
}

TEST(IntegrationTest, MultiSymbolRoutingKeepsBooksIndependent) {
    expect_script_output(R"(ADD_SYMBOL AAPL TREE
ADD_SYMBOL MSFT TREE
SUBMIT 110 AAPL BUY 100 5
SUBMIT 111 MSFT SELL 200 7
SUBMIT 112 AAPL SELL 100 2
PRINT
)",
                         R"(ACCEPTED accepted order 110
ACCEPTED accepted order 111
ACCEPTED accepted order 112
TRADE resting=110 incoming=112 price=100 quantity=2
book AAPL: orders=1 [110 BUY 100x3]
book MSFT: orders=1 [111 SELL 200x7]
)");
}

TEST(IntegrationTest, DeterministicReplayProducesIdenticalOutput) {
    const std::string script = R"(ADD_SYMBOL AAPL TREE
ADD_SYMBOL MSFT TREE
SUBMIT 120 AAPL BUY 100 5
SUBMIT 121 AAPL SELL 99 2
SUBMIT 122 MSFT BUY 50 1
PRINT
)";

    EXPECT_EQ(run_script_text(script), run_script_text(script));
}

TEST(IntegrationTest, MalformedInputLinesProduceStableRejections) {
    expect_script_output(R"(SUBMIT bad
MARKET 130 AAPL HOLD 5
PRINT AAPL
PRINT
)",
                         R"(REJECTED invalid command
REJECTED invalid command
REJECTED invalid command
book: empty
)");
}
