#include "exchange.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using matching_engine::Action;
using matching_engine::CancelOrderAction;
using matching_engine::Event;
using matching_engine::Exchange;
using matching_engine::MarketOrderAction;
using matching_engine::ModifyOrderAction;
using matching_engine::Side;
using matching_engine::SubmitOrderAction;
using matching_engine::TimeInForce;

constexpr std::string_view kSymbol{"ME"};
constexpr std::int64_t kPassiveBid = 99;
constexpr std::int64_t kPassiveAsk = 101;
constexpr std::int64_t kRestingBid = 100;
constexpr std::int64_t kRestingAsk = 102;
constexpr std::int64_t kCrossingBuy = 105;
constexpr std::uint64_t kQuantity = 1;
constexpr std::uint64_t kModifiableQuantity = 2;
constexpr std::uint64_t kIncomingIdBase = 1'000'000'000;
constexpr std::uint64_t kMarketIdBase = 2'000'000'000;
constexpr std::uint64_t kUnknownOrderIdBase = 3'000'000'000;
constexpr std::size_t kDefaultSampleCount = 1'000'000;
constexpr std::size_t kDefaultWarmupOperations = 10'000;
constexpr std::size_t kDefaultTrialCount = 5;

struct Options {
    std::filesystem::path output_dir{"benchmarks/results"};
    std::size_t sample_count{kDefaultSampleCount};
    std::size_t warmup_operations{kDefaultWarmupOperations};
    std::size_t trial_count{kDefaultTrialCount};
};

struct Percentiles {
    double p50{};
    double p95{};
    double p99{};
    double p999{};
    double max{};
};

struct LatencyResult {
    std::string benchmark_name;
    std::size_t workload_size{};
    std::size_t sample_count{};
    std::size_t warmup_operations{};
    std::size_t trial{};
    double timer_overhead_ns{};
    Percentiles percentiles{};
};

/**
 * @brief Keeps a benchmarked value observable to the optimizer.
 *
 * @tparam T Value type to keep visible.
 * @param value Value produced by the measured workload.
 */
