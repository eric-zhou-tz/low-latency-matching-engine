#include "book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::Side;

constexpr std::int64_t kBestPassiveBid = 99;
constexpr std::int64_t kBestPassiveAsk = 101;
constexpr std::uint64_t kQuantity = 1;

[[nodiscard]] std::vector<Order> make_passive_orders(std::int64_t count) {
    // Preallocate so vector growth does not add noise to benchmark setup.
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    // Alternate sides at non-crossing prices so every order rests.
    for (std::int64_t index = 0; index < count; ++index) {
        const bool is_buy = index % 2 == 0;
        const auto price_offset = index % 5;
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = is_buy ? Side::Buy : Side::Sell,
                               .price = is_buy ? kBestPassiveBid - price_offset
                                               : kBestPassiveAsk + price_offset,
                               .quantity = kQuantity});
    }

    // Return the prepared passive workload.
    return orders;
}

/**
 * Measures pure resting limit-order insertion throughput.
 *
 * The generated buys are priced below the generated sells, so no order crosses.
 * This isolates the order-book path that accepts and appends orders to FIFO
 * price levels without parser, file, stdin, or logging overhead.
 */
void BM_RestingLimitOrderInsert(benchmark::State& state) {
    // Use the benchmark argument as the number of orders inserted per iteration.
    const auto order_count = state.range(0);
    const auto expected_order_capacity = static_cast<std::size_t>(order_count);
    const auto orders = make_passive_orders(order_count);
    std::optional<OrderBook> book;

    // Google Benchmark controls the number of repetitions.
    for (auto _ : state) {
        // Create a fresh book outside the timed insert section.
        state.PauseTiming();
        book.emplace(expected_order_capacity);
        state.ResumeTiming();

        // Measure only non-crossing submit and append behavior.
        for (const auto& order : orders) {
            auto events = book->submit(order);
            benchmark::DoNotOptimize(events);
        }

        // Keep the compiler from optimizing away book mutations.
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        // Destroy the populated book outside the measured section.
        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    // Report throughput in accepted resting orders.
    state.SetItemsProcessed(state.iterations() * order_count);
}

BENCHMARK(BM_RestingLimitOrderInsert)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
