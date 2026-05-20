#include "io/parser.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using matching_engine::CancelOrderAction;
using matching_engine::AddSymbolAction;
using matching_engine::MarketOrderAction;
using matching_engine::ModifyOrderAction;
using matching_engine::Parser;
using matching_engine::PriceLevelMode;
using matching_engine::PrintBookAction;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;
using matching_engine::TimeInForce;

} // namespace

TEST(ParserTest, ParsesAddSymbolTreeCommand) {
    Parser parser;

    const auto add_symbol = parser.parse_line("ADD_SYMBOL AAPL TREE");
    ASSERT_TRUE(add_symbol.has_value());
    ASSERT_TRUE(std::holds_alternative<AddSymbolAction>(*add_symbol));

    const auto& action = std::get<AddSymbolAction>(*add_symbol);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.price_level_mode, PriceLevelMode::Tree);
    EXPECT_EQ(action.base_tick, 0);
    EXPECT_EQ(action.tick_range, 0);
}

TEST(ParserTest, ParsesAddSymbolLadderCommand) {
    Parser parser;

    const auto add_symbol = parser.parse_line("ADD_SYMBOL AAPL LADDER BASE 18500 RANGE 5000");
    ASSERT_TRUE(add_symbol.has_value());
    ASSERT_TRUE(std::holds_alternative<AddSymbolAction>(*add_symbol));

    const auto& action = std::get<AddSymbolAction>(*add_symbol);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.price_level_mode, PriceLevelMode::Ladder);
    EXPECT_EQ(action.base_tick, 18500);
    EXPECT_EQ(action.tick_range, 5000);
}

TEST(ParserTest, ParsesSubmitOrderCommand) {
    matching_engine::Parser parser;

    const auto submit = parser.parse_line("SUBMIT 1 AAPL BUY 100 10");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 1U);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.side, Side::Buy);
    EXPECT_EQ(action.price, 100);
    EXPECT_EQ(action.quantity, 10U);
    EXPECT_EQ(action.time_in_force, TimeInForce::GoodTilCancel);
}

TEST(ParserTest, ParsesIocSubmitOrderCommand) {
    Parser parser;

    const auto submit = parser.parse_line("SUBMIT 3 AAPL BUY 100 10 IOC");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 3U);
    EXPECT_EQ(action.time_in_force, TimeInForce::ImmediateOrCancel);
}

TEST(ParserTest, ParsesFokSubmitOrderCommand) {
    Parser parser;

    const auto submit = parser.parse_line("SUBMIT 4 AAPL BUY 100 10 FOK");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 4U);
    EXPECT_EQ(action.time_in_force, TimeInForce::FillOrKill);
}

TEST(ParserTest, ParsesCancelOrderCommand) {
    Parser parser;

    const auto cancel = parser.parse_line("CANCEL 1");
    ASSERT_TRUE(cancel.has_value());
    ASSERT_TRUE(std::holds_alternative<CancelOrderAction>(*cancel));
    EXPECT_EQ(std::get<CancelOrderAction>(*cancel).order_id, 1U);
}

TEST(ParserTest, ParsesModifyOrderCommand) {
    Parser parser;

    const auto modify = parser.parse_line("MODIFY 7 105 12");
    ASSERT_TRUE(modify.has_value());
    ASSERT_TRUE(std::holds_alternative<ModifyOrderAction>(*modify));

    const auto& action = std::get<ModifyOrderAction>(*modify);
    EXPECT_EQ(action.order_id, 7U);
    EXPECT_EQ(action.new_price, 105);
    EXPECT_EQ(action.new_quantity, 12U);
}

TEST(ParserTest, ParsesMarketOrderCommand) {
    Parser parser;

    const auto market = parser.parse_line("MARKET 2 AAPL SELL 25");
    ASSERT_TRUE(market.has_value());
    ASSERT_TRUE(std::holds_alternative<MarketOrderAction>(*market));

    const auto& action = std::get<MarketOrderAction>(*market);
    EXPECT_EQ(action.id, 2U);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.side, Side::Sell);
    EXPECT_EQ(action.quantity, 25U);
}

TEST(ParserTest, ParsesPrintBookCommand) {
    Parser parser;

    const auto print = parser.parse_line("PRINT");

    ASSERT_TRUE(print.has_value());
    EXPECT_TRUE(std::holds_alternative<PrintBookAction>(*print));
}

TEST(ParserTest, RejectsMalformedSubmitOrderCommand) {
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT bad");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsMalformedAddSymbolCommands) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL TREE EXTRA").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL HASH").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL LADDER").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL LADDER BASE 18500").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL LADDER RANGE 5000").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL LADDER BASE 18500 RANGE -1").has_value());
    EXPECT_FALSE(parser.parse_line("ADD_SYMBOL AAPL LADDER BASE 18500 RANGE 5000 EXTRA").has_value());
}

TEST(ParserTest, RejectsSubmitOrderCommandWithExtraTokens) {
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT 6 AAPL SELL 100 10 GTC EXTRA");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsUnknownSubmitTimeInForce) {
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT 5 AAPL BUY 100 10 DAY");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsMarketOrderCommandWithBadSideOrQuantity) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL HOLD 10").has_value());
    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL BUY 0").has_value());
}

TEST(ParserTest, RejectsMarketOrderCommandWithExtraTokens) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL BUY 10 EXTRA").has_value());
}

TEST(ParserTest, RejectsCancelOrderCommandWithExtraTokens) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("CANCEL 8 EXTRA").has_value());
}

TEST(ParserTest, RejectsModifyOrderCommandWithExtraTokensOrZeroQuantity) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MODIFY 9 101 0").has_value());
    EXPECT_FALSE(parser.parse_line("MODIFY 9 101 5 EXTRA").has_value());
}

TEST(ParserTest, RejectsPrintBookCommandWithExtraTokens) {
    Parser parser;

    EXPECT_FALSE(parser.parse_line("PRINT AAPL").has_value());
}
