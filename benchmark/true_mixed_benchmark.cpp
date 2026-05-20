#include "true_mixed_workload.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace {

using matching_engine::Event;
using matching_engine::OrderBook;
using matching_engine::benchmark_workloads::make_true_mixed_workload;
using matching_engine::benchmark_workloads::preload_true_mixed_book;
using matching_engine::benchmark_workloads::run_true_mixed_operation;

constexpr matching_engine::Price kLadderBasePrice =
    matching_engine::benchmark_workloads::kTrueMixedMidPrice;
constexpr matching_engine::Price kLadderRange = 64;

/**
 * @brief Price-level storage mode selected for a benchmark run.
 */
enum class StorageMode {
    Tree,
    Ladder
};

/**
 * @brief Constructs a tree or ladder book for side-by-side replay.
 *
 * @param book Optional storage receiving the constructed book.
 * @param mode Price-level storage mode under test.
 * @param reserve_order_capacity Live-order reserve hint for the run.
 */
void construct_book(std::optional<OrderBook>& book,
                    StorageMode mode,
                    std::size_t reserve_order_capacity) {
    if (mode == StorageMode::Ladder) {
        // The true mixed generator keeps passive and aggressive prices near the midpoint.
        book.emplace(reserve_order_capacity, kLadderBasePrice, kLadderRange);
        return;
    }

    // Preserve the original tree benchmark construction path.
    book.emplace();
    book->reserve_order_capacity(reserve_order_capacity);
}

/**
 * @brief Measures direct OrderBook throughput for randomly interleaved true mixed flow.
 *
 * The timed stream contains GTC limit submits, live cancels, live modifies, IOC
 * limit orders, market orders, and FOK limit orders. Parser, Exchange routing,
 * filesystem I/O, and event formatting are intentionally excluded.
 */
void run_order_book_true_mixed(benchmark::State& state, StorageMode mode) {
    const auto operation_count = static_cast<std::size_t>(state.range(0));
    const auto workload = make_true_mixed_workload(operation_count);
    std::optional<OrderBook> book;
    std::vector<Event> events;
    events.reserve(16);

    for (auto _ : state) {
        state.PauseTiming();
        construct_book(book, mode, workload.reserve_order_capacity);
        preload_true_mixed_book(*book, workload.preload_orders, events);
        state.ResumeTiming();

        for (const auto& operation : workload.operations) {
            auto cancel_result = run_true_mixed_operation(*book, operation, events);
            benchmark::DoNotOptimize(cancel_result);
            benchmark::DoNotOptimize(events.data());
            benchmark::DoNotOptimize(events.size());
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["preload_orders"] = static_cast<double>(workload.preload_orders.size());
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
    state.counters["ladder_range"] =
        mode == StorageMode::Ladder ? static_cast<double>(kLadderRange) : 0.0;
}

/**
 * @brief Measures the original tree-backed true mixed workload.
 */
void BM_OrderBookTrueMixed(benchmark::State& state) {
    run_order_book_true_mixed(state, StorageMode::Tree);
}

/**
 * @brief Measures the ladder-backed true mixed workload with the same action stream.
 */
void BM_OrderBookTrueMixedLadder(benchmark::State& state) {
    run_order_book_true_mixed(state, StorageMode::Ladder);
}

BENCHMARK(BM_OrderBookTrueMixed)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_OrderBookTrueMixedLadder)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
