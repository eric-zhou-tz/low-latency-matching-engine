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

constexpr std::int64_t kRestingAsk = 100;
constexpr std::int64_t kAggressiveBuy = 105;
constexpr std::uint64_t kQuantity = 1;
constexpr std::uint64_t kIncomingIdBase = 1'000'000'000;

[[nodiscard]] std::vector<Order> make_resting_asks(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Sell,
                               .price = kRestingAsk,
                               .quantity = kQuantity});
    }

    return orders;
}

[[nodiscard]] std::vector<Order> make_crossing_buys(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = kIncomingIdBase + static_cast<std::uint64_t>(index),
                               .side = Side::Buy,
                               .price = kAggressiveBuy,
                               .quantity = kQuantity});
    }

    return orders;
}

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
 * Measures crossing limit-order matching throughput.
 *
 * The book is preloaded with sell-side liquidity, then aggressive buy limits
 * consume it one order at a time. Preload timing is excluded because this
 * benchmark is about the matching hot path, not the cost of constructing the
 * scenario. That is intentionally different from the resting insert workload,
 * which measures orders that do not trade and instead remain on the book.
 */
void BM_OrderBook_OneLevelCrossingMatch_Throughput(benchmark::State& state) {
    const auto order_count = state.range(0);
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. This match-only benchmark has one resting order per
    // incoming crossing order, so order_count is the resting-order count.
    const auto reserve_order_capacity = static_cast<std::size_t>(order_count);
    const auto resting_orders = make_resting_asks(order_count);
    const auto crossing_orders = make_crossing_buys(order_count);
    std::optional<OrderBook> book;
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(reserve_order_capacity);
        preload_book(*book, resting_orders);
        state.ResumeTiming();

        for (const auto& order : crossing_orders) {
            book->submit(order, events);
            benchmark::DoNotOptimize(events.data());
            benchmark::DoNotOptimize(events.size());
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * order_count);
}

BENCHMARK(BM_OrderBook_OneLevelCrossingMatch_Throughput)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Arg(100'000);

} // namespace
