#include "io/local_benchmark_comparison.hpp"

#include "benchmarks/realistic_flow/true_mixed_workload.hpp"
#include "book/order_book.hpp"
#include "toy/order_book.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace matching_engine {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int64_t kBestPassiveBid = 99;
constexpr std::int64_t kBestPassiveAsk = 101;
constexpr std::int64_t kRestingBid = 100;
constexpr std::int64_t kRestingAsk = 102;
constexpr std::int64_t kCrossingBuy = 105;
constexpr std::uint64_t kQuantity = 1;
constexpr std::uint64_t kModifiableQuantity = 2;
constexpr std::uint64_t kIncomingIdBase = 1'000'000'000;
constexpr std::uint64_t kUnknownOrderIdBase = 2'000'000'000;
constexpr std::uint32_t kCancelShuffleSeed = 0xC0FFEE;
constexpr std::size_t kThroughputOperationCount = 10'000;
constexpr std::size_t kLatencyBatchSize = 256;
constexpr std::size_t kLatencySampleCount = 32;
constexpr std::size_t kLatencyWarmupBatches = 8;

/**
 * @brief Names the local comparison workloads.
 */
enum class LocalWorkload {
    PassiveInsert,
    OneLevelCrossingMatch,
    RandomCancel,
    UnknownCancel,
    ModifyIfPresent,
    OrderBookTrueMixed,
};

/**
 * @brief Percentile summary for amortized fixed-batch latency samples.
 */
struct Percentiles {
    double p50_ns{};
    double p99_ns{};
    double max_ns{};
};

/**
 * @brief Timing result for one engine on one workload.
 */
struct EngineResult {
    double ops_per_second{};
    Percentiles latency;
};

/**
 * @brief Side-by-side result for the optimized and toy books.
 */
struct ComparisonResult {
    LocalWorkload workload{};
    EngineResult optimized;
    EngineResult baseline;
};

/**
 * @brief Prevents the optimizer from discarding benchmarked values.
 *
 * @tparam T Value type kept observable to the compiler.
 * @param value Value produced by measured code.
 */
template <typename T>
void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    // A compiler barrier keeps local CLI timing from becoming a dead-code benchmark.
    asm volatile("" : : "g"(&value) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Prevents memory writes from moving across a measurement boundary.
 */
void clobber_memory() {
#if defined(__GNUC__) || defined(__clang__)
    // Force writes from book mutation and event buffers to stay before the stop timestamp.
    asm volatile("" : : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Returns a trimmed copy of user input.
 *
 * @param text Raw input line.
 * @return Input without leading or trailing ASCII whitespace.
 */
[[nodiscard]] std::string trim(std::string_view text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }

    // Preserve the user's spelling after removing only outer whitespace.
    return std::string(first, last);
}

/**
 * @brief Converts user control input to uppercase.
 *
 * @param text Raw input text.
 * @return Uppercase copy used for command matching.
 */
[[nodiscard]] std::string uppercase_copy(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });

    return result;
}

/**
 * @brief Checks whether a response requests leaving the current screen.
 *
 * @param text User response text.
 * @return True for q, quit, or exit.
 */
[[nodiscard]] bool is_quit_command(std::string_view text) {
    const auto command = uppercase_copy(trim(text));
    return command == "Q" || command == "QUIT" || command == "EXIT";
}

/**
 * @brief Waits before a local benchmark run.
 *
 * @param input User input stream.
 * @param output Presentation output stream.
 * @param workload_name Workload about to run.
 * @return True when Enter was pressed; false when the user exits.
 */
[[nodiscard]] bool confirm_benchmark_run(std::istream& input,
                                         std::ostream& output,
                                         std::string_view workload_name) {
    output << "\nReady to run " << workload_name << ".\n"
           << "Press Enter to start, or type q to return to the main menu: ";
    output.flush();

    std::string response;
    if (!std::getline(input, response)) {
        return false;
    }

    if (is_quit_command(response)) {
        output << "\nReturning to main menu.\n";
        return false;
    }

    // Non-empty text other than q still proceeds so Enter-like input stays forgiving.
    return true;
}