template <typename T>
void do_not_optimize(const T& value) {
    // Compiler barriers keep hot-path results live without adding benchmark framework calls.
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Prevents memory operations from crossing a measurement edge.
 */
void clobber_memory() {
    // The barrier keeps writes from the measured action before the ending timestamp.
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Reads the benchmark clock as nanoseconds.
 *
 * @return Monotonic timestamp in nanoseconds.
 */
[[nodiscard]] std::uint64_t read_clock_ns() {
    // Linux release runs use MONOTONIC_RAW so NTP adjustments do not affect samples.
#if defined(__linux__)
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

/**
 * @brief Reads a positive integer command-line option value.
 *
 * @param value Text following an option prefix.
 * @param fallback Value retained when parsing fails or yields zero.
 * @return Parsed positive value or the fallback.
 */
[[nodiscard]] std::size_t parse_positive_size(std::string_view value, std::size_t fallback) {
    char* end = nullptr;
    const std::string text{value};
    const auto parsed = std::strtoull(text.c_str(), &end, 10);

    // Invalid or zero values keep the existing option so accidental bad input is harmless.
    if (end == nullptr || *end != '\0' || parsed == 0) {
        return fallback;
    }

    return static_cast<std::size_t>(parsed);
}

/**
 * @brief Parses simple --name=value options for the runner.
 *
 * @param argc Argument count from main.
 * @param argv Argument values from main.
 * @return Runtime options for output and sample counts.
 */
[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        // The runner keeps options simple so EC2 shell scripts can pass values directly.
        const std::string_view arg{argv[index]};
        if (arg.starts_with("--output-dir=")) {
            options.output_dir = std::string(arg.substr(std::string_view{"--output-dir="}.size()));
        } else if (arg.starts_with("--samples=")) {
            options.sample_count =
                parse_positive_size(arg.substr(std::string_view{"--samples="}.size()), options.sample_count);
        } else if (arg.starts_with("--warmup=")) {
            options.warmup_operations =
                parse_positive_size(arg.substr(std::string_view{"--warmup="}.size()),
                                    options.warmup_operations);
        } else if (arg.starts_with("--trials=")) {
            options.trial_count =
                parse_positive_size(arg.substr(std::string_view{"--trials="}.size()), options.trial_count);
        }
    }

    return options;
}

/**
 * @brief Creates a GTC limit-order action.
 *
 * @param id Client order id.
 * @param side Buy or sell side.
 * @param price Limit price.
 * @param quantity Order quantity.
 * @return Submit action routed through Exchange.
 */
[[nodiscard]] Action make_limit_action(std::uint64_t id,
                                       Side side,
                                       std::int64_t price,
                                       std::uint64_t quantity) {
    // Construct actions before timing so symbol storage is not part of measured latency.
    return SubmitOrderAction{.id = id,
                             .symbol = std::string{kSymbol},
                             .side = side,
                             .price = price,
                             .quantity = quantity,
                             .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Creates a market-order action.
 *
 * @param id Client order id.
 * @param side Buy or sell side.
 * @param quantity Order quantity.
 * @return Market action routed through Exchange.
 */
[[nodiscard]] Action make_market_action(std::uint64_t id, Side side, std::uint64_t quantity) {
    // Market actions are prebuilt so timed samples include routing and matching only.
    return MarketOrderAction{.id = id,
                             .symbol = std::string{kSymbol},
                             .side = side,
                             .quantity = quantity};
}

/**
 * @brief Builds passive limit inserts that do not cross.
 *
 * @param count Number of actions to generate.
 * @return Precomputed insert actions.
 */
[[nodiscard]] std::vector<Action> make_passive_insert_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Alternating sides lets inserts rest without accidental matching.
        const bool is_buy = index % 2 == 0;
        const auto price_offset = static_cast<std::int64_t>(index % 5);
        actions.push_back(make_limit_action(static_cast<std::uint64_t>(index + 1),
                                            is_buy ? Side::Buy : Side::Sell,
                                            is_buy ? kPassiveBid - price_offset
                                                   : kPassiveAsk + price_offset,
                                            kQuantity));
    }

    return actions;
}

/**
 * @brief Builds aggressive limit buys that consume one resting ask each.
 *
 * @param count Number of actions to generate.
 * @return Precomputed crossing actions.
 */
[[nodiscard]] std::vector<Action> make_aggressive_match_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Incoming ids are disjoint from preloaded resting ids.
        actions.push_back(make_limit_action(kIncomingIdBase + static_cast<std::uint64_t>(index),
                                            Side::Buy,
                                            kCrossingBuy,
                                            kQuantity));
    }

    return actions;
}

/**
 * @brief Builds cancel actions for known live order ids.
 *
 * @param count Number of actions to generate.
 * @return Precomputed cancel actions.
 */
[[nodiscard]] std::vector<Action> make_known_cancel_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Cancel ids line up with the live orders created by the preload step.
        actions.push_back(CancelOrderAction{.order_id = static_cast<std::uint64_t>(index + 1)});
    }

    return actions;
}

/**
 * @brief Builds in-place modify actions for known live order ids.
 *
 * @param count Number of actions to generate.
 * @return Precomputed modify actions.
 */
[[nodiscard]] std::vector<Action> make_known_modify_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Quantity reduction preserves FIFO priority and avoids replacement matching.
        actions.push_back(ModifyOrderAction{.order_id = static_cast<std::uint64_t>(index + 1),
                                            .new_price = kRestingBid,
                                            .new_quantity = kQuantity});
    }

    return actions;
}

/**
 * @brief Builds market buys that consume one resting ask each.
 *
 * @param count Number of actions to generate.
 * @return Precomputed market-order actions.
 */
[[nodiscard]] std::vector<Action> make_market_order_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Market ids are separate from resting liquidity so the duplicate-id path is not measured.
        actions.push_back(
            make_market_action(kMarketIdBase + static_cast<std::uint64_t>(index), Side::Buy, kQuantity));
    }

    return actions;
}

/**
 * @brief Builds reject-path cancels for ids that never existed.
 *
 * @param count Number of actions to generate.
 * @return Precomputed rejected cancel actions.
 */
[[nodiscard]] std::vector<Action> make_unknown_cancel_actions(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Unknown ids exercise a clean exchange-level reject without changing book state.
        actions.push_back(
            CancelOrderAction{.order_id = kUnknownOrderIdBase + static_cast<std::uint64_t>(index)});
    }

    return actions;
}

