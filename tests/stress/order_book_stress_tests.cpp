#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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

constexpr std::size_t kOperationCount = 1'000'000;
constexpr std::size_t kValidationInterval = 10'000;
constexpr std::uint32_t kMixedSeed = 0x51A7E5A1U;
constexpr Price kMidPrice = 10'000;

/**
 * @brief Builds a clean order object for stress submissions.
 */
[[nodiscard]] Order make_order(OrderId id,
                               Side side,
                               Price price,
                               Quantity quantity,
                               TimeInForce time_in_force = TimeInForce::GoodTilCancel) {
    // Intrusive queue links start empty so each submit owns link initialization.
    return {.id = id,
            .side = side,
            .price = price,
            .quantity = quantity,
            .time_in_force = time_in_force};
}

/**
 * @brief Converts a side to compact operation-log text.
 */
[[nodiscard]] const char* side_text(Side side) {
    // Failure traces use protocol-like side labels for easy manual replay.
    return side == Side::Buy ? "BUY" : "SELL";
}

/**
 * @brief Formats recent stress operations for failure diagnostics.
 */
[[nodiscard]] std::string recent_operations(const std::vector<std::string>& operations) {
    constexpr std::size_t kMaxOperationsToPrint = 40;
    const std::size_t first =
        operations.size() > kMaxOperationsToPrint ? operations.size() - kMaxOperationsToPrint : 0;

    std::ostringstream output;
    output << "recent_ops=[";
    for (std::size_t index = first; index < operations.size(); ++index) {
        // Numbered operations make it clear which long-run prefix produced the failure.
        output << "\n  " << index << ": " << operations[index];
    }
    output << "\n]";
    return output.str();
}

/**
 * @brief Captures live order ids from a book snapshot in deterministic order.
 */
[[nodiscard]] std::vector<OrderId> sorted_live_order_ids(const OrderBook::DebugSnapshot& snapshot) {
    std::vector<OrderId> ids(snapshot.indexed_order_ids.begin(), snapshot.indexed_order_ids.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

/**
 * @brief Verifies that the best bid and best ask are not crossed.
 */
void validate_book_not_crossed(const OrderBook::DebugSnapshot& snapshot) {
    if (snapshot.bids.empty() || snapshot.asks.empty()) {
        return;
    }

    // Resting crossed liquidity means matching left executable volume behind.
    EXPECT_LT(snapshot.bids.front().price, snapshot.asks.front().price)
        << "best_bid=" << snapshot.bids.front().price
        << " best_ask=" << snapshot.asks.front().price;
}

/**
 * @brief Verifies that every queued order is indexed exactly once.
 */
void validate_index_consistency(const OrderBook& book, const OrderBook::DebugSnapshot& snapshot) {
    std::unordered_set<OrderId> queued_ids;

    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // The id index is the cancel/modify lookup source of truth.
            EXPECT_TRUE(queued_ids.insert(order.id).second) << "duplicate live id=" << order.id;
            EXPECT_TRUE(snapshot.indexed_order_ids.contains(order.id))
                << "bid id missing from index id=" << order.id;
            EXPECT_TRUE(book.contains_order(order.id)) << "bid id not contained id=" << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // Ask-side orders must obey the same one-node, one-index-entry rule.
            EXPECT_TRUE(queued_ids.insert(order.id).second) << "duplicate live id=" << order.id;
            EXPECT_TRUE(snapshot.indexed_order_ids.contains(order.id))
                << "ask id missing from index id=" << order.id;
            EXPECT_TRUE(book.contains_order(order.id)) << "ask id not contained id=" << order.id;
        }
    }

    EXPECT_EQ(snapshot.indexed_order_ids.size(), queued_ids.size())
        << "live index size differs from queued order count";

    for (const OrderId order_id : snapshot.indexed_order_ids) {
        // Stale index entries would route cancels or modifies to non-resting memory.
        EXPECT_TRUE(queued_ids.contains(order_id)) << "index id missing from queues id="
                                                   << order_id;
    }
}

/**
 * @brief Verifies aggregate level volume against individual order quantities.
 */
void validate_price_level_volumes(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        Quantity total = 0;
        for (const auto& order : level.orders) {
            // Level totals are used by FOK preflight and must track queue updates.
            total += order.quantity;
        }
        EXPECT_EQ(level.total_volume, total) << "bid price=" << level.price;
    }

    for (const auto& level : snapshot.asks) {
        Quantity total = 0;
        for (const auto& order : level.orders) {
            // Ask level totals must shrink correctly after fills, cancels, and modifies.
            total += order.quantity;
        }
        EXPECT_EQ(level.total_volume, total) << "ask price=" << level.price;
    }
}

/**
 * @brief Verifies that no empty or zero-volume price levels remain.
 */
