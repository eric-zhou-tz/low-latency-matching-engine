#include "parser.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using matching_engine::CancelOrderAction;
using matching_engine::Parser;
using matching_engine::SubmitOrderAction;

} // namespace

TEST(ParserTest, ParsesSubmitOrderCommand) {
    matching_engine::Parser parser;

    const auto submit = parser.parse_line("SUBMIT 1 AAPL BUY 100 10");
    ASSERT_TRUE(submit.has_value());
    EXPECT_TRUE(std::holds_alternative<SubmitOrderAction>(*submit));
}

TEST(ParserTest, ParsesCancelOrderCommand) {
    Parser parser;

    const auto cancel = parser.parse_line("CANCEL 1");
    ASSERT_TRUE(cancel.has_value());
    EXPECT_TRUE(std::holds_alternative<CancelOrderAction>(*cancel));
}

TEST(ParserTest, RejectsMalformedSubmitOrderCommand) {
    Parser parser;

    const auto invalid = parser.parse_line("SUBMIT bad");
    EXPECT_FALSE(invalid.has_value());
}