/**
 * @brief Processes setup actions before timing begins.
 *
 * @param exchange Exchange to populate.
 * @param actions Actions used to create the benchmark state.
 * @param events Reusable caller-owned event buffer.
 */
void preload_exchange(Exchange& exchange, const std::vector<Action>& actions, std::vector<Event>& events) {
    for (const auto& action : actions) {
        // Preload establishes the book shape and live index outside measured samples.
        exchange.process(action, events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }
}

/**
 * @brief Builds same-price resting asks for match and market workloads.
 *
 * @param count Number of resting orders to create.
 * @return Precomputed setup actions.
 */
[[nodiscard]] std::vector<Action> make_resting_ask_preload(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // One resting ask is created for each future aggressive or market buy.
        actions.push_back(make_limit_action(static_cast<std::uint64_t>(index + 1),
                                            Side::Sell,
                                            kRestingAsk,
                                            kQuantity));
    }

    return actions;
}

/**
 * @brief Builds same-price resting buys for cancel workloads.
 *
 * @param count Number of resting orders to create.
 * @return Precomputed setup actions.
 */
[[nodiscard]] std::vector<Action> make_resting_buy_preload(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Same-price buys create a stable live-id set for cancel timing.
        actions.push_back(make_limit_action(static_cast<std::uint64_t>(index + 1),
                                            Side::Buy,
                                            kRestingBid,
                                            kQuantity));
    }

    return actions;
}

/**
 * @brief Builds modifiable resting buys for modify workloads.
 *
 * @param count Number of resting orders to create.
 * @return Precomputed setup actions.
 */
[[nodiscard]] std::vector<Action> make_modifiable_buy_preload(std::size_t count) {
    std::vector<Action> actions;
    actions.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        // Quantity two lets each timed modify reduce quantity while keeping the order live.
        actions.push_back(make_limit_action(static_cast<std::uint64_t>(index + 1),
                                            Side::Buy,
                                            kRestingBid,
                                            kModifiableQuantity));
    }

    return actions;
}

/**
 * @brief Computes one nearest-rank percentile from sorted samples.
 *
 * @param sorted_samples Samples sorted in ascending order.
 * @param percentile Percentile in the range [0, 1].
 * @return Selected sample value.
 */
[[nodiscard]] double percentile_value(const std::vector<double>& sorted_samples, double percentile) {
    // Nearest-rank indexing keeps percentile reporting consistent with the batch runner.
    const auto rank = std::ceil(percentile * static_cast<double>(sorted_samples.size()));
    const auto index = static_cast<std::size_t>(
        std::clamp(rank, 1.0, static_cast<double>(sorted_samples.size())) - 1.0);
    return sorted_samples[index];
}

/**
 * @brief Computes percentile summaries over latency samples.
 *
 * @param samples Single-action nanosecond samples.
 * @return p50, p95, p99, p999, and max values.
 */
[[nodiscard]] Percentiles summarize_samples(std::vector<double> samples) {
    // Sorting once gives deterministic percentile and max lookup for the recorded distribution.
    std::ranges::sort(samples);

    return Percentiles{.p50 = percentile_value(samples, 0.50),
                       .p95 = percentile_value(samples, 0.95),
                       .p99 = percentile_value(samples, 0.99),
                       .p999 = percentile_value(samples, 0.999),
                       .max = samples.back()};
}

/**
 * @brief Measures the timer read pair overhead used by the benchmark.
 *
 * @param sample_count Number of overhead samples to collect.
 * @return Median back-to-back clock-read cost in nanoseconds.
 */
[[nodiscard]] double measure_timer_overhead(std::size_t sample_count) {
    std::vector<double> samples(sample_count);

    for (std::size_t index = 0; index < sample_count; ++index) {
        // Back-to-back reads estimate the measurement floor without engine work.
        const auto start = read_clock_ns();
        const auto end = read_clock_ns();
        samples[index] = static_cast<double>(end - start);
    }

    return summarize_samples(std::move(samples)).p50;
}

/**
 * @brief Measures one Exchange::process action per recorded sample.
 *
 * @param exchange Exchange carrying any preloaded benchmark state.
 * @param actions Warmup actions followed by recorded sample actions.
 * @param options Runtime options controlling sample counts.
 * @return Percentile summary of single-action samples.
 */
