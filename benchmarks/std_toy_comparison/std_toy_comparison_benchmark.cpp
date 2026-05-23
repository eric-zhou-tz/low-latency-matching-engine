#include "benchmarks/realistic_flow/true_mixed_workload.hpp"
#include "book/order_book.hpp"
#include "toy/order_book.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <type_traits>
#include <vector>

namespace {

using matching_engine::Event;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::OrderId;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::Side;

constexpr Price kBestPassiveBid = 99;
constexpr Price kBestPassiveAsk = 101;
constexpr Price kRestingBid = 100;
constexpr Price kRestingAsk = 102;
constexpr Price kCrossingBuy = 105;
constexpr Quantity kQuantity = 1;
constexpr Quantity kModifiableQuantity = 2;
constexpr OrderId kIncomingIdBase = 1'000'000'000;
constexpr OrderId kUnknownOrderIdBase = 2'000'000'000;
constexpr std::uint32_t kCancelShuffleSeed = 0xC0FFEE;

/**
 * @brief Builds passive orders that never cross.
 *
 * @param count Number of orders to generate.
 * @return Deterministic non-crossing order stream.
 */
[[nodiscard]] std::vector<Order> make_passive_orders(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // Alternate sides while keeping prices outside the spread.
        const bool is_buy = index % 2 == 0;
        const auto price_offset = static_cast<Price>(index % 5);
        orders.push_back(Order{.id = static_cast<OrderId>(index + 1),
                               .side = is_buy ? Side::Buy : Side::Sell,
                               .price = is_buy ? kBestPassiveBid - price_offset
                                               : kBestPassiveAsk + price_offset,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price asks used as one-level resting liquidity.
 *
 * @param count Number of asks to generate.
 * @return Passive ask orders with stable ids.
 */
[[nodiscard]] std::vector<Order> make_resting_asks(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // All asks share one price level so the match path isolates FIFO removal.
        orders.push_back(Order{.id = static_cast<OrderId>(index + 1),
                               .side = Side::Sell,
                               .price = kRestingAsk,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds aggressive buys that cross one resting ask each.
 *
 * @param count Number of buys to generate.
 * @return Crossing buy limit orders with non-conflicting ids.
 */
[[nodiscard]] std::vector<Order> make_crossing_buys(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // Use a separate id range so incoming taker flow cannot collide with resting liquidity.
        orders.push_back(Order{.id = kIncomingIdBase + static_cast<OrderId>(index),
                               .side = Side::Buy,
                               .price = kCrossingBuy,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price resting buys for cancel benchmarks.
 *
 * @param count Number of buys to generate.
 * @return Passive buy orders with stable ids.
 */
[[nodiscard]] std::vector<Order> make_same_price_resting_buys(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // One price level makes the baseline scan cost easy to compare.
        orders.push_back(Order{.id = static_cast<OrderId>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price buys whose quantity can be reduced in place.
 *
 * @param count Number of buys to generate.
 * @return Passive buy orders with modifiable quantity.
 */
[[nodiscard]] std::vector<Order> make_same_price_modifiable_buys(std::int64_t count) {
    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // Quantity starts above the replacement size so modify preserves the order id.
        orders.push_back(Order{.id = static_cast<OrderId>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kModifiableQuantity});
    }

    return orders;
}

/**
 * @brief Builds ids that visit live orders in FIFO order.
 *
 * @param count Number of ids to generate.
 * @return Ascending live order ids.
 */
[[nodiscard]] std::vector<OrderId> make_front_ids(std::int64_t count) {
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // Match generated order ids exactly.
        ids.push_back(static_cast<OrderId>(index + 1));
    }

    return ids;
}

/**
 * @brief Builds ids that cancel live orders in a fixed shuffled order.
 *
 * @param count Number of ids to generate.
 * @return Reproducibly shuffled live order ids.
 */
[[nodiscard]] std::vector<OrderId> make_random_cancel_ids(std::int64_t count) {
    auto ids = make_front_ids(count);
    std::mt19937 rng{kCancelShuffleSeed};

    // Shuffle once outside timing so the measured loop is only cancel work.
    std::ranges::shuffle(ids, rng);

    return ids;
}

/**
 * @brief Builds ids that are guaranteed to miss the live order index.
 *
 * @param count Number of ids to generate.
 * @return Deterministic unknown order ids.
 */
[[nodiscard]] std::vector<OrderId> make_unknown_cancel_ids(std::int64_t count) {
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        // Place misses in a separate range from every generated live order.
        ids.push_back(kUnknownOrderIdBase + static_cast<OrderId>(index));
    }

    return ids;
}

/**
 * @brief Constructs a book and applies reserve hints when supported.
 *
 * @tparam Book Book implementation under test.
 * @param reserve_order_capacity Expected peak live order count.
 * @return Empty book ready for benchmark setup.
 */
template <typename Book>
[[nodiscard]] Book make_book(std::size_t reserve_order_capacity) {
    if constexpr (std::is_same_v<Book, OrderBook>) {
        // The optimized book can size pooled storage and the id map before timing.
        return Book{reserve_order_capacity};
    } else {
        // The std toy book intentionally has no reserve API.
        static_cast<void>(reserve_order_capacity);
        return Book{};
    }
}

/**
 * @brief Inserts setup liquidity before timing begins.
 *
 * @tparam Book Book implementation under test.
 * @param book Book to populate.
 * @param orders Orders to submit as passive liquidity.
 */
template <typename Book>
void preload_book(Book& book, const std::vector<Order>& orders) {
    std::vector<Event> events;
    events.reserve(8);

    for (const auto& order : orders) {
        // Preload creates the intended book shape outside the measured loop.
        book.submit(order, events);
        benchmark::DoNotOptimize(events.data());
        benchmark::DoNotOptimize(events.size());
    }
}

/**
 * @brief Measures passive insert throughput for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_passive_insert(benchmark::State& state) {
    const auto order_count = state.range(0);
    const auto orders = make_passive_orders(order_count);
    std::optional<Book> book;
    std::vector<Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(make_book<Book>(static_cast<std::size_t>(order_count)));
        state.ResumeTiming();

        for (const auto& order : orders) {
            // Timed work appends a non-crossing order and updates live book state.
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

/**
 * @brief Measures one-level crossing match throughput for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_one_level_crossing_match(benchmark::State& state) {
    const auto order_count = state.range(0);
    const auto resting_orders = make_resting_asks(order_count);
    const auto crossing_orders = make_crossing_buys(order_count);
    std::optional<Book> book;
    std::vector<Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(make_book<Book>(static_cast<std::size_t>(order_count)));
        preload_book(*book, resting_orders);
        state.ResumeTiming();

        for (const auto& order : crossing_orders) {
            // Timed work consumes exactly one resting FIFO order.
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

/**
 * @brief Measures a prepared cancel sequence for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 * @param cancel_ids Cancel ids to execute in the timed loop.
 */
template <typename Book>
void run_cancel_sequence(benchmark::State& state, const std::vector<OrderId>& cancel_ids) {
    const auto order_count = state.range(0);
    const auto resting_orders = make_same_price_resting_buys(order_count);
    std::optional<Book> book;

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(make_book<Book>(static_cast<std::size_t>(order_count)));
        preload_book(*book, resting_orders);
        state.ResumeTiming();

        for (const auto order_id : cancel_ids) {
            // Timed work either unlinks a live id or rejects a known miss.
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
}

/**
 * @brief Measures shuffled live cancel throughput for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_random_cancel(benchmark::State& state) {
    const auto cancel_ids = make_random_cancel_ids(state.range(0));

    // Share the deterministic id stream between implementations.
    run_cancel_sequence<Book>(state, cancel_ids);
}

/**
 * @brief Measures unknown cancel throughput for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_unknown_cancel(benchmark::State& state) {
    const auto cancel_ids = make_unknown_cancel_ids(state.range(0));

    // Share the deterministic miss stream between implementations.
    run_cancel_sequence<Book>(state, cancel_ids);
}

/**
 * @brief Measures same-price modify throughput for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_modify_if_present(benchmark::State& state) {
    const auto order_count = state.range(0);
    const auto resting_orders = make_same_price_modifiable_buys(order_count);
    const auto target_ids = make_front_ids(order_count);
    std::optional<Book> book;
    std::vector<Event> events;
    events.reserve(4);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(make_book<Book>(static_cast<std::size_t>(order_count)));
        preload_book(*book, resting_orders);
        state.ResumeTiming();

        for (const auto order_id : target_ids) {
            // Timed work finds the live order and reduces quantity without changing price.
            book->modify(order_id, kRestingBid, kQuantity, events);
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

/**
 * @brief Preloads true-mixed liquidity before timing begins.
 *
 * @tparam Book Book implementation under test.
 * @param book Book to populate.
 * @param preload_orders Deterministic resting orders.
 * @param events Reusable caller-owned event buffer.
 */
template <typename Book>
void preload_true_mixed_book(Book& book,
                             const std::vector<Order>& preload_orders,
                             std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload is setup work so the timed stream starts from the modeled live depth.
        book.submit(order, events);
        benchmark::DoNotOptimize(events.data());
        benchmark::DoNotOptimize(events.size());
    }
}

/**
 * @brief Runs one true-mixed operation against one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param book Book being mutated.
 * @param operation Pre-generated mixed operation.
 * @param events Reusable caller-owned event buffer.
 */
template <typename Book>
void run_one_true_mixed_comparison_operation(
    Book& book,
    const matching_engine::benchmark_workloads::TrueMixedOperation& operation,
    std::vector<Event>& events) {
    using matching_engine::benchmark_workloads::TrueMixedOperationKind;

    if (operation.kind == TrueMixedOperationKind::Cancel) {
        // Cancel uses the compact result API on both implementations.
        auto result = book.cancel(operation.target_order_id);
        benchmark::DoNotOptimize(result);
        return;
    }

    if (operation.kind == TrueMixedOperationKind::Modify) {
        // Modify updates a live order selected by the deterministic workload model.
        book.modify(operation.target_order_id, operation.new_price, operation.new_quantity, events);
    } else if (operation.kind == TrueMixedOperationKind::Market) {
        // Market orders execute as transient taker flow and never rest leftovers.
        book.submit_market(operation.order, events);
    } else {
        // GTC, IOC, and FOK limit orders share the submit path.
        book.submit(operation.order, events);
    }

    benchmark::DoNotOptimize(events.data());
    benchmark::DoNotOptimize(events.size());
}

/**
 * @brief Measures deterministic true-mixed flow for one book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param state Google Benchmark state.
 */
template <typename Book>
void run_true_mixed(benchmark::State& state) {
    const auto operation_count = static_cast<std::size_t>(state.range(0));
    const auto workload =
        matching_engine::benchmark_workloads::make_true_mixed_workload(operation_count);
    std::optional<Book> book;
    std::vector<Event> events;
    events.reserve(16);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace(make_book<Book>(workload.reserve_order_capacity));
        preload_true_mixed_book(*book, workload.preload_orders, events);
        state.ResumeTiming();

        for (const auto& operation : workload.operations) {
            // Timed work applies the same interleaved exchange-style stream to each book.
            run_one_true_mixed_comparison_operation(*book, operation, events);
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
}

} // namespace

BENCHMARK_TEMPLATE(run_passive_insert, OrderBook)
    ->Name("BM_Compare_Optimized_PassiveInsert_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_passive_insert, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_PassiveInsert_Throughput")
    ->Arg(10'000);

BENCHMARK_TEMPLATE(run_one_level_crossing_match, OrderBook)
    ->Name("BM_Compare_Optimized_OneLevelCrossingMatch_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_one_level_crossing_match, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_OneLevelCrossingMatch_Throughput")
    ->Arg(10'000);

BENCHMARK_TEMPLATE(run_random_cancel, OrderBook)
    ->Name("BM_Compare_Optimized_RandomCancel_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_random_cancel, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_RandomCancel_Throughput")
    ->Arg(10'000);

BENCHMARK_TEMPLATE(run_unknown_cancel, OrderBook)
    ->Name("BM_Compare_Optimized_UnknownCancel_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_unknown_cancel, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_UnknownCancel_Throughput")
    ->Arg(10'000);

BENCHMARK_TEMPLATE(run_modify_if_present, OrderBook)
    ->Name("BM_Compare_Optimized_ModifyIfPresent_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_modify_if_present, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_ModifyIfPresent_Throughput")
    ->Arg(10'000);

BENCHMARK_TEMPLATE(run_true_mixed, OrderBook)
    ->Name("BM_Compare_Optimized_OrderBookTrueMixed_Throughput")
    ->Arg(10'000);
BENCHMARK_TEMPLATE(run_true_mixed, matching_engine::toy::OrderBook)
    ->Name("BM_Compare_StdToy_OrderBookTrueMixed_Throughput")
    ->Arg(10'000);
