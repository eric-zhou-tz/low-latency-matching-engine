#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using matching_engine::Event;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::OrderId;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::Side;
using matching_engine::TimeInForce;

constexpr std::uint32_t kSeed = 0xC0FFEE;
constexpr Price kMinPrice = 90;
constexpr Price kMaxPrice = 110;
constexpr Quantity kMinQuantity = 1;
constexpr Quantity kMaxQuantity = 25;

/**
 * @brief Builds an order with deterministic randomized fields.
 */
[[nodiscard]] Order make_order(OrderId id,
                               Side side,
                               Price price,
                               Quantity quantity,
                               TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Keep generated orders free of intrusive links before the book prepares them.
    return {.id = id,
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

/**
 * @brief Converts a side to compact operation-log text.
 */
[[nodiscard]] std::string side_text(Side side) {
    // Use stable labels so a failing operation sequence can be replayed manually.
    return side == Side::Buy ? "BUY" : "SELL";
}

/**
 * @brief Converts a time-in-force value to compact operation-log text.
 */
[[nodiscard]] std::string tif_text(TimeInForce time_in_force) {
    // Match the parser-visible names used by normal order scripts.
    return matching_engine::to_string(time_in_force);
}

/**
 * @brief Formats the last operations that led to an invariant failure.
 */
[[nodiscard]] std::string recent_operations(const std::vector<std::string>& operations) {
    constexpr std::size_t kMaxOperationsToPrint = 50;
    const std::size_t first =
        operations.size() > kMaxOperationsToPrint ? operations.size() - kMaxOperationsToPrint : 0;

    std::ostringstream output;
    output << "seed=0xC0FFEE recent_ops=[";

    for (std::size_t index = first; index < operations.size(); ++index) {
        // Include operation numbers to make the failing prefix easy to isolate.
        output << "\n  " << index << ": " << operations[index];
    }

    output << "\n]";
    return output.str();
}

/**
 * @brief Collects live order ids from the debug snapshot in deterministic order.
 */
[[nodiscard]] std::vector<OrderId> sorted_live_order_ids(const OrderBook::DebugSnapshot& snapshot) {
    std::vector<OrderId> ids(snapshot.indexed_order_ids.begin(), snapshot.indexed_order_ids.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

/**
 * @brief Verifies that best bid and best ask do not cross.
 */
void validate_book_not_crossed(const OrderBook::DebugSnapshot& snapshot) {
    if (snapshot.bids.empty() || snapshot.asks.empty()) {
        return;
    }

    // A crossed resting book would mean executable liquidity was left unmatched.
    EXPECT_LT(snapshot.bids.front().price, snapshot.asks.front().price)
        << "best_bid=" << snapshot.bids.front().price
        << " best_ask=" << snapshot.asks.front().price;
}

/**
 * @brief Verifies that the live id index equals the orders stored in queues.
 */
void validate_index_consistency(const OrderBook& book, const OrderBook::DebugSnapshot& snapshot) {
    std::unordered_set<OrderId> queue_ids;

    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // Every queued bid must also be reachable by id lookup.
            EXPECT_TRUE(queue_ids.insert(order.id).second) << "duplicate live order id " << order.id;
            EXPECT_TRUE(snapshot.indexed_order_ids.contains(order.id))
                << "queued bid missing from index id=" << order.id;
            EXPECT_TRUE(book.contains_order(order.id)) << "contains_order false for bid id="
                                                       << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // Every queued ask must also be reachable by id lookup.
            EXPECT_TRUE(queue_ids.insert(order.id).second) << "duplicate live order id " << order.id;
            EXPECT_TRUE(snapshot.indexed_order_ids.contains(order.id))
                << "queued ask missing from index id=" << order.id;
            EXPECT_TRUE(book.contains_order(order.id)) << "contains_order false for ask id="
                                                       << order.id;
        }
    }

    EXPECT_EQ(snapshot.indexed_order_ids.size(), queue_ids.size())
        << "order id index size does not match live resting order count";

    for (const OrderId order_id : snapshot.indexed_order_ids) {
        // Each index entry must point back to exactly one queued resting order.
        EXPECT_TRUE(queue_ids.contains(order_id)) << "index id missing from queues id=" << order_id;
    }
}

/**
 * @brief Verifies that no live id appears more than once.
 */
void validate_no_duplicate_order_ids(const OrderBook::DebugSnapshot& snapshot) {
    std::unordered_set<OrderId> seen;

    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // Duplicate ids would make cancel/modify routing ambiguous.
            EXPECT_TRUE(seen.insert(order.id).second) << "duplicate bid id=" << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // Duplicate ids would make cancel/modify routing ambiguous.
            EXPECT_TRUE(seen.insert(order.id).second) << "duplicate ask id=" << order.id;
        }
    }
}