[[nodiscard]] Percentiles measure_single_actions(Exchange& exchange,
                                                 const std::vector<Action>& actions,
                                                 const Options& options) {
    std::vector<Event> events;
    events.reserve(8);
    std::vector<double> samples(options.sample_count);

    for (std::size_t index = 0; index < options.warmup_operations; ++index) {
        // Warmup pays branch prediction, cache, and first-use effects before recording.
        exchange.process(actions[index], events);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }

    for (std::size_t sample = 0; sample < options.sample_count; ++sample) {
        const auto action_index = options.warmup_operations + sample;
        const auto start = read_clock_ns();
        exchange.process(actions[action_index], events);
        clobber_memory();
        const auto end = read_clock_ns();

        // Store by index so result collection never reallocates inside the loop.
        samples[sample] = static_cast<double>(end - start);
        do_not_optimize(events.data());
        do_not_optimize(events.size());
    }

    return summarize_samples(std::move(samples));
}

/**
 * @brief Runs one named single-order latency workload.
 *
 * @param benchmark_name Workload label for artifacts.
 * @param options Runtime options controlling sample counts.
 * @return Result row for one workload and trial.
 */
[[nodiscard]] Percentiles run_named_workload(std::string_view benchmark_name, const Options& options) {
    // Each workload owns a fresh exchange so destructive actions do not bleed across scenarios.
    const auto total_operations = options.sample_count + options.warmup_operations;
    Exchange exchange{total_operations};
    std::vector<Event> events;
    events.reserve(8);

    if (benchmark_name == "BM_Exchange_PassiveInsert_SingleOrderLatency") {
        return measure_single_actions(exchange, make_passive_insert_actions(total_operations), options);
    }

    if (benchmark_name == "BM_Exchange_AggressiveMatch_SingleOrderLatency") {
        preload_exchange(exchange, make_resting_ask_preload(total_operations), events);
        return measure_single_actions(exchange, make_aggressive_match_actions(total_operations), options);
    }

    if (benchmark_name == "BM_Exchange_KnownCancel_SingleOrderLatency") {
        preload_exchange(exchange, make_resting_buy_preload(total_operations), events);
        return measure_single_actions(exchange, make_known_cancel_actions(total_operations), options);
    }

    if (benchmark_name == "BM_Exchange_ModifyIfPresent_SingleOrderLatency") {
        preload_exchange(exchange, make_modifiable_buy_preload(total_operations), events);
        return measure_single_actions(exchange, make_known_modify_actions(total_operations), options);
    }

    if (benchmark_name == "BM_Exchange_MarketOrder_SingleOrderLatency") {
        preload_exchange(exchange, make_resting_ask_preload(total_operations), events);
        return measure_single_actions(exchange, make_market_order_actions(total_operations), options);
    }

    return measure_single_actions(exchange, make_unknown_cancel_actions(total_operations), options);
}

/**
 * @brief Collects all single-order latency rows for the configured run.
 *
 * @param options Runtime options controlling samples, warmup, and trials.
 * @return Deterministically ordered result rows.
 */
