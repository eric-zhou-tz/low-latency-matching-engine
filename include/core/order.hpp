#pragma once

#include <cstdint>
namespace matching_engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;

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
    ImmediateOrCancel,
    FillOrKill
};

/**
 * @brief Hot-path order representation stored by a single-symbol book.
 *
 * Price and quantity use integer fields to keep the core deterministic and avoid
 * floating-point surprises. The symbol is intentionally kept outside this
 * struct because OrderBook already owns orders for exactly one symbol.
 */
struct Order {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
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
 * @return Stable protocol label for the time-in-force policy.
 */
[[nodiscard]] constexpr const char* to_string(TimeInForce time_in_force) noexcept {
    // Keep the protocol labels in one place for parser tests and diagnostics.
    switch (time_in_force) {
    case TimeInForce::GoodTilCancel:
        return "GTC";
    case TimeInForce::ImmediateOrCancel:
        return "IOC";
    case TimeInForce::FillOrKill:
        return "FOK";
    }

    // Return the default resting policy label for defensive future enum changes.
    return "GTC";
}

} // namespace matching_engine