/**
 * @brief Verifies aggregate level volume against contained order quantities.
 */
void validate_price_level_volumes(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        Quantity summed_quantity = 0;
        for (const auto& order : level.orders) {
            // Summing queue quantities catches missed aggregate updates after fills/modifies.
            summed_quantity += order.quantity;
        }
        EXPECT_EQ(level.total_volume, summed_quantity) << "bid price=" << level.price;
    }

    for (const auto& level : snapshot.asks) {
        Quantity summed_quantity = 0;
        for (const auto& order : level.orders) {
            // Summing queue quantities catches missed aggregate updates after fills/modifies.
            summed_quantity += order.quantity;
        }
        EXPECT_EQ(level.total_volume, summed_quantity) << "ask price=" << level.price;
    }
}

/**
 * @brief Verifies that empty levels were erased.
 */
void validate_no_empty_price_levels(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        // Empty levels waste tree nodes and can corrupt best-price checks.
        EXPECT_FALSE(level.orders.empty()) << "empty bid level price=" << level.price;
        EXPECT_GT(level.total_volume, 0U) << "zero-volume bid level price=" << level.price;
    }

    for (const auto& level : snapshot.asks) {
        // Empty levels waste tree nodes and can corrupt best-price checks.
        EXPECT_FALSE(level.orders.empty()) << "empty ask level price=" << level.price;
        EXPECT_GT(level.total_volume, 0U) << "zero-volume ask level price=" << level.price;
    }
}

/**
 * @brief Verifies that resting orders always have positive remaining quantity.
 */
void validate_no_zero_quantity_resting_orders(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // A zero-quantity order should have been removed immediately after a fill.
            EXPECT_GT(order.quantity, 0U) << "zero-quantity bid id=" << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // A zero-quantity order should have been removed immediately after a fill.
            EXPECT_GT(order.quantity, 0U) << "zero-quantity ask id=" << order.id;
        }
    }
}

/**
 * @brief Verifies best bid and ask ordering when both sides are present.
 */
void validate_best_bid_best_ask_ordering(const OrderBook::DebugSnapshot& snapshot) {
    if (snapshot.bids.empty() || snapshot.asks.empty()) {
        return;
    }

    // The first map entry is the best price for each side.
    EXPECT_LT(snapshot.bids.front().price, snapshot.asks.front().price)
        << "best bid must be below best ask";
}

/**
 * @brief Verifies that resting orders are stored on the correct side and price.
 */
void validate_resting_orders_match_level_side_and_price(
    const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // Bid-side levels must contain only buy orders at the level price.
            EXPECT_EQ(order.side, Side::Buy) << "bid level has non-buy id=" << order.id;
            EXPECT_EQ(order.price, level.price) << "bid id stored at wrong price id=" << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // Ask-side levels must contain only sell orders at the level price.
            EXPECT_EQ(order.side, Side::Sell) << "ask level has non-sell id=" << order.id;
            EXPECT_EQ(order.price, level.price) << "ask id stored at wrong price id=" << order.id;
        }
    }
}

/**
 * @brief Verifies total live quantity is consistent across all structures.
 */
void validate_total_live_quantity_consistency(const OrderBook::DebugSnapshot& snapshot) {
    Quantity total_level_quantity = 0;
    Quantity total_order_quantity = 0;

    for (const auto& level : snapshot.bids) {
        // Level totals should add up to the same quantity as individual orders.
        total_level_quantity += level.total_volume;
        for (const auto& order : level.orders) {
            total_order_quantity += order.quantity;
        }
    }

    for (const auto& level : snapshot.asks) {
        // Level totals should add up to the same quantity as individual orders.
        total_level_quantity += level.total_volume;
        for (const auto& order : level.orders) {
            total_order_quantity += order.quantity;
        }
    }

    EXPECT_EQ(total_level_quantity, total_order_quantity);
}

/**
 * @brief Verifies touched ids that are no longer queued are absent from lookup.
 */
void validate_absent_order_ids_are_not_live(
    const OrderBook& book,
    const OrderBook::DebugSnapshot& snapshot,
    const std::unordered_set<OrderId>& observed_order_ids) {
    for (const OrderId order_id : observed_order_ids) {
        if (snapshot.indexed_order_ids.contains(order_id)) {
            continue;
        }

        // Canceled, rejected, market, IOC/FOK, or fully-filled ids must not remain live.
        EXPECT_FALSE(book.contains_order(order_id)) << "absent order id is still live id="
                                                    << order_id;
    }
}

/**
 * @brief Runs every invariant check against the current book.
 */