void validate_no_empty_price_levels(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        // Empty levels can corrupt best-price traversal and hide cleanup bugs.
        EXPECT_FALSE(level.orders.empty()) << "empty bid level price=" << level.price;
        EXPECT_GT(level.total_volume, 0U) << "zero-volume bid level price=" << level.price;
    }

    for (const auto& level : snapshot.asks) {
        // Empty ask levels should be erased as soon as their final order leaves.
        EXPECT_FALSE(level.orders.empty()) << "empty ask level price=" << level.price;
        EXPECT_GT(level.total_volume, 0U) << "zero-volume ask level price=" << level.price;
    }
}

/**
 * @brief Verifies live resting orders have valid side, price, and quantity metadata.
 */
void validate_resting_order_metadata(const OrderBook::DebugSnapshot& snapshot) {
    for (const auto& level : snapshot.bids) {
        for (const auto& order : level.orders) {
            // Bid queues should contain only positive-quantity buy orders at their level price.
            EXPECT_EQ(order.side, Side::Buy) << "bid level contains non-buy id=" << order.id;
            EXPECT_EQ(order.price, level.price) << "bid order stored at wrong price id="
                                                << order.id;
            EXPECT_GT(order.quantity, 0U) << "zero-quantity bid id=" << order.id;
        }
    }

    for (const auto& level : snapshot.asks) {
        for (const auto& order : level.orders) {
            // Ask queues should contain only positive-quantity sell orders at their level price.
            EXPECT_EQ(order.side, Side::Sell) << "ask level contains non-sell id=" << order.id;
            EXPECT_EQ(order.price, level.price) << "ask order stored at wrong price id="
                                                << order.id;
            EXPECT_GT(order.quantity, 0U) << "zero-quantity ask id=" << order.id;
        }
    }
}

/**
 * @brief Runs all long-run structural checks against a book snapshot.
 */
void validate_book_health(const OrderBook& book,
                          const std::vector<std::string>& operations,
                          std::size_t operation_index) {
    const auto snapshot = book.debug_snapshot();
    SCOPED_TRACE("operation_index=" + std::to_string(operation_index));
    SCOPED_TRACE(recent_operations(operations));

    // Each check targets a distinct corruption class that can appear after churn.
    validate_book_not_crossed(snapshot);
    validate_index_consistency(book, snapshot);
    validate_price_level_volumes(snapshot);
    validate_no_empty_price_levels(snapshot);
    validate_resting_order_metadata(snapshot);
}

/**
 * @brief Chooses one live id from a deterministic id list.
 */
[[nodiscard]] OrderId choose_live_id(const std::vector<OrderId>& live_ids, std::mt19937& rng) {
    std::uniform_int_distribution<std::size_t> distribution{0, live_ids.size() - 1};

    // Sorted ids keep the selection independent from hash-table iteration order.
    return live_ids[distribution(rng)];
}

/**
 * @brief Refreshes a live-id candidate cache from the book snapshot.
 */
void refresh_live_candidates(const OrderBook& book, std::vector<OrderId>& live_candidates) {
    // Full snapshots are reserved for cache refreshes and invariant checks, not every operation.
    live_candidates = sorted_live_order_ids(book.debug_snapshot());
}

/**
 * @brief Chooses a currently live id from a candidate cache.
 */
[[nodiscard]] std::optional<OrderId> choose_live_candidate(const OrderBook& book,
                                                           std::vector<OrderId>& live_candidates,
                                                           std::mt19937& rng) {
    constexpr int kFastAttempts = 16;

    for (int attempt = 0; attempt < kFastAttempts && !live_candidates.empty(); ++attempt) {
        const OrderId order_id = choose_live_id(live_candidates, rng);
        if (book.contains_order(order_id)) {
            // Stale candidates are expected after fills and cancels, so accept the first live hit.
            return order_id;
        }
    }

    refresh_live_candidates(book, live_candidates);
    if (live_candidates.empty()) {
        // Long-running taker flow can legitimately drain the book before replenishment.
        return std::nullopt;
    }

    // After refresh every candidate came from the live index.
    return choose_live_id(live_candidates, rng);
}

/**
 * @brief Runs a health check at fixed operation intervals and at the end.
 */
void maybe_validate_book_health(const OrderBook& book,
                                const std::vector<std::string>& operations,
                                std::size_t operation_index,
                                std::size_t operation_count) {
    if (operation_index % kValidationInterval != 0 && operation_index != operation_count) {
        return;
    }

    // Periodic snapshots catch long-run corruption without making every operation snapshot-heavy.
    validate_book_health(book, operations, operation_index);
}

/**
 * @brief Records one compact operation line in a bounded diagnostic ring.
 */
void record_operation(std::vector<std::string>& operations, std::string operation) {
    constexpr std::size_t kMaxRecordedOperations = 128;

    // Keep only recent operations so million-op failures stay readable.
    if (operations.size() == kMaxRecordedOperations) {
        operations.erase(operations.begin());
    }
    operations.push_back(std::move(operation));
}

