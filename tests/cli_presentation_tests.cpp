#include "io/cli_presentation.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

/**
 * @brief Runs the interactive CLI against scripted user input.
 */
[[nodiscard]] std::string run_cli_text(const std::string& input_text) {
    std::istringstream input{input_text};
    std::ostringstream output;

    // Drive the public presentation boundary so menu routing and manual mode are covered together.
    matching_engine::run_cli_presentation(input, output);

    return output.str();
}

/**
 * @brief Checks whether CLI output contains a readable fragment.
 */
void expect_contains(const std::string& output, const std::string& fragment) {
    // Substring checks keep the test resilient to menu copy while pinning functional output.
    EXPECT_NE(output.find(fragment), std::string::npos) << output;
}

/**
 * @brief Checks whether CLI output omits a fragment.
 */
void expect_not_contains(const std::string& output, const std::string& fragment) {
    // Manual mode should stay compact and avoid guided-demo trace text.
    EXPECT_EQ(output.find(fragment), std::string::npos) << output;
}

} // namespace

TEST(CliPresentationTest, ManualModeAcceptsCommandsAndPrintsOrderBookState) {
    const std::string output = run_cli_text(R"(4
SUBMIT 1 AAPL BUY 100 10 GTC
SUBMIT 2 AAPL SELL 105 3 GTC
PRINT
EXIT
7
)");

    expect_contains(output, "Manual command mode.");
    expect_contains(output,
                    "ACCEPTED order_id=1 symbol=AAPL side=BUY price=100 quantity=10 tif=GTC "
                    "status=RESTING");
    expect_contains(output,
                    "ACCEPTED order_id=2 symbol=AAPL side=SELL price=105 quantity=3 tif=GTC "
                    "status=RESTING");
    expect_contains(output, "Book: AAPL");
    expect_contains(output, "BIDS");
    expect_contains(output, "ASKS");
    expect_contains(output, "price=100");
    expect_contains(output, "qty=10");
    expect_contains(output, "id=1");
    expect_contains(output, "price=105");
    expect_contains(output, "qty=3");
    expect_contains(output, "id=2");
    expect_not_contains(output, "Live execution trace:");
}

TEST(CliPresentationTest, ManualModeRejectsBadInputAndContinuesAcceptingCommands) {
    const std::string output = run_cli_text(R"(4
SUBMIT bad
SUBMIT 10 MSFT BUY 200 4
PRINT
EXIT
7
)");

    expect_contains(output, "REJECTED invalid command");
    expect_contains(output, "Book: MSFT");
    expect_contains(output, "price=200");
    expect_contains(output, "qty=4");
    expect_contains(output, "id=10");
    expect_not_contains(output, "Live execution trace:");
}
