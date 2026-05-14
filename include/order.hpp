#pragma once

#include <cstdint>
#include <string>

namespace matching_engine {

/**
 * @brief Side of an order.
 */
enum class Side {
    Buy,
    Sell
};

/**
 * @brief Minimal order representation used by the first scaffold.
 *
 * Price and quantity use integer fields to keep the core deterministic and avoid
 * floating-point surprises. A production engine would usually define explicit
 * tick-size and lot-size types around these primitives.
 */
struct Order {
    std::uint64_t id{};
    std::string symbol;
    Side side{Side::Buy};
    std::int64_t price{};
    std::uint64_t quantity{};
};

/**
 * @brief Converts an order side to a stable display string.
 *
 * @param side Side to format.
 * @return "BUY" for bids and "SELL" for asks.
 */
[[nodiscard]] constexpr const char* to_string(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

} // namespace matching_engine
