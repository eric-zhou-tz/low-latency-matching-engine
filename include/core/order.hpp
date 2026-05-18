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
 * @brief Lifetime policy for unfilled limit-order quantity.
 */
enum class TimeInForce {
    GoodTilCancel,
    ImmediateOrCancel
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
    TimeInForce time_in_force{TimeInForce::GoodTilCancel};
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

/**
 * @brief Converts a time-in-force policy to a stable display string.
 *
 * @param time_in_force Lifetime policy to format.
 * @return "GTC" for resting limits and "IOC" for immediate-or-cancel limits.
 */
[[nodiscard]] constexpr const char* to_string(TimeInForce time_in_force) noexcept {
    // Keep the protocol labels in one place for parser tests and diagnostics.
    return time_in_force == TimeInForce::ImmediateOrCancel ? "IOC" : "GTC";
}

} // namespace matching_engine