/**
 * @brief Submits one passive replenishment order and tracks it if it rests.
 */
void submit_replenishment(OrderBook& book,
                          std::vector<Event>& events,
                          std::vector<OrderId>& live_candidates,
                          std::vector<std::string>& operations,
                          OrderId& next_id,
                          Side side) {
    const Price price = side == Side::Buy ? kMidPrice - 200 : kMidPrice + 200;

    // Passive prices rebuild resting liquidity without unexpectedly crossing the spread.
    book.submit(make_order(next_id, side, price, 10), events);
    if (book.contains_order(next_id)) {
        live_candidates.push_back(next_id);
    }
    record_operation(operations, "REPLENISH " + std::to_string(next_id) + " " + side_text(side));
    ++next_id;
}

/**
 * @brief Exercises a broad mixed order-flow stream for one million operations.
 */
void run_long_run_mixed(std::size_t operation_count) {
    OrderBook book{operation_count / 10};
    std::vector<Event> events;
    std::vector<std::string> operations;
    std::vector<OrderId> live_candidates;
    std::mt19937 rng{kMixedSeed};
    OrderId next_id = 1;

    events.reserve(32);
    operations.reserve(128);
    live_candidates.reserve(operation_count / 10);

    for (std::size_t index = 1; index <= operation_count; ++index) {
        const int selector = std::uniform_int_distribution<int>{0, 99}(rng);

        if (selector < 35 || live_candidates.empty()) {
            const Side side = selector % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price = kMidPrice + std::uniform_int_distribution<int>{-8, 8}(rng);
            const Quantity quantity = std::uniform_int_distribution<Quantity>{1, 20}(rng);

            // GTC flow keeps the book populated while random prices create natural crosses.
            book.submit(make_order(next_id, side, price, quantity), events);
            if (book.contains_order(next_id)) {
                // Only resting remainders enter the live candidate cache.
                live_candidates.push_back(next_id);
            }
            record_operation(operations,
                             "SUBMIT " + std::to_string(next_id) + " " + side_text(side));
            ++next_id;
        } else if (selector < 55) {
            const auto order_id = choose_live_candidate(book, live_candidates, rng);
            if (!order_id) {
                submit_replenishment(book, events, live_candidates, operations, next_id, Side::Buy);
                maybe_validate_book_health(book, operations, index, operation_count);
                continue;
            }

            // Cancels remove arbitrary live nodes from FIFO queues and the id index.
            static_cast<void>(book.cancel(*order_id));
            record_operation(operations, "CANCEL " + std::to_string(*order_id));
        } else if (selector < 75) {
            const auto order_id = choose_live_candidate(book, live_candidates, rng);
            if (!order_id) {
                submit_replenishment(book, events, live_candidates, operations, next_id, Side::Sell);
                maybe_validate_book_health(book, operations, index, operation_count);
                continue;
            }
            const Price price = kMidPrice + std::uniform_int_distribution<int>{-8, 8}(rng);
            const Quantity quantity = std::uniform_int_distribution<Quantity>{1, 20}(rng);

            // Modifies exercise in-place reductions and cancel-replace paths.
            book.modify(*order_id, price, quantity, events);
            record_operation(operations, "MODIFY " + std::to_string(*order_id));
        } else if (selector < 88) {
            const Side side = selector % 2 == 0 ? Side::Buy : Side::Sell;
            const Quantity quantity = std::uniform_int_distribution<Quantity>{1, 25}(rng);

            // Market flow consumes available liquidity without leaving remainders.
            book.submit_market(make_order(next_id, side, 0, quantity), events);
            record_operation(operations,
                             "MARKET " + std::to_string(next_id) + " " + side_text(side));
            ++next_id;
        } else {
            const Side side = selector % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price = kMidPrice + std::uniform_int_distribution<int>{-8, 8}(rng);
            const Quantity quantity = std::uniform_int_distribution<Quantity>{1, 20}(rng);
            const TimeInForce tif = selector % 3 == 0 ? TimeInForce::FillOrKill
                                                      : TimeInForce::ImmediateOrCancel;

            // IOC and FOK pressure transient matching and rejection paths.
            book.submit(make_order(next_id, side, price, quantity, tif), events);
            record_operation(operations, "SUBMIT_TRANSIENT " + std::to_string(next_id));
            ++next_id;
        }

        maybe_validate_book_health(book, operations, index, operation_count);
    }
}

/**
 * @brief Exercises repeated creation and deletion of many price levels.
 */
