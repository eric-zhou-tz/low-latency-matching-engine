#include "book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

/**
 * @brief Reads the benchmark order-id load factor tuning knob.
 *
 * @return Requested max load factor, or the benchmark baseline when unset.
 */
[[nodiscard]] float benchmark_order_id_max_load_factor() {
    const char* value = std::getenv("MATCHING_ENGINE_ORDER_ID_MAX_LOAD_FACTOR");
    if (value == nullptr) {
        return kDefaultOrderIdMaxLoadFactor;
    }

    const float load_factor = std::strtof(value, nullptr);
    if (load_factor <= 0.0F) {
        return kDefaultOrderIdMaxLoadFactor;
    }

    return load_factor;
}

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
    const auto expected_order_capacity = static_cast<std::size_t>(order_count);
    const auto order_id_max_load_factor = benchmark_order_id_max_load_factor();
    const auto resting_orders = make_same_price_resting_buys(order_count);
    std::optional<OrderBook> book;

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(expected_order_capacity, order_id_max_load_factor);
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
    state.counters["order_id_max_load_factor"] = order_id_max_load_factor;
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
[[nodiscard]] std::vector<MixedOperation> make_mixed_operations(std::int64_t operation_count) {
    std::vector<MixedOperation> operations;
    operations.reserve(static_cast<std::size_t>(operation_count));

    std::deque<std::uint64_t> cancelable_buy_ids;
    std::uint64_t next_resting_id = 1;
    std::uint64_t next_crossing_id = kMixedCrossingIdBase;

    for (std::int64_t index = 0; index < operation_count; ++index) {
        const auto slot = index % 10;

        if (slot < 7) {
            const bool make_buy = slot == 1 || slot == 3 || slot == 5 || slot == 6;
            const auto side = make_buy ? Side::Buy : Side::Sell;
            auto order = make_mixed_resting_order(next_resting_id++, side);

            if (side == Side::Buy) {
                cancelable_buy_ids.push_back(order.id);
            }

            operations.push_back(MixedOperation{.kind = MixedOperationKind::RestingInsert,
                                                .order = order,
                                                .cancel_id = 0});
        } else if (slot < 9) {
            const auto order_id = cancelable_buy_ids.front();
            cancelable_buy_ids.pop_front();
            operations.push_back(MixedOperation{.kind = MixedOperationKind::Cancel,
                                                .order = {},
                                                .cancel_id = order_id});
        } else {
            operations.push_back(MixedOperation{.kind = MixedOperationKind::CrossingOrder,
                                                .order = make_mixed_crossing_buy(next_crossing_id++),
                                                .cancel_id = 0});
        }
    }

    return operations;
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
    const auto expected_order_capacity = static_cast<std::size_t>(operation_count);
    const auto order_id_max_load_factor = benchmark_order_id_max_load_factor();
    const auto operations = make_mixed_operations(operation_count);
    std::optional<OrderBook> book;
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(expected_order_capacity, order_id_max_load_factor);
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
    state.counters["order_id_max_load_factor"] = order_id_max_load_factor;
}

BENCHMARK(BM_CancelFront)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelBack)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelRandom)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CancelUnknown)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_MixedSubmitCancel)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
