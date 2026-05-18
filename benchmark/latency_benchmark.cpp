#include "book/order_book.hpp"

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

constexpr std::int64_t kBestPassiveBid = 99;
constexpr std::int64_t kBestPassiveAsk = 101;
constexpr std::int64_t kRestingBid = 100;
constexpr std::int64_t kRestingAsk = 102;
constexpr std::int64_t kCrossingBuy = 105;
constexpr std::uint64_t kQuantity = 1;
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
    std::filesystem::path output_dir{"benchmarks"};
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
    // Use a compiler barrier when available so hot-path calls remain observable.
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
    // Keep writes inside the timed batch visible before reading the clock again.
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
    // Parse through strtoull so invalid input can fall back without exceptions.
    char* end = nullptr;
    const std::string text{value};
    const auto parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed == 0) {
        return fallback;
    }

    // Cast after validation because benchmark sizes are bounded by memory anyway.
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

    // Keep the command-line surface intentionally small and deterministic.
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

    // Return the normalized runner options.
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

    // Alternate sides across a few stable price levels so warmup creates the levels.
    for (std::size_t index = 0; index < count; ++index) {
        const bool is_buy = index % 2 == 0;
        const auto price_offset = static_cast<std::int64_t>(index % 5);
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = is_buy ? Side::Buy : Side::Sell,
                               .price = is_buy ? kBestPassiveBid - price_offset
                                               : kBestPassiveAsk + price_offset,
                               .quantity = kQuantity});
    }

    // Return the pre-generated order stream used by timed batches.
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

    // Same-price asks make each aggressive buy consume exactly one resting order.
    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Sell,
                               .price = kRestingAsk,
                               .quantity = kQuantity});
    }

    // Return the liquidity preload.
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

    // Use ids outside the resting range so duplicate checks never reject.
    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = kIncomingIdBase + static_cast<std::uint64_t>(index),
                               .side = Side::Buy,
                               .price = kCrossingBuy,
                               .quantity = kQuantity});
    }

    // Return the prepared aggressive stream.
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

    // One price level makes front/back/random cancel positions explicit.
    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<std::uint64_t>(index + 1),
                               .side = Side::Buy,
                               .price = kRestingBid,
                               .quantity = kQuantity});
    }

    // Return the cancel preload stream.
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

    // Ascending ids match insertion order at the single price level.
    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(static_cast<std::uint64_t>(index + 1));
    }

    // Return the front-cancel stream.
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

    // Descending ids target the newest remaining order each time.
    for (std::size_t remaining = count; remaining > 0; --remaining) {
        ids.push_back(static_cast<std::uint64_t>(remaining));
    }

    // Return the back-cancel stream.
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

    // Use a fixed seed so random-cancel locality is comparable across runs.
    std::mt19937 rng{kCancelShuffleSeed};
    std::ranges::shuffle(ids, rng);

    // Return the deterministic random cancel stream.
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

    // Keep rejected ids far away from generated live order ids.
    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(kUnknownOrderIdBase + static_cast<std::uint64_t>(index));
    }

    // Return the unknown-cancel stream.
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
    // Price the two sides apart so normal inserts remain passive.
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
    // Price above the passive ask to exercise the match path.
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

    // Track cancelable buys because aggressive buys only consume resting asks.
    std::deque<std::uint64_t> cancelable_buy_ids;
    std::uint64_t next_resting_id = 1;
    std::uint64_t next_crossing_id = kMixedCrossingIdBase;

    // Repeat the existing benchmark mix: seven inserts, two cancels, one match.
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

    // Return the fully prepared mixed workload.
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

    // Build the starting state outside measured batches.
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
    // Convert the requested percentile to a stable nearest-rank index.
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
    // Sort after measurement so percentile work is not inside timed batches.
    std::ranges::sort(samples);

    // Report the requested latency summary over amortized batch samples.
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

    // Run warmup batches first so one-time cache and level-creation effects settle.
    for (std::size_t batch = 0; batch < warmup_batches; ++batch) {
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            operation(operation_index++);
        }
    }

    // Record one amortized ns/op sample per timed batch.
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

    // Summarize samples outside the measured operation loop.
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
    const auto orders = make_passive_orders(total_operations);
    OrderBook book{total_operations};
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    // Submit pre-generated orders into one pre-reserved book.
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
    const auto resting_orders = make_resting_asks(total_operations);
    const auto crossing_orders = make_crossing_buys(total_operations);
    OrderBook book{total_operations};
    std::vector<matching_engine::Event> events;
    events.reserve(8);
    preload_book(book, resting_orders);

    // Submit pre-generated aggressive buys against preloaded liquidity.
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
    const auto resting_orders = make_same_price_resting_buys(total_operations);
    OrderBook book{total_operations};
    preload_book(book, resting_orders);

    // Cancel pre-generated ids from the preloaded book.
    return measure_batches(batch_size, sample_count, warmup_batches, [&](std::size_t index) {
        const auto result = book.cancel(cancel_ids[index]);
        do_not_optimize(result);
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
    const auto operations = make_mixed_operations(total_operations);
    OrderBook book{total_operations};
    std::vector<matching_engine::Event> events;
    events.reserve(8);

    // Dispatch the prepared exchange-style stream without allocating per operation.
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
    // Keep workload dispatch outside the measured operation loops.
    if (benchmark_name == "RestingLimitOrderInsert") {
        return run_resting_insert_latency(batch_size, sample_count, warmup_batches);
    }
    if (benchmark_name == "CrossingLimitOrderMatch") {
        return run_crossing_match_latency(batch_size, sample_count, warmup_batches);
    }
    if (benchmark_name == "CancelFront") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_front_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "CancelBack") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_back_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "CancelRandom") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_random_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }
    if (benchmark_name == "CancelUnknown") {
        const auto total_operations = (sample_count + warmup_batches) * batch_size;
        return run_cancel_latency(make_unknown_cancel_ids(total_operations), batch_size, sample_count,
                                  warmup_batches);
    }

    // MixedSubmitCancel is the only remaining registered workload.
    return run_mixed_latency(batch_size, sample_count, warmup_batches);
}

