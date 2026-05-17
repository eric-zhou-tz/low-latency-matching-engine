#pragma once

#include <cstdint>
namespace matching_engine {

using OrderId = std::uint64_t;

/**
 * @brief Side of an order.
 */
enum class Side {
    Buy,
    Sell
};

/**
 * @brief Hot-path order representation stored by a single-symbol book.
 *
 * Price and quantity use integer fields to keep the core deterministic and avoid
 * floating-point surprises. The symbol is intentionally kept outside this
 * struct because OrderBook already owns orders for exactly one symbol.
 */
struct Order {
    std::uint64_t id{};
    Side side{Side::Buy};
    std::int64_t price{};
    std::uint64_t quantity{};
    Order* prev{};
    Order* next{};
};

/**
 * @brief Converts an order side to a stable display string.
 *
 * @param side Side to format.
 * @return "BUY" for bids and "SELL" for asks.
 */
[[nodiscard]] constexpr const char* to_string(Side side) noexcept {
    // Keep formatting stable for snapshots, events, and tests.
    return side == Side::Buy ? "BUY" : "SELL";
}

} // namespace matching_engine