/**
 * @brief Pauses after a benchmark result.
 *
 * @param input User input stream.
 * @param output Presentation output stream.
 * @return True when the local benchmark menu should continue.
 */
[[nodiscard]] bool pause_after_result(std::istream& input, std::ostream& output) {
    output << "\nPress Enter to continue, or type q to return to the main menu: ";
    output.flush();

    std::string response;
    if (!std::getline(input, response)) {
        return false;
    }

    if (is_quit_command(response)) {
        output << "\nReturning to main menu.\n";
        return false;
    }

    return true;
}

/**
 * @brief Builds passive orders that never cross.
 *
 * @param count Number of orders to create.
 * @return Deterministic non-crossing order stream.
 */
[[nodiscard]] std::vector<Order> make_passive_orders(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const bool is_buy = index % 2 == 0;
        const auto price_offset = static_cast<std::int64_t>(index % 5);
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
 * @param count Number of asks to create.
 * @return Deterministic passive ask stream.
 */
[[nodiscard]] std::vector<Order> make_resting_asks(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = static_cast<OrderId>(index + 1),
                               .side = Side::Sell,
                               .price = kRestingAsk,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds aggressive buys that cross the one-level ask queue.
 *
 * @param count Number of buys to create.
 * @return Deterministic crossing buy stream.
 */
[[nodiscard]] std::vector<Order> make_crossing_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        orders.push_back(Order{.id = kIncomingIdBase + static_cast<OrderId>(index),
                               .side = Side::Buy,
                               .price = kCrossingBuy,
                               .quantity = kQuantity});
    }

    return orders;
}

/**
 * @brief Builds same-price buy orders for cancel benchmarks.
 *
 * @param count Number of buys to create.
 * @return Deterministic passive buy stream.
 */
[[nodiscard]] std::vector<Order> make_same_price_resting_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
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
 * @param count Number of buys to create.
 * @return Deterministic passive buy stream for modify tests.
 */
[[nodiscard]] std::vector<Order> make_same_price_modifiable_buys(std::size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
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
 * @param count Number of ids to create.
 * @return Ascending live order ids.
 */
[[nodiscard]] std::vector<OrderId> make_front_ids(std::size_t count) {
    std::vector<OrderId> ids;
    ids.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(static_cast<OrderId>(index + 1));
    }

    return ids;
}

/**
 * @brief Builds ids that cancel live orders in a fixed shuffled order.
 *
 * @param count Number of ids to create.
 * @return Reproducibly shuffled live order ids.
 */
[[nodiscard]] std::vector<OrderId> make_random_cancel_ids(std::size_t count) {
    auto ids = make_front_ids(count);
    std::mt19937 rng{kCancelShuffleSeed};
    std::ranges::shuffle(ids, rng);

    return ids;
}

/**
 * @brief Builds ids that are guaranteed to miss the live order index.
 *
 * @param count Number of ids to create.
 * @return Deterministic unknown order ids.
 */
[[nodiscard]] std::vector<OrderId> make_unknown_cancel_ids(std::size_t count) {
    std::vector<OrderId> ids;
    ids.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(kUnknownOrderIdBase + static_cast<OrderId>(index));
    }

    return ids;
}

/**
 * @brief Constructs a book and applies reserve hints where the type supports them.
 *
 * @tparam Book Book implementation under test.
 * @param reserve_order_capacity Expected peak live order count.
 * @return Book ready for setup or timing.
 */
template <typename Book>
[[nodiscard]] Book make_book(std::size_t reserve_order_capacity) {
    if constexpr (std::is_same_v<Book, OrderBook>) {
        return Book{reserve_order_capacity};
    } else {
        static_cast<void>(reserve_order_capacity);
        return Book{};
    }
}

/**
 * @brief Inserts resting orders before timing begins.
 *
 * @tparam Book Book implementation under test.
 * @param book Book to populate.
 * @param orders Orders to submit.
 */
