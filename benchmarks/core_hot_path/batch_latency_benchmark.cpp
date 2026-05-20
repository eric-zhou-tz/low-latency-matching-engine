#include "book/order_book.hpp"
#include "benchmarks/realistic_flow/true_mixed_workload.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::Side;
using matching_engine::benchmark_workloads::make_true_mixed_workload;
using matching_engine::benchmark_workloads::preload_true_mixed_book;
using matching_engine::benchmark_workloads::run_true_mixed_operation;

constexpr std::int64_t kBestPassiveBid = 99;
constexpr std::int64_t kBestPassiveAsk = 101;
constexpr std::int64_t kRestingBid = 100;
constexpr std::int64_t kRestingAsk = 102;
constexpr std::int64_t kCrossingBuy = 105;
constexpr std::uint64_t kQuantity = 1;
constexpr std::uint64_t kModifiableQuantity = 2;
constexpr std::uint64_t kIncomingIdBase = 1'000'000'000;
constexpr std::uint64_t kUnknownOrderIdBase = 2'000'000'000;
constexpr std::uint64_t kMixedCrossingIdBase = 3'000'000'000;
constexpr std::uint32_t kCancelShuffleSeed = 0xC0FFEE;
constexpr std::size_t kDefaultSampleCount = 1'024;
constexpr std::size_t kDefaultWarmupBatches = 128;
constexpr std::size_t kDefaultTrialCount = 1;

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

struct Options {
    std::filesystem::path output_dir{"benchmarks/results"};
    std::size_t sample_count{kDefaultSampleCount};
    std::size_t warmup_batches{kDefaultWarmupBatches};
    std::size_t trial_count{kDefaultTrialCount};
};

struct Percentiles {
    double p50{};
    double p95{};
    double p99{};
    double max{};
};

struct LatencyResult {
    std::string benchmark_name;
    std::size_t workload_size{};
    std::size_t batch_size{};
    std::size_t sample_count{};
    std::size_t trial{};
    Percentiles percentiles{};
};

/**
 * @brief Prevents the optimizer from discarding benchmarked values.
 *
 * @tparam T Value type to keep observable to the compiler.
 * @param value Value produced by the measured workload.
 */
template <typename T>
void do_not_optimize(const T& value) {
    // compiler barriers keep measured hot-path work observable without adding runtime calls.
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Prevents memory operations from being moved across a measurement edge.
 */
void clobber_memory() {
    // force writes from the measured batch to remain before the ending timestamp.
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Reads a positive integer command-line option value.
 *
 * @param value Text following an option prefix.
 * @param fallback Value to keep when parsing fails or yields zero.
 * @return Parsed positive value or the fallback.
 */
[[nodiscard]] std::size_t parse_positive_size(std::string_view value, std::size_t fallback) {
    char* end = nullptr;
    const std::string text{value};
    const auto parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed == 0) {
        return fallback;
    }

    return static_cast<std::size_t>(parsed);
}

/**
 * @brief Parses simple --name=value options for the latency runner.
 *
 * @param argc Argument count from main.
 * @param argv Argument values from main.
 * @return Runtime options for output and sample counts.
 */
[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        if (arg.starts_with("--output-dir=")) {
            options.output_dir = std::string(arg.substr(std::string_view{"--output-dir="}.size()));
        } else if (arg.starts_with("--samples=")) {
            options.sample_count =
                parse_positive_size(arg.substr(std::string_view{"--samples="}.size()), options.sample_count);
        } else if (arg.starts_with("--warmup=")) {
            options.warmup_batches =
                parse_positive_size(arg.substr(std::string_view{"--warmup="}.size()), options.warmup_batches);
        } else if (arg.starts_with("--trials=")) {
            options.trial_count =
                parse_positive_size(arg.substr(std::string_view{"--trials="}.size()), options.trial_count);
        }
    }

    return options;
}

/**
 * @brief Builds passive orders that never cross each other.
 *
 * @param count Number of orders to create.
 * @return Deterministic resting insert stream.
 */
