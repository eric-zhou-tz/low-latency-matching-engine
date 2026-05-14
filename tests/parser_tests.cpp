#include "parser.hpp"

#include <cassert>
#include <variant>

/**
 * @brief Minimal parser smoke tests.
 *
 * These are built as a simple executable so the scaffold has no external test
 * dependency. A future test framework can replace this file without changing
 * production targets.
 */
int main() {
    matching_engine::Parser parser;

    const auto submit = parser.parse_line("SUBMIT 1 AAPL BUY 100 10");
    assert(submit.has_value());
    assert(std::holds_alternative<matching_engine::SubmitOrderAction>(*submit));

    const auto cancel = parser.parse_line("CANCEL 1");
    assert(cancel.has_value());
    assert(std::holds_alternative<matching_engine::CancelOrderAction>(*cancel));

    const auto invalid = parser.parse_line("SUBMIT bad");
    assert(!invalid.has_value());

    return 0;
}