/**
 * @brief Collects all latency benchmark results for the configured run.
 *
 * @param options Runtime options controlling samples, warmup, and trials.
 * @return Deterministically ordered result rows.
 */
[[nodiscard]] std::vector<LatencyResult> run_all_latency_benchmarks(const Options& options) {
    constexpr std::string_view workloads[] = {"RestingLimitOrderInsert", "CrossingLimitOrderMatch",
                                              "CancelFront", "CancelBack", "CancelRandom",
                                              "CancelUnknown", "MixedSubmitCancel"};
    constexpr std::size_t batch_sizes[] = {64, 256, 1'024};
    std::vector<LatencyResult> results;
    results.reserve(std::size(workloads) * std::size(batch_sizes) * options.trial_count);

    // Run in a fixed order so text and JSON artifacts are easy to diff.
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

    // Return the completed artifact rows.
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

    // Emit one stable row per workload, batch size, and trial.
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

    // Write stable objects with required fields and the trial number.
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

    // Run all registered latency workloads before writing artifacts.
    const auto results = run_all_latency_benchmarks(options);
    write_text_results(options.output_dir / "latency_results.txt", options, results);
    write_json_results(options.output_dir / "latency_results.json", options, results);

    // Print artifact paths so scripts have a clear terminal breadcrumb.
    std::cout << "Wrote " << (options.output_dir / "latency_results.txt") << '\n';
    std::cout << "Wrote " << (options.output_dir / "latency_results.json") << '\n';
    return 0;
}

} // namespace

/**
 * @brief Program entry point.
 */
int main(int argc, char** argv) {
    // Keep main tiny so benchmark logic remains testable by inspection.
    return latency_main(argc, argv);
}