[[nodiscard]] std::vector<Order> make_passive_orders(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const bool is_buy = index % 2 == 0;
        const auto price_offset = static_cast<std::int64_t>(index % 5);
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = is_buy ? Side::Buy : Side::Sell,
                               .price = is_buy ? kBestPassiveBid - price_offset
                                               : kBestPassiveAsk + price_offset,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price resting asks for crossing-buy workloads.
 *
 * @param count Number of asks to create.
 * @return Passive ask liquidity.
 */
[[nodiscard]] std::vector<Order> make_resting_asks(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Sell,
                               .price = kRestingAsk,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds aggressive buy orders that cross the resting ask.
 *
 * @param count Number of buys to create.
 * @return Incoming crossing order stream.
 */
[[nodiscard]] std::vector<Order> make_crossing_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = kIncomingIdBase + static_cast<std::uint64_t>(index),
                               .side = Side::Buy,
                               .price = kCrossingBuy,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price resting buys for cancel workloads.
 *
 * @param count Number of buys to create.
 * @return Passive buy liquidity.
 */
[[nodiscard]] std::vector<Order> make_same_price_resting_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price resting buys whose quantity can be reduced in place.
 *
 * @param count Number of buys to create.
 * @return Passive buy liquidity for modify latency samples.
 */
[[nodiscard]] std::vector<Order> make_same_price_modifiable_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kModifiableQuantity});
    }

    return orders;
}

/**
 * @brief Builds ids that cancel the FIFO front first.
 *
 * @param count Number of cancel ids to create.
 * @return Ascending live order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_front_cancel_ids(std::size_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(static_cast<std::uint64_t>(index + 1));
    }

    return ids;
}

/**
 * @brief Builds ids that cancel the FIFO back first.
 *
 * @param count Number of cancel ids to create.
 * @return Descending live order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_back_cancel_ids(std::size_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(count);

    for (std::size_t remaining = count; remaining > 0; --remaining) {
        ids.push_back(static_cast<std::uint64_t>(remaining));
    }

    return ids;
}

/**
 * @brief Builds a reproducibly shuffled cancel-id stream.
 *
 * @param count Number of cancel ids to create.
 * @return Shuffled live order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_random_cancel_ids(std::size_t count) {
    auto ids = make_front_cancel_ids(count);

    std::mt19937 rng{kCancelShuffleSeed};
    std::ranges::shuffle(ids, rng);

    return ids;
}

/**
 * @brief Builds ids that are guaranteed not to exist in the book.
 *
 * @param count Number of cancel ids to create.
 * @return Unknown order ids.
 */
[[nodiscard]] std::vector<std::uint64_t> make_unknown_cancel_ids(std::size_t count) {
    std::vector<std::uint64_t> ids;
    ids.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(kUnknownOrderIdBase + static_cast<std::uint64_t>(index));
    }

    return ids;
}

/**
 * @brief Creates one passive order for the mixed stream.
 *
 * @param order_id Stable id assigned to the order.
 * @param side Side to rest on the book.
 * @return Non-crossing limit order.
 */
[[nodiscard]] Order make_mixed_resting_order(std::uint64_t order_id, Side side) {
    const auto price = side == Side::Buy ? kRestingBid : kRestingAsk;
    return Order{.id = order_id, .side = side, .price = price, .quantity = kQuantity};
}

/**
 * @brief Creates one aggressive buy for the mixed stream.
 *
 * @param order_id Stable id assigned to the order.
 * @return Crossing buy limit order.
 */
[[nodiscard]] Order make_mixed_crossing_buy(std::uint64_t order_id) {
    return Order{.id = order_id, .side = Side::Buy, .price = kCrossingBuy, .quantity = kQuantity};
}

/**
 * @brief Builds a deterministic 70/20/10 insert/cancel/match stream.
 *
 * @param count Number of operations to generate.
 * @return Mixed operation sequence.
 */