[[nodiscard]] std::vector<LatencyResult> run_all_latency_benchmarks(const Options& options) {
    constexpr std::string_view workloads[] = {
        "BM_Exchange_PassiveInsert_SingleOrderLatency",
        "BM_Exchange_AggressiveMatch_SingleOrderLatency",
        "BM_Exchange_KnownCancel_SingleOrderLatency",
        "BM_Exchange_ModifyIfPresent_SingleOrderLatency",
        "BM_Exchange_MarketOrder_SingleOrderLatency",
        "BM_Exchange_UnknownCancelReject_SingleOrderLatency"};
    std::vector<LatencyResult> results;
    results.reserve(std::size(workloads) * options.trial_count);

    for (std::size_t trial = 1; trial <= options.trial_count; ++trial) {
        // Timer overhead is recorded per trial so future docs can show the measurement floor.
        const auto timer_overhead_ns = measure_timer_overhead(options.sample_count);
        for (const auto workload : workloads) {
            // Workloads are run in a fixed order to make text and JSON artifacts comparable.
            auto percentiles = run_named_workload(workload, options);
            results.push_back(LatencyResult{.benchmark_name = std::string(workload),
                                            .workload_size = options.sample_count,
                                            .sample_count = options.sample_count,
                                            .warmup_operations = options.warmup_operations,
                                            .trial = trial,
                                            .timer_overhead_ns = timer_overhead_ns,
                                            .percentiles = percentiles});
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
    // The text artifact is meant for quick terminal inspection after the EC2 run.
    output << "Single-order latency results\n";
    output << "sample_count=" << options.sample_count << '\n';
    output << "warmup_operations=" << options.warmup_operations << '\n';
    output << "trials=" << options.trial_count << '\n';
    output << "unit=ns/action around one Exchange::process(action) call\n\n";
    output << std::left << std::setw(52) << "benchmark" << std::right << std::setw(12)
           << "samples" << std::setw(10) << "warmup" << std::setw(8) << "trial"
           << std::setw(14) << "timer_p50" << std::setw(14) << "p50" << std::setw(14)
           << "p95" << std::setw(14) << "p99" << std::setw(14) << "p999"
           << std::setw(14) << "max" << '\n';
    output << std::string(166, '-') << '\n';
    output << std::fixed << std::setprecision(2);

    for (const auto& result : results) {
        // Emit one row per workload/trial so noisy trial behavior remains visible.
        output << std::left << std::setw(52) << result.benchmark_name << std::right
               << std::setw(12) << result.sample_count << std::setw(10)
               << result.warmup_operations << std::setw(8) << result.trial << std::setw(14)
               << result.timer_overhead_ns << std::setw(14) << result.percentiles.p50
               << std::setw(14) << result.percentiles.p95 << std::setw(14)
               << result.percentiles.p99 << std::setw(14) << result.percentiles.p999
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
    // JSON keeps field names aligned with the SQLite benchmark_history columns.
    output << std::fixed << std::setprecision(2);
    output << "{\n";
    output << "  \"sample_count\": " << options.sample_count << ",\n";
    output << "  \"warmup_operations\": " << options.warmup_operations << ",\n";
    output << "  \"trials\": " << options.trial_count << ",\n";
    output << "  \"unit\": \"ns/action around one Exchange::process(action) call\",\n";
    output << "  \"results\": [\n";

    for (std::size_t index = 0; index < results.size(); ++index) {
        // Result rows are flat so import scripts can map fields directly.
        const auto& result = results[index];
        output << "    {\n";
        output << "      \"benchmark_name\": \"" << result.benchmark_name << "\",\n";
        output << "      \"workload_size\": " << result.workload_size << ",\n";
        output << "      \"sample_count\": " << result.sample_count << ",\n";
        output << "      \"warmup_operations\": " << result.warmup_operations << ",\n";
        output << "      \"trial\": " << result.trial << ",\n";
        output << "      \"timer_overhead_ns\": " << result.timer_overhead_ns << ",\n";
        output << "      \"p50_latency_ns\": " << result.percentiles.p50 << ",\n";
        output << "      \"p95_latency_ns\": " << result.percentiles.p95 << ",\n";
        output << "      \"p99_latency_ns\": " << result.percentiles.p99 << ",\n";
        output << "      \"p999_latency_ns\": " << result.percentiles.p999 << ",\n";
        output << "      \"max_latency_ns\": " << result.percentiles.max << "\n";
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
 * @brief Entry point for the standalone single-order latency runner.
 *
 * @param argc Argument count.
 * @param argv Argument values.
 * @return Process exit code.
 */
int latency_main(int argc, char** argv) {
    // Parse options first so CI and EC2 scripts can control sample counts.
    const auto options = parse_options(argc, argv);
    std::filesystem::create_directories(options.output_dir);

    // Run all configured scenarios and write both human and machine-readable artifacts.
    const auto results = run_all_latency_benchmarks(options);
    write_text_results(options.output_dir / "single_order_latency_results.txt", options, results);
    write_json_results(options.output_dir / "single_order_latency_results.json", options, results);

    std::cout << "Wrote " << (options.output_dir / "single_order_latency_results.txt") << '\n';
    std::cout << "Wrote " << (options.output_dir / "single_order_latency_results.json") << '\n';
    return 0;
}

} // namespace

/**
 * @brief Program entry point.
 */
int main(int argc, char** argv) {
    // Keep main small so the benchmark workflow stays testable above.
    return latency_main(argc, argv);
}
