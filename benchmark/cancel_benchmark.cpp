#include "book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace {

using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::Side;

constexpr std::int64_t kRestingBid = 100;
constexpr std::int64_t kRestingAsk = 102;
constexpr std::int64_t kCrossingBuy = 105;
constexpr std::uint64_t kQuantity = 1;
constexpr std::uint64_t kUnknownOrderIdBase = 1'000'000'000;
constexpr std::uint64_t kMixedCrossingIdBase = 2'000'000'000;
constexpr std::uint32_t kCancelShuffleSeed = 0xC0FFEE;
constexpr float kDefaultOrderIdMaxLoadFactor = 0.80F;

enum class MixedOperationKind {
    RestingInsert,
    Cancel,
    CrossingOrder
};

struct MixedOperation {
    MixedOperationKind kind{};
    Order order{};
    std::uint64_t cancel_id{};
};

struct MixedWorkload {
    std::vector<MixedOperation> operations;
    std::size_t max_live_orders{};
};

/**
 * @brief Builds one same-price FIFO queue of resting buy orders.
 *
 * @param count Number of orders to generate.
 * @return Passive buy orders with stable ids and equal price.
 */
[[nodiscard]] std::vector<Order> make_same_price_resting_buys(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds cancel ids that remove the oldest orders first.
 *
 * @param count Number of cancel ids to generate.
 * @return Ascending ids matching the FIFO front of the price level.
 */
[[nodiscard]] std::vector<std::uint64_t> make_front_cancel_ids(std::int64_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        ids.push_back(static_cast<std::uint64_t>(index + 1));
    }

    return ids;
}

/**
 * @brief Builds cancel ids that remove the newest orders first.
 *
 * @param count Number of cancel ids to generate.
 * @return Descending ids matching the FIFO back of the price level.
 */
[[nodiscard]] std::vector<std::uint64_t> make_back_cancel_ids(std::int64_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = count; index > 0; --index) {
        ids.push_back(static_cast<std::uint64_t>(index));
    }

    return ids;
}

/**
 * @brief Builds cancel ids in a reproducibly shuffled order.
 *
 * @param count Number of cancel ids to generate.
 * @return Shuffled live order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_random_cancel_ids(std::int64_t count) {
    auto ids = make_front_cancel_ids(count);

    std::mt19937 rng{kCancelShuffleSeed};
    std::ranges::shuffle(ids, rng);

    return ids;
}

/**
 * @brief Builds cancel ids that are guaranteed not to exist in the book.
 *
 * @param count Number of unknown ids to generate.
 * @return Non-live order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_unknown_cancel_ids(std::int64_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        ids.push_back(kUnknownOrderIdBase + static_cast<std::uint64_t>(index));
    }

    return ids;
}

/**
 * @brief Inserts resting orders before the timed cancel path.
 *
 * @param book Book to populate.
 * @param resting_orders Orders to submit as passive liquidity.
 */
void preload_book(OrderBook& book, const std::vector<Order>& resting_orders) {
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (const auto& order : resting_orders) {
        book.submit(order, events);
        benchmark::DoNotOptimize(events.data());
        benchmark::DoNotOptimize(events.size());
    }
}

/**
 * @brief Measures a prepared cancel sequence against a preloaded book.
 *
 * @param state Google Benchmark state.
 * @param cancel_ids Cancel ids to run inside the measured section.
 */
void run_cancel_workload(benchmark::State& state, const std::vector<std::uint64_t>& cancel_ids) {
    const auto order_count = state.range(0);
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Cancel-only workloads preload one live order for
    // each cancel operation.
    const auto reserve_order_capacity = static_cast<std::size_t>(order_count);
    const auto resting_orders = make_same_price_resting_buys(order_count);
    std::optional<OrderBook> book;

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(reserve_order_capacity);
        preload_book(*book, resting_orders);
        state.ResumeTiming();

        for (const auto order_id : cancel_ids) {
            auto result = book->cancel(order_id);
            benchmark::DoNotOptimize(result);
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * order_count);
    state.counters["order_id_max_load_factor"] = kDefaultOrderIdMaxLoadFactor;
}

/**
 * @brief Creates a passive order for the mixed exchange-style stream.
 *
 * @param order_id Stable id assigned to the resting insert.
 * @param side Side to place on the book.
 * @return Resting limit order.
 */
[[nodiscard]] Order make_mixed_resting_order(std::uint64_t order_id, Side side) {
    const auto price = side == Side::Buy ? kRestingBid : kRestingAsk;
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = kQuantity};
}

