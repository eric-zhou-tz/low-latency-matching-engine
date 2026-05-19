#include "io/parser.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using matching_engine::CancelOrderAction;
using matching_engine::MarketOrderAction;
using matching_engine::ModifyOrderAction;
using matching_engine::Parser;
using matching_engine::PrintBookAction;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;
using matching_engine::TimeInForce;

} // namespace

TEST(ParserTest, ParsesSubmitOrderCommand) {
    // Parse a complete SUBMIT command.
    matching_engine::Parser parser;

    const auto submit = parser.parse_line("SUBMIT 1 AAPL BUY 100 10");
    // Confirm parsing succeeded and produced the expected action variant.
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    // Verify routing and hot-path fields stay separated in the submit payload.
    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 1U);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.side, Side::Buy);
    EXPECT_EQ(action.price, 100);
    EXPECT_EQ(action.quantity, 10U);
    EXPECT_EQ(action.time_in_force, TimeInForce::GoodTilCancel);
}

TEST(ParserTest, ParsesIocSubmitOrderCommand) {
    // Parse a SUBMIT command with an explicit immediate-or-cancel flag.
    Parser parser;

    const auto submit = parser.parse_line("SUBMIT 3 AAPL BUY 100 10 IOC");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    // Verify the optional flag is carried into the submit action.
    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 3U);
    EXPECT_EQ(action.time_in_force, TimeInForce::ImmediateOrCancel);
}

TEST(ParserTest, ParsesFokSubmitOrderCommand) {
    // Parse a SUBMIT command with an explicit fill-or-kill flag.
    Parser parser;

    const auto submit = parser.parse_line("SUBMIT 4 AAPL BUY 100 10 FOK");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));

    // Verify the optional flag is carried into the submit action.
    const auto& action = std::get<SubmitOrderAction>(*submit);
    EXPECT_EQ(action.id, 4U);
    EXPECT_EQ(action.time_in_force, TimeInForce::FillOrKill);
}

TEST(ParserTest, ParsesCancelOrderCommand) {
    // Parse a simple cancel command with an order id.
    Parser parser;

    const auto cancel = parser.parse_line("CANCEL 1");
    // Confirm the action type and payload are preserved.
    ASSERT_TRUE(cancel.has_value());
    ASSERT_TRUE(std::holds_alternative<CancelOrderAction>(*cancel));
    EXPECT_EQ(std::get<CancelOrderAction>(*cancel).order_id, 1U);
}

TEST(ParserTest, ParsesModifyOrderCommand) {
    // Parse a modify command with replacement price and quantity.
    Parser parser;

    const auto modify = parser.parse_line("MODIFY 7 105 12");
    ASSERT_TRUE(modify.has_value());
    ASSERT_TRUE(std::holds_alternative<ModifyOrderAction>(*modify));

    // Verify the replacement fields are preserved for exchange routing.
    const auto& action = std::get<ModifyOrderAction>(*modify);
    EXPECT_EQ(action.order_id, 7U);
    EXPECT_EQ(action.new_price, 105);
    EXPECT_EQ(action.new_quantity, 12U);
}

TEST(ParserTest, ParsesMarketOrderCommand) {
    // Parse a market command with no limit price field.
    Parser parser;

    const auto market = parser.parse_line("MARKET 2 AAPL SELL 25");
    // Confirm parsing succeeded and produced the market action variant.
    ASSERT_TRUE(market.has_value());
    ASSERT_TRUE(std::holds_alternative<MarketOrderAction>(*market));

    // Verify routing fields and quantity are preserved.
    const auto& action = std::get<MarketOrderAction>(*market);
    EXPECT_EQ(action.id, 2U);
    EXPECT_EQ(action.symbol, "AAPL");
    EXPECT_EQ(action.side, Side::Sell);
    EXPECT_EQ(action.quantity, 25U);
}

TEST(ParserTest, ParsesPrintBookCommand) {
    // Parse a snapshot request with no payload.
    Parser parser;

    const auto print = parser.parse_line("PRINT");

    // Confirm the parser emits the explicit print action variant.
    ASSERT_TRUE(print.has_value());
    EXPECT_TRUE(std::holds_alternative<PrintBookAction>(*print));
}

TEST(ParserTest, RejectsMalformedSubmitOrderCommand) {
    // Missing fields should make the parser return no action.
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT bad");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsSubmitOrderCommandWithExtraTokens) {
    // Extra trailing fields should not be silently ignored.
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT 6 AAPL SELL 100 10 GTC EXTRA");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsUnknownSubmitTimeInForce) {
    // Unknown trailing flags should not be silently accepted.
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT 5 AAPL BUY 100 10 DAY");
    EXPECT_FALSE(invalid.has_value());
}

TEST(ParserTest, RejectsMarketOrderCommandWithBadSideOrQuantity) {
    // Market commands still need a valid side and non-zero quantity.
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL HOLD 10").has_value());
    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL BUY 0").has_value());
}

TEST(ParserTest, RejectsMarketOrderCommandWithExtraTokens) {
    // Market commands do not accept a price or time-in-force payload.
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MARKET 8 AAPL BUY 10 EXTRA").has_value());
}

TEST(ParserTest, RejectsCancelOrderCommandWithExtraTokens) {
    // Cancel commands should contain only the order id.
    Parser parser;

    EXPECT_FALSE(parser.parse_line("CANCEL 8 EXTRA").has_value());
}

TEST(ParserTest, RejectsModifyOrderCommandWithExtraTokensOrZeroQuantity) {
    // Modify must be a precise replacement payload.
    Parser parser;

    EXPECT_FALSE(parser.parse_line("MODIFY 9 101 0").has_value());
    EXPECT_FALSE(parser.parse_line("MODIFY 9 101 5 EXTRA").has_value());
}

TEST(ParserTest, RejectsPrintBookCommandWithExtraTokens) {
    // PRINT currently snapshots every known book and has no selector syntax.
    Parser parser;

    EXPECT_FALSE(parser.parse_line("PRINT AAPL").has_value());
}