void run_long_run_level_churn(std::size_t operation_count) {
    OrderBook book{4096};
    std::vector<Event> events;
    std::vector<std::string> operations;
    OrderId next_id = 1;

    events.reserve(8);
    operations.reserve(128);

    for (std::size_t index = 1; index <= operation_count; index += 2) {
        const Price price = kMidPrice + static_cast<Price>((index / 2) % 4096);

        // Insert a same-price pair that should create, cross, and erase a level quickly.
        book.submit(make_order(next_id, Side::Sell, price, 1), events);
        record_operation(operations, "LEVEL_SELL " + std::to_string(next_id));
        ++next_id;

        book.submit(make_order(next_id, Side::Buy, price, 1), events);
        record_operation(operations, "LEVEL_BUY_CROSS " + std::to_string(next_id));
        ++next_id;

        maybe_validate_book_health(book, operations, std::min(index + 1, operation_count),
                                   operation_count);
    }
}

/**
 * @brief Exercises heavy modify and cancel churn over a bounded live set.
 */
void run_long_run_modify_cancel_churn(std::size_t operation_count) {
    constexpr std::size_t kInitialLiveOrders = 4096;
    OrderBook book{kInitialLiveOrders * 2};
    std::vector<Event> events;
    std::vector<std::string> operations;
    std::vector<OrderId> live_candidates;
    std::mt19937 rng{0xCACE1U};
    OrderId next_id = 1;

    events.reserve(16);
    operations.reserve(128);
    live_candidates.reserve(kInitialLiveOrders * 2);

    for (std::size_t index = 0; index < kInitialLiveOrders; ++index) {
        const Side side = index % 2 == 0 ? Side::Buy : Side::Sell;
        const Price price = side == Side::Buy ? kMidPrice - 100 - static_cast<Price>(index % 16)
                                              : kMidPrice + 100 + static_cast<Price>(index % 16);

        // Seed non-crossing liquidity so subsequent churn targets existing resting orders.
        book.submit(make_order(next_id, side, price, 10), events);
        live_candidates.push_back(next_id);
        ++next_id;
    }

    validate_book_health(book, operations, 0);

    for (std::size_t index = 1; index <= operation_count; ++index) {
        if (index % 3 == 0) {
            const auto order_id = choose_live_candidate(book, live_candidates, rng);
            if (!order_id) {
                submit_replenishment(book, events, live_candidates, operations, next_id, Side::Buy);
                maybe_validate_book_health(book, operations, index, operation_count);
                continue;
            }

            // Cancel arbitrary nodes to stress removal from middle, front, and back positions.
            static_cast<void>(book.cancel(*order_id));
            record_operation(operations, "CHURN_CANCEL " + std::to_string(*order_id));
        } else {
            const auto order_id = choose_live_candidate(book, live_candidates, rng);
            if (!order_id) {
                submit_replenishment(book, events, live_candidates, operations, next_id, Side::Sell);
                maybe_validate_book_health(book, operations, index, operation_count);
                continue;
            }
            const Price price = kMidPrice + std::uniform_int_distribution<int>{-120, 120}(rng);
            const Quantity quantity = std::uniform_int_distribution<Quantity>{1, 30}(rng);

            // Modify churn repeatedly updates or replaces live ids under random prices.
            book.modify(*order_id, price, quantity, events);
            record_operation(operations, "CHURN_MODIFY " + std::to_string(*order_id));
        }

        if (index % 5 == 0) {
            const Side side = index % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price = side == Side::Buy ? kMidPrice - 150 : kMidPrice + 150;

            // Replenishment keeps the live set from collapsing into only unknown-id paths.
            book.submit(make_order(next_id, side, price, 10), events);
            if (book.contains_order(next_id)) {
                // Replenished orders are intentionally passive and should usually rest.
                live_candidates.push_back(next_id);
            }
            record_operation(operations, "CHURN_REPLENISH " + std::to_string(next_id));
            ++next_id;
        }

        maybe_validate_book_health(book, operations, index, operation_count);
    }
}

} // namespace

/**
 * @brief Soaks broad mixed order flow with periodic structural validation.
 */
TEST(OrderBookStressTest, LongRunMixedOneMillionMaintainsBookHealth) {
    // The mixed stream covers GTC, cancel, modify, market, IOC, and FOK behavior together.
    run_long_run_mixed(kOperationCount);
}

/**
 * @brief Soaks repeated price-level creation and cleanup.
 */
TEST(OrderBookStressTest, LongRunLevelChurnOneMillionMaintainsBookHealth) {
    // Level churn is tuned to expose missed empty-level erasure and aggregate-volume drift.
    run_long_run_level_churn(kOperationCount);
}

/**
 * @brief Soaks heavy modify and cancel traffic over live resting orders.
 */
TEST(OrderBookStressTest, LongRunModifyCancelChurnOneMillionMaintainsBookHealth) {
    // Modify/cancel churn targets stale index entries, FIFO link corruption, and pool reuse bugs.
    run_long_run_modify_cancel_churn(kOperationCount);
}