[[nodiscard]] std::vector<MixedOperation> make_mixed_operations(std::size_t count) {
    std::vector<MixedOperation> operations;
    operations.reserve(count);

    std::deque<std::uint64_t> cancelable_buy_ids;
    std::uint64_t next_resting_id = 1;
    std::uint64_t next_crossing_id = kMixedCrossingIdBase;

    for (std::size_t index = 0; index < count; ++index) {
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
 * @brief Inserts resting orders before timing starts.
 *
 * @param book Book to populate.
 * @param orders Passive orders to submit.
 */
void preload_book(OrderBook& book, const std::vector<Order>& orders) {
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    for (const auto& order : orders) {
        book.submit(order, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
}

/**
 * @brief Computes one nearest-rank percentile from sorted samples.
 *
 * @param sorted_samples Samples sorted in ascending order.
 * @param percentile Percentile in the range [0, 1].
 * @return Selected sample value.
 */
[[nodiscard]] double percentile_value(const std::vector<double>& sorted_samples, double percentile) {
    const auto rank = std::ceil(percentile * static_cast<double>(sorted_samples.size()));
    const auto index = static_cast<std::size_t>(
        std::clamp(rank, 1.0, static_cast<double>(sorted_samples.size())) - 1.0);
    return sorted_samples[index];
}

/**
 * @brief Computes percentiles over amortized ns/op batch samples.
 *
 * @param samples Batch-level amortized ns/op values.
 * @return p50, p95, p99, and max values.
 */
[[nodiscard]] Percentiles summarize_samples(std::vector<double> samples) {
    std::ranges::sort(samples);

    return Percentiles{.p50 = percentile_value(samples, 0.50),
                       .p95 = percentile_value(samples, 0.95),
                       .p99 = percentile_value(samples, 0.99),
                       .max = samples.back()};
}

/**
 * @brief Measures fixed-size batches and stores amortized ns/op samples.
 *
 * @tparam Operation Callable that executes one pre-generated operation by index.
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @param operation Operation dispatcher.
 * @return Percentile summary for recorded amortized samples.
 */
template <typename Operation>
[[nodiscard]] Percentiles measure_batches(std::size_t batch_size,
                                          std::size_t sample_count,
                                          std::size_t warmup_batches,
                                          Operation operation) {
    std::vector<double> samples;
    samples.reserve(sample_count);
    std::size_t operation_index = 0;

    for (std::size_t batch = 0; batch < warmup_batches; ++batch) {
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            operation(operation_index++);
        }
    }

    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            operation(operation_index++);
        }
        clobber_memory();
        const auto end = Clock::now();
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        samples.push_back(static_cast<double>(elapsed_ns) / static_cast<double>(batch_size));
    }

    return summarize_samples(std::move(samples));
}

/**
 * @brief Measures resting limit-order insert latency batches.
 *
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_resting_insert_latency(std::size_t batch_size,
                                                     std::size_t sample_count,
                                                     std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Submit-only latency rests every submitted order.
    const auto reserve_order_capacity = total_operations;
    const auto orders = make_passive_orders(total_operations);
    OrderBook book{reserve_order_capacity};
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        book.submit(orders[index], events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures crossing limit-order match latency batches.
 *
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_crossing_match_latency(std::size_t batch_size,
                                                     std::size_t sample_count,
                                                     std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. This match-only benchmark has one resting order per
    // incoming crossing order, so total_operations is the resting-order count.
    const auto reserve_order_capacity = total_operations;
    const auto resting_orders = make_resting_asks(total_operations);
    const auto crossing_orders = make_crossing_buys(total_operations);
    OrderBook book{reserve_order_capacity};
    std::vector<matching_engine::Event> events;
    events.reserve(8);
    preload_book(book, resting_orders);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        book.submit(crossing_orders[index], events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures one cancel-id stream in latency batches.
 *
 * @param cancel_ids Pre-generated cancel sequence.
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_cancel_latency(const std::vector<std::uint64_t>& cancel_ids,
                                             std::size_t batch_size,
                                             std::size_t sample_count,
                                             std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Cancel-only latency preloads one live order for each
    // cancel operation.
    const auto reserve_order_capacity = total_operations;
    const auto resting_orders = make_same_price_resting_buys(total_operations);
    OrderBook book{reserve_order_capacity};
    preload_book(book, resting_orders);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        const auto result = book.cancel(cancel_ids[index]);
        do_not_optimize(result);
    });
}

/**
 * @brief Measures in-place modify latency batches for live resting orders.
 *
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_modify_if_present_latency(std::size_t batch_size,
                                                        std::size_t sample_count,
                                                        std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    // Preload modifiable quantity so timing covers lookup and in-level volume updates only.
    const auto reserve_order_capacity = total_operations;
    const auto resting_orders = make_same_price_modifiable_buys(total_operations);
    const auto target_ids = make_front_cancel_ids(total_operations);
    OrderBook book{reserve_order_capacity};
    std::vector<matching_engine::Event> events;
    events.reserve(4);
    preload_book(book, resting_orders);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        book.modify(target_ids[index], kRestingBid, kQuantity, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures mixed insert/cancel/match latency batches.
 *
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_mixed_latency(std::size_t batch_size,
                                            std::size_t sample_count,
                                            std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Submit/cancel/modify-heavy workloads usually have
    // far fewer concurrent live orders than total messages, so mixed and
    // end-to-end use 10% as the current benchmark-tuned proxy.
    const auto reserve_order_capacity = std::max<std::size_t>(1024, total_operations / 10);
    const auto operations = make_mixed_operations(total_operations);
    OrderBook book{reserve_order_capacity};
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        const auto& operation = operations[index];
        if (operation.kind == MixedOperationKind::Cancel) {
            const auto result = book.cancel(operation.cancel_id);
            do_not_optimize(result);
        } else {
            book.submit(operation.order, events);
            do_not_optimize(events.data());
            do_not_optimize(events.size());
        }
    });
}

/**
 * @brief Measures true mixed OrderBook latency batches.
 *
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of unrecorded batches.
 * @return Percentile summary over amortized batch samples.
 */
[[nodiscard]] Percentiles run_true_mixed_latency(std::size_t batch_size,
                                                 std::size_t sample_count,
                                                 std::size_t warmup_batches) {
    const auto total_operations = (sample_count + warmup_batches) * batch_size;
    const auto workload = make_true_mixed_workload(total_operations);
    OrderBook book;
    std::vector<matching_engine::Event> events;
    events.reserve(16);

    // Setup and preload happen outside measured batches so samples isolate hot-path operations.
    book.reserve_order_capacity(workload.reserve_order_capacity);
    preload_true_mixed_book(book, workload.preload_orders, events);

    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        auto cancel_result = run_true_mixed_operation(book, workload.operations[index], events);
        do_not_optimize(cancel_result);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Runs one named workload for a batch size.
 *
 * @param benchmark_name Workload label for artifacts.
 * @param batch_size Number of operations per timed batch.
 * @param sample_count Number of recorded batches.
 * @param warmup_batches Number of warmup batches.
 * @return Percentile summary for the workload.
 */
[[nodiscard]] Percentiles run_named_workload(std::string_view benchmark_name,
                                             std::size_t batch_size,
                                             std::size_t sample_count,
                                             std::size_t warmup_batches) {
    if (benchmark_name == "BM_OrderBook_PassiveInsert_BatchLatency") {
        return run_resting_insert_latency(batch_size, sample_count, warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_OneLevelCrossingMatch_BatchLatency") {
        return run_crossing_match_latency(batch_size, sample_count, warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_CancelFront_BatchLatency") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_front_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_CancelBack_BatchLatency") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_back_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_CancelRandom_BatchLatency") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_random_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_CancelUnknown_BatchLatency") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_unknown_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_ModifyIfPresent_BatchLatency") {
        return run_modify_if_present_latency(batch_size, sample_count, warmup_batches);
    }
    if (benchmark_name == "BM_OrderBook_TrueMixed_BatchLatency") {
        return run_true_mixed_latency(batch_size, sample_count, warmup_batches);
    }

    return run_mixed_latency(batch_size, sample_count, warmup_batches);
}

/**
 * @brief Collects all latency benchmark results for the configured run.
 *
 * @param options Runtime options controlling samples, warmup, and trials.
 * @return Deterministically ordered result rows.
 */
[[nodiscard]] std::vector<LatencyResult> run_all_latency_benchmarks(const Options& options) {
    constexpr std::string_view workloads[] = {
        "BM_OrderBook_PassiveInsert_BatchLatency",
        "BM_OrderBook_OneLevelCrossingMatch_BatchLatency",
        "BM_OrderBook_CancelRandom_BatchLatency",
        "BM_OrderBook_CancelUnknown_BatchLatency",
        "BM_OrderBook_ModifyIfPresent_BatchLatency",
        "BM_OrderBook_TrueMixed_BatchLatency"};
    constexpr std::size_t batch_sizes[] = {64, 256, 1'024};
    std::vector<LatencyResult> results;
    results.reserve(std::size(workloads) * std::size(batch_sizes) * options.trial_count);

    for (std::size_t trial = 1; trial <= options.trial_count; ++trial) {
        for (const auto workload : workloads) {
            for (const auto batch_size : batch_sizes) {
                const auto workload_size = (options.sample_count + options.warmup_batches) * batch_size;
                auto percentiles =
                    run_named_workload(workload, batch_size, options.sample_count, options.warmup_batches);
                results.push_back(LatencyResult{.benchmark_name = std::string(workload),
                                                .workload_size = workload_size,
                                                .batch_size = batch_size,
                                                .sample_count = options.sample_count,
                                                .trial = trial,
                                                .percentiles = percentiles});
            }
        }
    }

    return results;
}

/**
 * @brief Writes human-readable latency results.
 *
 * @param path Output path for the text artifact.
 * @param options Runtime options used for the run.
 * @param results Result rows to write.
 */
void write_text_results(const std::filesystem::path& path,
                        const Options& options,
                        const std::vector<LatencyResult>& results) {
    std::ofstream output{path};
    output << "Amortized batch latency results\n";
    output << "sample_count=" << options.sample_count << '\n';
    output << "warmup_batches=" << options.warmup_batches << '\n';
    output << "trials=" << options.trial_count << '\n';
    output << "unit=ns/op over fixed-size timed batches\n\n";
    output << std::left << std::setw(28) << "benchmark" << std::right << std::setw(14)
           << "workload" << std::setw(10) << "batch" << std::setw(10) << "samples"
           << std::setw(8) << "trial" << std::setw(14) << "p50" << std::setw(14) << "p95"
           << std::setw(14) << "p99" << std::setw(14) << "max" << '\n';
    output << std::string(126, '-') << '\n';
    output << std::fixed << std::setprecision(2);

    for (const auto& result : results) {
        output << std::left << std::setw(28) << result.benchmark_name << std::right
               << std::setw(14) << result.workload_size << std::setw(10) << result.batch_size
               << std::setw(10) << result.sample_count << std::setw(8) << result.trial
               << std::setw(14) << result.percentiles.p50 << std::setw(14)
               << result.percentiles.p95 << std::setw(14) << result.percentiles.p99
               << std::setw(14) << result.percentiles.max << '\n';
    }
}

/**
 * @brief Writes machine-readable latency results.
 *
 * @param path Output path for the JSON artifact.
 * @param options Runtime options used for the run.
 * @param results Result rows to write.
 */
void write_json_results(const std::filesystem::path& path,
                        const Options& options,
                        const std::vector<LatencyResult>& results) {
    std::ofstream output{path};
    output << std::fixed << std::setprecision(2);
    output << "{\n";
    output << "  \"sample_count\": " << options.sample_count << ",\n";
    output << "  \"warmup_batches\": " << options.warmup_batches << ",\n";
    output << "  \"trials\": " << options.trial_count << ",\n";
    output << "  \"unit\": \"ns/op over fixed-size timed batches\",\n";
    output << "  \"results\": [\n";

    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        output << "    {\n";
        output << "      \"benchmark_name\": \"" << result.benchmark_name << "\",\n";
        output << "      \"workload_size\": " << result.workload_size << ",\n";
        output << "      \"batch_size\": " << result.batch_size << ",\n";
        output << "      \"sample_count\": " << result.sample_count << ",\n";
        output << "      \"trial\": " << result.trial << ",\n";
        output << "      \"p50_ns_per_op\": " << result.percentiles.p50 << ",\n";
        output << "      \"p95_ns_per_op\": " << result.percentiles.p95 << ",\n";
        output << "      \"p99_ns_per_op\": " << result.percentiles.p99 << ",\n";
        output << "      \"max_ns_per_op\": " << result.percentiles.max << "\n";
        output << "    }";
        if (index + 1 != results.size()) {
            output << ',';
        }
        output << '\n';
    }

    output << "  ]\n";
    output << "}\n";
}

/**
 * @brief Entry point for the standalone latency benchmark runner.
 *
 * @param argc Argument count.
 * @param argv Argument values.
 * @return Process exit code.
 */
int latency_main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    std::filesystem::create_directories(options.output_dir);

    const auto results = run_all_latency_benchmarks(options);
    write_text_results(options.output_dir / "batch_latency_results.txt", options, results);
    write_json_results(options.output_dir / "batch_latency_results.json", options, results);

    std::cout << "Wrote " << (options.output_dir / "batch_latency_results.txt") << '\n';
    std::cout << "Wrote " << (options.output_dir / "batch_latency_results.json") << '\n';
    return 0;
}

} // namespace

/**
 * @brief Program entry point.
 */
int main(int argc, char** argv) {
    return latency_main(argc, argv);
}