template <typename Book>
void preload_book(Book& book, const std::vector<Order>& orders) {
    std::vector<Event> events;
    events.reserve(8);

    for (const auto& order : orders) {
        book.submit(order, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
}

/**
 * @brief Computes operations per second from elapsed time.
 *
 * @param operation_count Number of operations processed.
 * @param elapsed Elapsed wall time.
 * @return Operations per second, or zero if the clock interval is empty.
 */
[[nodiscard]] double operations_per_second(std::size_t operation_count, Clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(operation_count) / seconds;
}

/**
 * @brief Measures passive insert throughput for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param operation_count Number of timed orders.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_passive_insert_throughput(std::size_t operation_count) {
    const auto orders = make_passive_orders(operation_count);
    auto book = make_book<Book>(operation_count);
    std::vector<Event> events;
    events.reserve(8);

    const auto start = Clock::now();
    for (const auto& order : orders) {
        book.submit(order, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
    clobber_memory();
    const auto end = Clock::now();
    do_not_optimize(book);

    return operations_per_second(operation_count, end - start);
}

/**
 * @brief Measures one-level crossing throughput for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param operation_count Number of timed orders.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_crossing_match_throughput(std::size_t operation_count) {
    const auto resting_orders = make_resting_asks(operation_count);
    const auto crossing_orders = make_crossing_buys(operation_count);
    auto book = make_book<Book>(operation_count);
    std::vector<Event> events;
    events.reserve(8);
    preload_book(book, resting_orders);

    const auto start = Clock::now();
    for (const auto& order : crossing_orders) {
        book.submit(order, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
    clobber_memory();
    const auto end = Clock::now();
    do_not_optimize(book);

    return operations_per_second(operation_count, end - start);
}

/**
 * @brief Measures one cancel-id stream for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param cancel_ids Pre-generated cancel ids.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_cancel_throughput(const std::vector<OrderId>& cancel_ids) {
    const auto resting_orders = make_same_price_resting_buys(cancel_ids.size());
    auto book = make_book<Book>(cancel_ids.size());
    preload_book(book, resting_orders);

    const auto start = Clock::now();
    for (const auto order_id : cancel_ids) {
        const auto result = book.cancel(order_id);
        do_not_optimize(result);
    }
    clobber_memory();
    const auto end = Clock::now();
    do_not_optimize(book);

    return operations_per_second(cancel_ids.size(), end - start);
}

/**
 * @brief Measures in-place modify throughput for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param operation_count Number of timed modifies.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_modify_if_present_throughput(std::size_t operation_count) {
    const auto resting_orders = make_same_price_modifiable_buys(operation_count);
    const auto target_ids = make_front_ids(operation_count);
    auto book = make_book<Book>(operation_count);
    std::vector<Event> events;
    events.reserve(4);
    preload_book(book, resting_orders);

    const auto start = Clock::now();
    for (const auto order_id : target_ids) {
        book.modify(order_id, kRestingBid, kQuantity, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
    clobber_memory();
    const auto end = Clock::now();
    do_not_optimize(book);

    return operations_per_second(operation_count, end - start);
}

/**
 * @brief Preloads deterministic true-mixed liquidity before timing.
 *
 * @tparam Book Book implementation under test.
 * @param book Book to populate.
 * @param preload_orders Pre-generated resting orders.
 * @param events Reusable caller-owned event buffer.
 */
template <typename Book>
void preload_true_mixed_book_local(
    Book& book,
    const std::vector<Order>& preload_orders,
    std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload is setup work so the timed path sees the intended mixed stream.
        book.submit(order, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
}

/**
 * @brief Runs one true-mixed operation against either book implementation.
 *
 * @tparam Book Book implementation under test.
 * @param book Book being mutated.
 * @param operation Pre-generated operation.
 * @param events Reusable caller-owned event buffer.
 * @return Cancel result when applicable, otherwise a harmless rejected result.
 */
template <typename Book>
[[nodiscard]] CancelResult run_true_mixed_operation_local(
    Book& book,
    const benchmark_workloads::TrueMixedOperation& operation,
    std::vector<Event>& events) {
    using benchmark_workloads::TrueMixedOperationKind;

    if (operation.kind == TrueMixedOperationKind::Cancel) {
        // Cancel has a compact single-result API on both implementations.
        return book.cancel(operation.target_order_id);
    }

    if (operation.kind == TrueMixedOperationKind::Modify) {
        // Modify reuses the caller-owned event buffer and may preserve or lose FIFO priority.
        book.modify(operation.target_order_id, operation.new_price, operation.new_quantity, events);
    } else if (operation.kind == TrueMixedOperationKind::Market) {
        // Market orders are transient taker flow and never rest.
        book.submit_market(operation.order, events);
    } else {
        // GTC, IOC, and FOK limit orders share the submit path.
        book.submit(operation.order, events);
    }

    return RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = 0};
}

/**
 * @brief Measures direct OrderBook true-mixed throughput for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param operation_count Number of timed operations.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_true_mixed_throughput(std::size_t operation_count) {
    const auto workload = benchmark_workloads::make_true_mixed_workload(operation_count);
    auto book = make_book<Book>(workload.reserve_order_capacity);
    std::vector<Event> events;
    events.reserve(16);
    preload_true_mixed_book_local(book, workload.preload_orders, events);

    const auto start = Clock::now();
    for (const auto& operation : workload.operations) {
        auto cancel_result = run_true_mixed_operation_local(book, operation, events);
        do_not_optimize(cancel_result);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
    clobber_memory();
    const auto end = Clock::now();
    do_not_optimize(book);

    return operations_per_second(operation_count, end - start);
}

/**
 * @brief Selects and runs a throughput workload for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param workload Workload to measure.
 * @return Operations per second.
 */
template <typename Book>
[[nodiscard]] double run_throughput(LocalWorkload workload) {
    switch (workload) {
    case LocalWorkload::PassiveInsert:
        return run_passive_insert_throughput<Book>(kThroughputOperationCount);
    case LocalWorkload::OneLevelCrossingMatch:
        return run_crossing_match_throughput<Book>(kThroughputOperationCount);
    case LocalWorkload::RandomCancel:
        return run_cancel_throughput<Book>(make_random_cancel_ids(kThroughputOperationCount));
    case LocalWorkload::UnknownCancel:
        return run_cancel_throughput<Book>(make_unknown_cancel_ids(kThroughputOperationCount));
    case LocalWorkload::ModifyIfPresent:
        return run_modify_if_present_throughput<Book>(kThroughputOperationCount);
    case LocalWorkload::OrderBookTrueMixed:
        return run_true_mixed_throughput<Book>(kThroughputOperationCount);
    }

    return 0.0;
}

/**
 * @brief Selects one nearest-rank percentile from sorted samples.
 *
 * @param sorted_samples Samples sorted ascending.
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
 * @brief Summarizes amortized ns/op samples.
 *
 * @param samples Batch samples to summarize.
 * @return p50, p99, and max values.
 */
[[nodiscard]] Percentiles summarize_samples(std::vector<double> samples) {
    std::ranges::sort(samples);

    return Percentiles{.p50_ns = percentile_value(samples, 0.50),
                       .p99_ns = percentile_value(samples, 0.99),
                       .max_ns = samples.back()};
}

/**
 * @brief Measures fixed-size batches and stores amortized ns/op samples.
 *
 * @tparam Operation Callable that runs one pre-generated operation by index.
 * @param operation Operation dispatcher.
 * @return Percentile summary over recorded batches.
 */
template <typename Operation>
[[nodiscard]] Percentiles measure_latency_batches(Operation operation) {
    std::vector<double> samples;
    samples.reserve(kLatencySampleCount);
    std::size_t operation_index = 0;

    for (std::size_t batch = 0; batch < kLatencyWarmupBatches; ++batch) {
        for (std::size_t offset = 0; offset < kLatencyBatchSize; ++offset) {
            operation(operation_index++);
        }
    }

    for (std::size_t sample = 0; sample < kLatencySampleCount; ++sample) {
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < kLatencyBatchSize; ++offset) {
            operation(operation_index++);
        }
        clobber_memory();
        const auto end = Clock::now();
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        samples.push_back(static_cast<double>(elapsed_ns) / static_cast<double>(kLatencyBatchSize));
    }

    return summarize_samples(std::move(samples));
}

/**
 * @brief Measures passive insert batch latency for one book type.
 *
 * @tparam Book Book implementation under test.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_passive_insert_latency() {
    const auto total_operations = (kLatencySampleCount + kLatencyWarmupBatches) * kLatencyBatchSize;
    const auto orders = make_passive_orders(total_operations);
    auto book = make_book<Book>(total_operations);
    std::vector<Event> events;
    events.reserve(8);

    return measure_latency_batches([&](std::size_t index) {
        book.submit(orders[index], events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures one-level crossing batch latency for one book type.
 *
 * @tparam Book Book implementation under test.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_crossing_match_latency() {
    const auto total_operations = (kLatencySampleCount + kLatencyWarmupBatches) * kLatencyBatchSize;
    const auto resting_orders = make_resting_asks(total_operations);
    const auto crossing_orders = make_crossing_buys(total_operations);
    auto book = make_book<Book>(total_operations);
    std::vector<Event> events;
    events.reserve(8);
    preload_book(book, resting_orders);

    return measure_latency_batches([&](std::size_t index) {
        book.submit(crossing_orders[index], events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures one cancel-id stream batch latency for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param cancel_ids Pre-generated cancel ids.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_cancel_latency(const std::vector<OrderId>& cancel_ids) {
    const auto resting_orders = make_same_price_resting_buys(cancel_ids.size());
    auto book = make_book<Book>(cancel_ids.size());
    preload_book(book, resting_orders);

    return measure_latency_batches([&](std::size_t index) {
        const auto result = book.cancel(cancel_ids[index]);
        do_not_optimize(result);
    });
}

/**
 * @brief Measures in-place modify batch latency for one book type.
 *
 * @tparam Book Book implementation under test.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_modify_if_present_latency() {
    const auto total_operations = (kLatencySampleCount + kLatencyWarmupBatches) * kLatencyBatchSize;
    const auto resting_orders = make_same_price_modifiable_buys(total_operations);
    const auto target_ids = make_front_ids(total_operations);
    auto book = make_book<Book>(total_operations);
    std::vector<Event> events;
    events.reserve(4);
    preload_book(book, resting_orders);

    return measure_latency_batches([&](std::size_t index) {
        book.modify(target_ids[index], kRestingBid, kQuantity, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Measures true-mixed batch latency for one book type.
 *
 * @tparam Book Book implementation under test.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_true_mixed_latency() {
    const auto total_operations = (kLatencySampleCount + kLatencyWarmupBatches) * kLatencyBatchSize;
    const auto workload = benchmark_workloads::make_true_mixed_workload(total_operations);
    auto book = make_book<Book>(workload.reserve_order_capacity);
    std::vector<Event> events;
    events.reserve(16);
    preload_true_mixed_book_local(book, workload.preload_orders, events);

    return measure_latency_batches([&](std::size_t index) {
        auto cancel_result = run_true_mixed_operation_local(book, workload.operations[index], events);
        do_not_optimize(cancel_result);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    });
}

/**
 * @brief Selects and runs a latency workload for one book type.
 *
 * @tparam Book Book implementation under test.
 * @param workload Workload to measure.
 * @return Percentile summary over amortized 256-operation batches.
 */
template <typename Book>
[[nodiscard]] Percentiles run_latency(LocalWorkload workload) {
    const auto total_operations = (kLatencySampleCount + kLatencyWarmupBatches) * kLatencyBatchSize;

    switch (workload) {
    case LocalWorkload::PassiveInsert:
        return run_passive_insert_latency<Book>();
    case LocalWorkload::OneLevelCrossingMatch:
        return run_crossing_match_latency<Book>();
    case LocalWorkload::RandomCancel:
        return run_cancel_latency<Book>(make_random_cancel_ids(total_operations));
    case LocalWorkload::UnknownCancel:
        return run_cancel_latency<Book>(make_unknown_cancel_ids(total_operations));
    case LocalWorkload::ModifyIfPresent:
        return run_modify_if_present_latency<Book>();
    case LocalWorkload::OrderBookTrueMixed:
        return run_true_mixed_latency<Book>();
    }

    return {};
}

/**
 * @brief Runs the optimized and toy books for one workload.
 *
 * @param workload Workload to measure.
 * @return Side-by-side benchmark result.
 */
[[nodiscard]] ComparisonResult run_comparison(LocalWorkload workload) {
    EngineResult optimized;
    optimized.ops_per_second = run_throughput<OrderBook>(workload);
    optimized.latency = run_latency<OrderBook>(workload);

    EngineResult baseline;
    baseline.ops_per_second = run_throughput<toy::OrderBook>(workload);
    baseline.latency = run_latency<toy::OrderBook>(workload);

    return ComparisonResult{.workload = workload, .optimized = optimized, .baseline = baseline};
}

/**
 * @brief Returns a compact display name for a workload.
 *
 * @param workload Workload identifier.
 * @return Human-readable workload name.
 */
[[nodiscard]] std::string_view workload_name(LocalWorkload workload) {
    switch (workload) {
    case LocalWorkload::PassiveInsert:
        return "Passive Insert";
    case LocalWorkload::OneLevelCrossingMatch:
        return "One-Level Crossing Match";
    case LocalWorkload::RandomCancel:
        return "Random Cancel";
    case LocalWorkload::UnknownCancel:
        return "Unknown Cancel";
    case LocalWorkload::ModifyIfPresent:
        return "Modify If Present";
    case LocalWorkload::OrderBookTrueMixed:
        return "OrderBook True Mixed";
    }

    return "Unknown";
}

/**
 * @brief Explains the optimized engine advantage for a workload.
 *
 * @param workload Workload identifier.
 * @return Short explanatory text.
 */
[[nodiscard]] std::string_view optimized_advantage(LocalWorkload workload) {
    switch (workload) {
    case LocalWorkload::PassiveInsert:
        return "Optimized mode reserves stable order storage and appends through compact price "
               "levels, reducing allocator churn while maintaining the live id index.";
    case LocalWorkload::OneLevelCrossingMatch:
        return "Optimized mode removes filled FIFO nodes directly from pooled storage; the toy "
               "path relies on std::deque/std::map bookkeeping for the same one-level match flow.";
    case LocalWorkload::RandomCancel:
        return "Optimized mode maps order ids directly to intrusive order nodes, so shuffled "
               "cancels avoid book-wide scans; the toy path searches std::deque price levels for "
               "each requested id.";
    case LocalWorkload::UnknownCancel:
        return "Optimized mode uses a dense flat lookup table for misses, which is designed for "
               "cache-local id probes instead of scanning visible book queues.";
    case LocalWorkload::ModifyIfPresent:
        return "Optimized mode jumps from id lookup to the live node and updates aggregate volume; "
               "the toy path scans visible queues before reducing quantity.";
    case LocalWorkload::OrderBookTrueMixed:
        return "Optimized mode combines dense id lookup, intrusive FIFO removal, pooled storage, "
               "and reusable event buffers across interleaved submit, cancel, modify, IOC, market, "
               "and FOK flow.";
    }

    return "";
}

/**
 * @brief Explains the std baseline advantage for a workload.
 *
 * @param workload Workload identifier.
 * @return Short explanatory text.
 */
[[nodiscard]] std::string_view baseline_advantage(LocalWorkload workload) {
    switch (workload) {
    case LocalWorkload::PassiveInsert:
        return "The std baseline is intentionally straightforward, but map/deque insertion and "
               "duplicate checks pay general-purpose container and scan costs.";
    case LocalWorkload::OneLevelCrossingMatch:
        return "The std baseline benefits from simple deque front removal in this narrow case, "
               "though it still carries general-purpose container bookkeeping.";
    case LocalWorkload::RandomCancel:
        return "The std baseline keeps no book-local id lookup, so shuffled cancels repeatedly "
               "scan visible queues before erasing.";
    case LocalWorkload::UnknownCancel:
        return "The std baseline rejects misses only after scanning all visible queues, which is "
               "the intentionally slow old-baseline behavior.";
    case LocalWorkload::ModifyIfPresent:
        return "The std baseline has simple storage, but it must scan visible queues before it can "
               "apply the same-price quantity reduction.";
    case LocalWorkload::OrderBookTrueMixed:
        return "The std baseline runs the same deterministic operation stream, but duplicate "
               "checks, cancels, modifies, and misses scan the visible std::deque price levels.";
    }

    return "";
}

/**
 * @brief Formats operations per second in millions.
 *
 * @param value Operations per second.
 * @return Compact metric string.
 */
[[nodiscard]] std::string format_ops_per_second(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << (value / 1'000'000.0) << "M";
    return output.str();
}

/**
 * @brief Formats a latency sample in microseconds per operation.
 *
 * @param ns_per_op Nanoseconds per operation.
 * @return Compact latency string.
 */
[[nodiscard]] std::string format_latency_us(double ns_per_op) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << (ns_per_op / 1'000.0) << " us";
    return output.str();
}

/**
 * @brief Computes optimized-to-baseline throughput ratio.
 *
 * @param optimized Optimized ops/sec.
 * @param baseline Baseline ops/sec.
 * @return Improvement ratio, or zero when baseline did not finish.
 */
[[nodiscard]] double improvement_ratio(double optimized, double baseline) {
    if (baseline <= 0.0) {
        return 0.0;
    }

    return optimized / baseline;
}

/**
 * @brief Prints one benchmark result in readable tables.
 *
 * @param result Side-by-side result to print.
 * @param output Presentation output stream.
 */
void print_result(const ComparisonResult& result, std::ostream& output) {
    output << "\nThroughput\n"
           << std::left << std::setw(31) << "Workload" << std::right << std::setw(20)
           << "Optimized ops/sec" << std::setw(22) << "Std baseline ops/sec" << std::setw(12)
           << "Improvement" << '\n'
           << std::string(85, '-') << '\n';

    output << std::left << std::setw(31) << workload_name(result.workload) << std::right
           << std::setw(20) << format_ops_per_second(result.optimized.ops_per_second)
           << std::setw(22) << format_ops_per_second(result.baseline.ops_per_second);

    std::ostringstream improvement;
    improvement << std::fixed << std::setprecision(2)
                << improvement_ratio(result.optimized.ops_per_second,
                                     result.baseline.ops_per_second)
                << "x";
    output << std::setw(12) << improvement.str() << "\n\n";

    output << "Amortized 256-op batch latency, not true single-operation tail latency.\n"
           << std::left << std::setw(18) << "Engine" << std::right << std::setw(14) << "p50"
           << std::setw(14) << "p99" << std::setw(14) << "max" << '\n'
           << std::string(60, '-') << '\n';

    output << std::left << std::setw(18) << "Optimized" << std::right << std::setw(14)
           << format_latency_us(result.optimized.latency.p50_ns) << std::setw(14)
           << format_latency_us(result.optimized.latency.p99_ns) << std::setw(14)
           << format_latency_us(result.optimized.latency.max_ns) << '\n';
    output << std::left << std::setw(18) << "Std baseline" << std::right << std::setw(14)
           << format_latency_us(result.baseline.latency.p50_ns) << std::setw(14)
           << format_latency_us(result.baseline.latency.p99_ns) << std::setw(14)
           << format_latency_us(result.baseline.latency.max_ns) << "\n\n";

    output << "Why this workload differs:\n"
           << "  Optimized: " << optimized_advantage(result.workload) << '\n'
           << "  Std baseline: " << baseline_advantage(result.workload) << "\n\n"
           << "These are local exploratory numbers. Official benchmark claims should come from "
              "the pinned Linux/EC2 workflow described in Benchmarks.md.\n";
}

/**
 * @brief Prints the local benchmark mode warning and build guidance.
 *
 * @param output Presentation output stream.
 */
void print_local_benchmark_warning(std::ostream& output) {
    output << "LOCAL BENCHMARK MODE -- results are from this machine only.\n"
           << "These are not EC2 release benchmark claims.\n\n"
           << "For meaningful results, run a Release build with -O3 -DNDEBUG -march=native.\n";

#ifndef NDEBUG
    output << "WARNING: this binary appears to be a Debug build because NDEBUG is not defined.\n";
#endif

    output << "\nLocal run shape:\n"
           << "  throughput operations per engine: " << kThroughputOperationCount << '\n'
           << "  latency samples per engine: " << kLatencySampleCount << " batches\n"
           << "  latency warmup per engine: " << kLatencyWarmupBatches << " batches\n"
           << "  latency batch size: " << kLatencyBatchSize << " operations\n\n";
}

/**
 * @brief Prints the benchmark submenu.
 *
 * @param output Presentation output stream.
 */
void print_benchmark_menu(std::ostream& output) {
    output << "Benchmark comparison: optimized engine vs std baseline\n\n"
           << "1) Run Passive Insert\n"
           << "2) Run One-Level Crossing Match\n"
           << "3) Run Random Cancel\n"
           << "4) Run Unknown Cancel\n"
           << "5) Run Modify If Present\n"
           << "6) Run OrderBook True Mixed\n"
           << "7) Back\n\n"
           << "Select option: ";
    output.flush();
}

/**
 * @brief Maps a submenu selection to a workload.
 *
 * @param choice Trimmed user selection.
 * @param workload Output workload when the choice is a workload item.
 * @return True when a workload was selected.
 */
[[nodiscard]] bool parse_workload_choice(std::string_view choice, LocalWorkload& workload) {
    if (choice == "1") {
        workload = LocalWorkload::PassiveInsert;
        return true;
    }
    if (choice == "2") {
        workload = LocalWorkload::OneLevelCrossingMatch;
        return true;
    }
    if (choice == "3") {
        workload = LocalWorkload::RandomCancel;
        return true;
    }
    if (choice == "4") {
        workload = LocalWorkload::UnknownCancel;
        return true;
    }
    if (choice == "5") {
        workload = LocalWorkload::ModifyIfPresent;
        return true;
    }
    if (choice == "6") {
        workload = LocalWorkload::OrderBookTrueMixed;
        return true;
    }

    return false;
}

/**
 * @brief Runs one confirmed benchmark and pauses after printing results.
 *
 * @param workload Workload to run.
 * @param input User input stream.
 * @param output Presentation output stream.
 * @return True when the benchmark menu should continue.
 */
[[nodiscard]] bool run_confirmed_workload(LocalWorkload workload,
                                          std::istream& input,
                                          std::ostream& output) {
    if (!confirm_benchmark_run(input, output, workload_name(workload))) {
        return false;
    }

    output << "\nRunning " << workload_name(workload) << " locally...\n";
    output.flush();

    const auto result = run_comparison(workload);
    print_result(result, output);

    return pause_after_result(input, output);
}

} // namespace

/**
 * @brief Runs the local benchmark comparison submenu.
 */
void run_local_benchmark_comparison(std::istream& input, std::ostream& output) {
    print_local_benchmark_warning(output);

    while (true) {
        print_benchmark_menu(output);

        std::string selection;
        if (!std::getline(input, selection)) {
            output << '\n';
            return;
        }

        const std::string choice = uppercase_copy(trim(selection));
        output << '\n';

        if (is_quit_command(choice) || choice == "7" || choice == "BACK" || choice == "MENU") {
            output << "Returning to main menu.\n";
            return;
        }

        LocalWorkload workload{};
        if (parse_workload_choice(choice, workload)) {
            if (!run_confirmed_workload(workload, input, output)) {
                return;
            }
            continue;
        }

        output << "Unknown selection. Choose 1-7, or type q to return to the main menu.\n\n";
    }
}

} // namespace matching_engine