/**
 * @brief Creates an aggressive buy that consumes one resting ask.
 *
 * @param order_id Stable id assigned to the crossing order.
 * @return Crossing buy limit order.
 */
[[nodiscard]] Order make_mixed_crossing_buy(std::uint64_t order_id) {
    return Order{.id = order_id,
                 .side = Side::Buy,
                 .price = kCrossingBuy,
                 .quantity = kQuantity};
}

/**
 * @brief Builds a deterministic 70/20/10 insert/cancel/match workload.
 *
 * @param operation_count Number of operations to generate.
 * @return Mixed operation stream for one benchmark iteration.
 */
[[nodiscard]] MixedWorkload make_mixed_workload(std::int64_t operation_count) {
    // Build both the deterministic stream and its peak live depth in one pass.
    MixedWorkload workload;
    workload.operations.reserve(static_cast<std::size_t>(operation_count));

    // Track cancelable buys because crossing buys only consume passive asks.
    std::deque<std::uint64_t> cancelable_buy_ids;
    std::uint64_t next_resting_id = 1;
    std::uint64_t next_crossing_id = kMixedCrossingIdBase;
    std::size_t live_orders = 0;

    for (std::int64_t index = 0; index < operation_count; ++index) {
        const auto slot = index % 10;

        if (slot < 7) {
            // Seven slots per cycle add passive liquidity without crossing.
            const bool make_buy = slot == 1 || slot == 3 || slot == 5 || slot == 6;
            const auto side = make_buy ? Side::Buy : Side::Sell;
            auto order = make_mixed_resting_order(next_resting_id++, side);

            // Only buys are canceled later, so crossing orders cannot consume cancel targets.
            if (side == Side::Buy) {
                cancelable_buy_ids.push_back(order.id);
            }

            workload.operations.push_back(MixedOperation{.kind = MixedOperationKind::RestingInsert,
                                                         .order = order,
                                                         .cancel_id = 0});
            ++live_orders;
        } else if (slot < 9) {
            // Two slots per cycle remove known-live buy ids from the book.
            const auto order_id = cancelable_buy_ids.front();
            cancelable_buy_ids.pop_front();
            workload.operations.push_back(MixedOperation{.kind = MixedOperationKind::Cancel,
                                                         .order = {},
                                                         .cancel_id = order_id});
            --live_orders;
        } else {
            // One slot per cycle consumes one passive ask through the matching path.
            workload.operations.push_back(
                MixedOperation{.kind = MixedOperationKind::CrossingOrder,
                               .order = make_mixed_crossing_buy(next_crossing_id++),
                               .cancel_id = 0});
            --live_orders;
        }

        // Peak live depth lets the reserve sweep compare capacity against actual demand.
        workload.max_live_orders = std::max(workload.max_live_orders, live_orders);
    }

    return workload;
}

/**
 * @brief Builds only the mixed operations for benchmarks that do not need metadata.
 *
 * @param operation_count Number of operations to generate.
 * @return Mixed operation stream for one benchmark iteration.
 */
[[nodiscard]] std::vector<MixedOperation> make_mixed_operations(std::int64_t operation_count) {
    // Preserve the original helper surface for the baseline mixed benchmark.
    return make_mixed_workload(operation_count).operations;
}

/**
 * @brief Constructs a book for reserve-sweep experiments.
 *
 * @param reserve_capacity Explicit reserve size; zero means no reserve call.
 * @return Empty book configured for the benchmark trial.
 */
[[nodiscard]] OrderBook make_mixed_sweep_book(std::size_t reserve_capacity) {
    // Construct manually so reserve_capacity zero really means no explicit reserve call.
    OrderBook book;

    // Nonzero sweep points reserve both the hash index and order pool before timing.
    if (reserve_capacity > 0) {
        book.reserve_order_capacity(reserve_capacity);
    }

    return book;
}

/**
 * @brief Runs the mixed workload with a caller-selected reserve capacity.
 *
 * @param state Google Benchmark state.
 * @param reserve_capacity Explicit reserve size; zero means no reserve call.
 */
