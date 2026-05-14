#include "order_book.hpp"

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
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        const bool is_buy = index % 2 == 0;
        const auto price_offset = index % 5;
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .symbol = "AAPL",
                               .side = is_buy ? Side::Buy : Side::Sell,
                               .price = is_buy ? kBestPassiveBid - price_offset
                                               : kBestPassiveAsk + price_offset,
                               .quantity = kQuantity});
    }

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
    const auto order_count = state.range(0);
    const auto orders = make_passive_orders(order_count);
    std::optional<OrderBook> book;

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace();
        state.ResumeTiming();

        for (const auto& order : orders) {
            auto events = book->submit(order);
            benchmark::DoNotOptimize(events);
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * order_count);
}

BENCHMARK(BM_RestingLimitOrderInsert)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