void validate_all_invariants(const OrderBook& book,
                             const std::vector<std::string>& operations,
                             const std::unordered_set<OrderId>& observed_order_ids) {
    const auto snapshot = book.debug_snapshot();
    SCOPED_TRACE(recent_operations(operations));

    // Check each invariant independently so one failure reports the exact violated property.
    validate_book_not_crossed(snapshot);
    validate_index_consistency(book, snapshot);
    validate_no_duplicate_order_ids(snapshot);
    validate_price_level_volumes(snapshot);
    validate_no_empty_price_levels(snapshot);
    validate_no_zero_quantity_resting_orders(snapshot);
    validate_best_bid_best_ask_ordering(snapshot);
    validate_absent_order_ids_are_not_live(book, snapshot, observed_order_ids);
    validate_resting_orders_match_level_side_and_price(snapshot);
    validate_total_live_quantity_consistency(snapshot);
}

class RandomOperationGenerator {
public:
    /**
     * @brief Creates a deterministic operation generator.
     */
    explicit RandomOperationGenerator(std::mt19937& rng) : rng_(rng) {
        // Reserve common operation buffers so stress tests do not depend on realloc patterns.
        events_.reserve(32);
    }

    /**
     * @brief Applies one deterministic pseudo-random operation to the book.
     */
    void apply_next(OrderBook& book, std::vector<std::string>& operations) {
        const auto live_ids = sorted_live_order_ids(book.debug_snapshot());
        const int operation = uniform_int(0, 8);

        switch (operation) {
        case 0:
            submit_limit(book, operations, Side::Buy, TimeInForce::GoodTilCancel);
            break;
        case 1:
            submit_limit(book, operations, Side::Sell, TimeInForce::GoodTilCancel);
            break;
        case 2:
            cancel_existing_or_unknown(book, operations, live_ids);
            break;
        case 3:
            cancel_unknown(book, operations);
            break;
        case 4:
            modify_existing_or_unknown(book, operations, live_ids);
            break;
        case 5:
            modify_unknown(book, operations);
            break;
        case 6:
            submit_market(book, operations, random_side());
            break;
        case 7:
            submit_limit(book, operations, random_side(), TimeInForce::ImmediateOrCancel);
            break;
        case 8:
            submit_limit(book, operations, random_side(), TimeInForce::FillOrKill);
            break;
        }
    }

    /**
     * @brief Returns every id touched by generated operations.
     */
    [[nodiscard]] const std::unordered_set<OrderId>& observed_order_ids() const {
        // The observed set supports absence checks for canceled and fully filled orders.
        return observed_order_ids_;
    }

private:
    /**
     * @brief Draws an integer from a closed range.
     */
    [[nodiscard]] int uniform_int(int low, int high) {
        // A local distribution keeps each draw explicit and reproducible from the fixed seed.
        return std::uniform_int_distribution<int>{low, high}(rng_);
    }

    /**
     * @brief Draws a random side.
     */
    [[nodiscard]] Side random_side() {
        // Sides are balanced so both bid and ask paths receive randomized pressure.
        return uniform_int(0, 1) == 0 ? Side::Buy : Side::Sell;
    }

    /**
     * @brief Draws a randomized limit price.
     */
    [[nodiscard]] Price random_price() {
        // Keep prices clustered so randomized submissions frequently cross.
        return static_cast<Price>(uniform_int(static_cast<int>(kMinPrice),
                                             static_cast<int>(kMaxPrice)));
    }

    /**
     * @brief Draws a randomized non-zero quantity.
     */
    [[nodiscard]] Quantity random_quantity() {
        // Generated orders avoid invalid zero quantity unless a specific test needs it.
        return static_cast<Quantity>(uniform_int(static_cast<int>(kMinQuantity),
                                                static_cast<int>(kMaxQuantity)));
    }

    /**
     * @brief Picks one live id from a deterministic sorted id vector.
     */
    [[nodiscard]] OrderId choose_live_id(const std::vector<OrderId>& live_ids) {
        // Sorting before selection avoids iteration-order dependence from the hash index.
        const auto index =
            static_cast<std::size_t>(uniform_int(0, static_cast<int>(live_ids.size() - 1)));
        return live_ids[index];
    }

    /**
     * @brief Creates a unique id for new incoming orders.
     */
    [[nodiscard]] OrderId next_known_id() {
        // New ids never intentionally collide with live ids in this stress generator.
        return next_order_id_++;
    }

    /**
     * @brief Creates an id that should not exist in the live book.
     */
    [[nodiscard]] OrderId next_unknown_id() {
        // Unknown ids live far away from generated order ids so collisions are avoided.
        return next_unknown_order_id_++;
    }