void run_mixed_submit_cancel_with_reserve(benchmark::State& state,
                                          std::size_t reserve_capacity) {
    // Generate deterministic input and metadata outside the timed benchmark loop.
    const auto operation_count = state.range(0);
    const auto workload = make_mixed_workload(operation_count);
    std::optional<OrderBook> book;
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (auto _ : state) {
        // Keep reserve allocation and book construction out of the measured path.
        state.PauseTiming();
        book.emplace(make_mixed_sweep_book(reserve_capacity));
        state.ResumeTiming();

        // Measure the same mixed stream while changing only setup-time reserve capacity.
        for (const auto& operation : workload.operations) {
            if (operation.kind == MixedOperationKind::Cancel) {
                auto result = book->cancel(operation.cancel_id);
                benchmark::DoNotOptimize(result);
            } else {
                book->submit(operation.order, events);
                benchmark::DoNotOptimize(events.data());
                benchmark::DoNotOptimize(events.size());
            }
        }

        // Keep book mutations and event writes observable to the optimizer.
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        // Destroy the remaining book state outside the measured section.
        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    // Expose sweep metadata directly in Google Benchmark output and JSON artifacts.
    state.SetItemsProcessed(state.iterations() * operation_count);
    state.counters["order_id_max_load_factor"] = kDefaultOrderIdMaxLoadFactor;
    state.counters["reserve_capacity"] = static_cast<double>(reserve_capacity);
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
}

/**
 * @brief Measures cancellation of the oldest order at one FIFO price level.
 */
void BM_CancelFront(benchmark::State& state) {
    const auto cancel_ids = make_front_cancel_ids(state.range(0));
    run_cancel_workload(state, cancel_ids);
}

/**
 * @brief Measures cancellation of the newest order at one FIFO price level.
 */
void BM_CancelBack(benchmark::State& state) {
    const auto cancel_ids = make_back_cancel_ids(state.range(0));
    run_cancel_workload(state, cancel_ids);
}

/**
 * @brief Measures cancellation through a deterministic random id order.
 */
void BM_CancelRandom(benchmark::State& state) {
    const auto cancel_ids = make_random_cancel_ids(state.range(0));
    run_cancel_workload(state, cancel_ids);
}

/**
 * @brief Measures rejected cancels that miss the order-id index.
 */
void BM_CancelUnknown(benchmark::State& state) {
    const auto cancel_ids = make_unknown_cancel_ids(state.range(0));
    run_cancel_workload(state, cancel_ids);
}

/**
 * @brief Measures a realistic stream of inserts, cancels, and matches.
 */
void BM_MixedSubmitCancel(benchmark::State& state) {
    const auto operation_count = state.range(0);
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Submit/cancel/modify-heavy workloads usually have
    // far fewer concurrent live orders than total messages, so mixed and
    // end-to-end use 10% as the current benchmark-tuned proxy.
    const auto reserve_order_capacity =
        std::max<std::size_t>(1024, static_cast<std::size_t>(operation_count) / 10);
    const auto operations = make_mixed_operations(operation_count);
    std::optional<OrderBook> book;
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        // Apply the mixed-workload reserve proxy during setup so timing covers
        // operation processing rather than allocation policy.
        book.emplace(reserve_order_capacity);
        state.ResumeTiming();

        for (const auto& operation : operations) {
            if (operation.kind == MixedOperationKind::Cancel) {
                auto result = book->cancel(operation.cancel_id);
                benchmark::DoNotOptimize(result);
            } else {
                book->submit(operation.order, events);
                benchmark::DoNotOptimize(events.data());
                benchmark::DoNotOptimize(events.size());
            }
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * operation_count);
    state.counters["order_id_max_load_factor"] = kDefaultOrderIdMaxLoadFactor;
}

/**
 * @brief Sweeps reserve capacity for the mixed workload without changing engine defaults.
 */
void BM_MixedSubmitCancelReserveSweep(benchmark::State& state) {
    // The second argument is the explicit reserve capacity under investigation.
    run_mixed_submit_cancel_with_reserve(state, static_cast<std::size_t>(state.range(1)));
}

BENCHMARK(BM_CancelFront)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelBack)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelRandom)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelUnknown)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_MixedSubmitCancel)->Arg(1'000)->Arg(10'000)->Arg(100'000);

#if defined(MATCHING_ENGINE_ENABLE_RESERVE_SWEEP)
BENCHMARK(BM_MixedSubmitCancelReserveSweep)
    ->Args({100'000, 0})
    ->Args({100'000, 1'000})
    ->Args({100'000, 8'192})
    ->Args({100'000, 10'000})
    ->Args({100'000, 16'384})
    ->Args({100'000, 32'768})
    ->Args({100'000, 40'000})
    ->Args({100'000, 65'536})
    ->Args({100'000, 100'000})
    ->Args({100'000, 1'000'000})
    ->Args({100'000, 10'000'000});
#endif

} // namespace
