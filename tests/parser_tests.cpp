#include "io/parser.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using matching_engine::CancelOrderAction;
using matching_engine::Parser;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;

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

TEST(ParserTest, RejectsMalformedSubmitOrderCommand) {
    // Missing fields should make the parser return no action.
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT bad");
    EXPECT_FALSE(invalid.has_value());
}