    /**
     * @brief Submits a randomized limit order.
     */
    void submit_limit(OrderBook& book,
                      std::vector<std::string>& operations,
                      Side side,
                      TimeInForce time_in_force) {
        const OrderId id = next_known_id();
        const Price price = random_price();
        const Quantity quantity = random_quantity();

        // Submit through the public API so matching behavior stays production-real.
        observed_order_ids_.insert(id);
        book.submit(make_order(id, side, price, quantity, time_in_force), events_);

        std::ostringstream operation;
        operation << "SUBMIT " << id << ' ' << side_text(side) << ' ' << price << ' ' << quantity
                  << ' ' << tif_text(time_in_force);
        operations.push_back(operation.str());
    }

    /**
     * @brief Submits a randomized market order.
     */
    void submit_market(OrderBook& book, std::vector<std::string>& operations, Side side) {
        const OrderId id = next_known_id();
        const Quantity quantity = random_quantity();

        // Market orders should never rest, even when only partially filled.
        observed_order_ids_.insert(id);
        book.submit_market(make_order(id, side, 0, quantity), events_);

        std::ostringstream operation;
        operation << "MARKET " << id << ' ' << side_text(side) << ' ' << quantity;
        operations.push_back(operation.str());
    }

    /**
     * @brief Cancels a live order when possible, otherwise exercises unknown cancel.
     */
    void cancel_existing_or_unknown(OrderBook& book,
                                    std::vector<std::string>& operations,
                                    const std::vector<OrderId>& live_ids) {
        const OrderId id = live_ids.empty() ? next_unknown_id() : choose_live_id(live_ids);

        // A cancel either removes one live node or leaves the book unchanged when unknown.
        observed_order_ids_.insert(id);
        static_cast<void>(book.cancel(id));

        std::ostringstream operation;
        operation << "CANCEL_EXISTING " << id;
        operations.push_back(operation.str());
    }

    /**
     * @brief Cancels an id that should be absent from the live index.
     */
    void cancel_unknown(OrderBook& book, std::vector<std::string>& operations) {
        const OrderId id = next_unknown_id();

        // Unknown cancels validate rejection paths without changing book state.
        observed_order_ids_.insert(id);
        static_cast<void>(book.cancel(id));

        std::ostringstream operation;
        operation << "CANCEL_UNKNOWN " << id;
        operations.push_back(operation.str());
    }

    /**
     * @brief Modifies a live order when possible, otherwise exercises unknown modify.
     */
    void modify_existing_or_unknown(OrderBook& book,
                                    std::vector<std::string>& operations,
                                    const std::vector<OrderId>& live_ids) {
        const OrderId id = live_ids.empty() ? next_unknown_id() : choose_live_id(live_ids);
        const Price price = random_price();
        const Quantity quantity = random_quantity();

        // Existing modifies may reduce in place, replace, match, or fully fill.
        observed_order_ids_.insert(id);
        book.modify(id, price, quantity, events_);

        std::ostringstream operation;
        operation << "MODIFY_EXISTING " << id << ' ' << price << ' ' << quantity;
        operations.push_back(operation.str());
    }

    /**
     * @brief Modifies an id that should be absent from the live index.
     */
    void modify_unknown(OrderBook& book, std::vector<std::string>& operations) {
        const OrderId id = next_unknown_id();
        const Price price = random_price();
        const Quantity quantity = random_quantity();

        // Unknown modifies validate rejection paths without changing book state.
        observed_order_ids_.insert(id);
        book.modify(id, price, quantity, events_);

        std::ostringstream operation;
        operation << "MODIFY_UNKNOWN " << id << ' ' << price << ' ' << quantity;
        operations.push_back(operation.str());
    }

    std::mt19937& rng_;
    OrderId next_order_id_{1};
    OrderId next_unknown_order_id_{1'000'000'000};
    std::vector<Event> events_;
    std::unordered_set<OrderId> observed_order_ids_;
};

/**
 * @brief Runs a deterministic randomized invariant stress test.
 */
void run_randomized_stress(std::size_t operation_count) {
    std::mt19937 rng{kSeed};
    RandomOperationGenerator generator{rng};
    OrderBook book{operation_count + 128};
    std::vector<std::string> operations;
    operations.reserve(operation_count);

    for (std::size_t index = 0; index < operation_count; ++index) {
        // Validate after every operation so the failing prefix is as small as possible.
        generator.apply_next(book, operations);
        validate_all_invariants(book, operations, generator.observed_order_ids());
    }
}

} // namespace

/**
 * @brief Exercises a quick deterministic randomized operation sequence.
 */
TEST(OrderBookInvariantTest, SmallRandomizedStressMaintainsBookInvariants) {
    // Small stress keeps a quick randomized invariant pass in the normal test suite.
    run_randomized_stress(1'000);
}

/**
 * @brief Exercises a larger deterministic randomized operation sequence.
 */
TEST(OrderBookInvariantTest, MediumRandomizedStressMaintainsBookInvariants) {
    // Medium stress gives modifies, cancels, IOC/FOK, and market paths more mixing time.
    run_randomized_stress(10'000);
}
