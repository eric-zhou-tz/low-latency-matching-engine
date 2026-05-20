#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"
#include "true_mixed_workload.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kBaseQuantity = 10;
constexpr const char* kEndToEndMixedSymbol = "MIXED";
constexpr std::size_t kEndToEndLatencySampleCount = 1'024;
constexpr std::size_t kEndToEndLatencyWarmupBatches = 128;

struct Percentiles {
    double p50{};
    double p95{};
    double p99{};
    double max{};
};

struct BatchLatencyResult {
    Percentiles percentiles{};
    std::uint64_t measured_ns{};
};

/**
 * @brief Appends one command to the generated in-memory script.
 */
void add_command(std::vector<std::string>& commands, std::string command) {
    // Store complete CLI-style lines so parsing cost is part of the timed work.
    commands.push_back(std::move(command));
}

/**
 * @brief Builds a deterministic submit command line.
 */
[[nodiscard]] std::string submit_line(std::uint64_t id,
                                      const std::string& symbol,
                                      const char* side,
                                      std::int64_t price,
                                      std::uint64_t quantity,
                                      const char* time_in_force = "GTC") {
    // Keep formatting simple and stable so benchmark inputs are reproducible.
    return "SUBMIT " + std::to_string(id) + " " + symbol + " " + side + " " +
           std::to_string(price) + " " + std::to_string(quantity) + " " + time_in_force;
}

/**
 * @brief Builds a deterministic market command line.
 */
[[nodiscard]] std::string market_line(std::uint64_t id,
                                      const std::string& symbol,
                                      const char* side,
                                      std::uint64_t quantity) {
    // Market orders exercise the public parser and exchange path without resting remainders.
    return "MARKET " + std::to_string(id) + " " + symbol + " " + side + " " +
           std::to_string(quantity);
}

/**
 * @brief Converts an order side to the public command token.
 */
[[nodiscard]] const char* side_token(matching_engine::Side side) {
    // Command strings must match parser tokens so the full public path is exercised.
    return side == matching_engine::Side::Buy ? "BUY" : "SELL";
}

/**
 * @brief Converts a time-in-force policy to the public command token.
 */
[[nodiscard]] const char* time_in_force_token(matching_engine::TimeInForce time_in_force) {
    // Explicit tokens keep GTC, IOC, and FOK command generation deterministic.
    switch (time_in_force) {
    case matching_engine::TimeInForce::GoodTilCancel:
        return "GTC";
    case matching_engine::TimeInForce::ImmediateOrCancel:
        return "IOC";
    case matching_engine::TimeInForce::FillOrKill:
        return "FOK";
    }

    return "GTC";
}

/**
 * @brief Builds a deterministic cancel command line.
 */
[[nodiscard]] std::string cancel_line(std::uint64_t id) {
    // Cancels are routed by order id at the exchange boundary.
    return "CANCEL " + std::to_string(id);
}

/**
 * @brief Builds a deterministic modify command line.
 */
[[nodiscard]] std::string modify_line(std::uint64_t id,
                                      std::int64_t new_price,
                                      std::uint64_t new_quantity) {
    // Modifies exercise in-place and cancel-replace update paths through the parser.
    return "MODIFY " + std::to_string(id) + " " + std::to_string(new_price) + " " +
           std::to_string(new_quantity);
}

/**
 * @brief Converts one generated OrderBook order into a public submit command.
 */
[[nodiscard]] std::string submit_line(const matching_engine::Order& order,
                                      const std::string& symbol) {
    // The command preserves the generated price, quantity, side, id, and TIF.
    return submit_line(order.id,
                       symbol,
                       side_token(order.side),
                       order.price,
                       order.quantity,
                       time_in_force_token(order.time_in_force));
}

/**
 * @brief Converts one generated market order into a public market command.
 */
[[nodiscard]] std::string market_line(const matching_engine::Order& order,
                                      const std::string& symbol) {
    // Market commands intentionally omit price because the parser owns that boundary.
    return market_line(order.id, symbol, side_token(order.side), order.quantity);
}

/**
 * @brief Builds public preload commands from deterministic seed liquidity.
 */
[[nodiscard]] std::vector<std::string> make_end_to_end_mixed_preload_script(
    const std::vector<matching_engine::Order>& preload_orders) {
    std::vector<std::string> commands;
    commands.reserve(preload_orders.size());

    for (const auto& order : preload_orders) {
        // Preload orders establish the same active book state as the hot-path benchmark.
        add_command(commands, submit_line(order, kEndToEndMixedSymbol));
    }

    return commands;
}

/**
 * @brief Converts true mixed operations into public command lines.
 */
[[nodiscard]] std::vector<std::string> make_end_to_end_mixed_command_script(
    const std::vector<matching_engine::benchmark_workloads::TrueMixedOperation>& operations) {
    using matching_engine::benchmark_workloads::TrueMixedOperationKind;

    std::vector<std::string> commands;
    commands.reserve(operations.size());

    for (const auto& operation : operations) {
        if (operation.kind == TrueMixedOperationKind::Cancel) {
            // Cancels target ids that were live when the deterministic stream was generated.
            add_command(commands, cancel_line(operation.target_order_id));
        } else if (operation.kind == TrueMixedOperationKind::Modify) {
            // Modifies carry the generated replacement price and quantity through Parser.
            add_command(commands,
                        modify_line(operation.target_order_id,
                                    operation.new_price,
                                    operation.new_quantity));
        } else if (operation.kind == TrueMixedOperationKind::Market) {
            // Market orders are transient taker flow and never enter the live-id set.
            add_command(commands, market_line(operation.order, kEndToEndMixedSymbol));
        } else {
            // GTC, IOC, and FOK limit orders share the public SUBMIT command shape.
            add_command(commands, submit_line(operation.order, kEndToEndMixedSymbol));
        }
    }

    return commands;
}

/**
 * @brief Generates a compact parse/process/format script with deterministic traffic.
 */
[[nodiscard]] std::vector<std::string> make_parse_process_format_script(std::int64_t count) {
    std::vector<std::string> commands;
    commands.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        const auto id = static_cast<std::uint64_t>(index + 1);
        const std::string symbol = (index % 2 == 0) ? "AAPL" : "MSFT";
        const bool is_buy = index % 4 < 2;
        const auto price_offset = index % 5;
        const auto price = is_buy ? 99 - price_offset : 101 + price_offset;

        // Non-crossing prices keep this workload focused on public-boundary overhead.
        add_command(commands,
                    submit_line(id, symbol, is_buy ? "BUY" : "SELL", price, kBaseQuantity));
    }

    return commands;
}

/**
 * @brief Runs one pre-generated CLI line through parser, exchange, and formatter.
 */
void run_end_to_end_command(const std::string& command,
                            const matching_engine::Parser& parser,
                            matching_engine::Exchange& exchange,
                            std::vector<matching_engine::Event>& events,
                            std::string& formatted_output) {
    // Parse each public command line exactly as the CLI boundary does.
    const auto action = parser.parse_line(command);
    if (!action) {
        formatted_output += "REJECTED invalid command\n";
        return;
    }

    // Process the typed action and format every emitted event inside timing.
    exchange.process(*action, events);
    for (const auto& event : events) {
        formatted_output += matching_engine::format_event(event);
        formatted_output.push_back('\n');
    }

    benchmark::DoNotOptimize(events.data());
    benchmark::DoNotOptimize(events.size());
}

/**
 * @brief Runs pre-generated CLI lines through parser, exchange, and formatter.
 */
void run_end_to_end_script(const std::vector<std::string>& commands,
                           const matching_engine::Parser& parser,
                           matching_engine::Exchange& exchange,
                           std::vector<matching_engine::Event>& events,
                           std::string& formatted_output) {
    for (const auto& command : commands) {
        // Reuse the single-command path so all end-to-end workloads measure the same work.
        run_end_to_end_command(command, parser, exchange, events, formatted_output);
    }

    benchmark::DoNotOptimize(formatted_output.data());
    benchmark::DoNotOptimize(formatted_output.size());
    benchmark::ClobberMemory();
}

/**
 * @brief Computes one nearest-rank percentile from sorted samples.
 */
[[nodiscard]] double percentile_value(const std::vector<double>& sorted_samples, double percentile) {
    const auto rank = std::ceil(percentile * static_cast<double>(sorted_samples.size()));
    const auto index = static_cast<std::size_t>(
        std::clamp(rank, 1.0, static_cast<double>(sorted_samples.size())) - 1.0);
    return sorted_samples[index];
}

/**
 * @brief Computes amortized ns/op percentiles for fixed-size batch samples.
 */
[[nodiscard]] Percentiles summarize_samples(std::vector<double> samples) {
    std::ranges::sort(samples);

    // Nearest-rank percentiles match the standalone latency runner methodology.
    return Percentiles{.p50 = percentile_value(samples, 0.50),
                       .p95 = percentile_value(samples, 0.95),
                       .p99 = percentile_value(samples, 0.99),
                       .max = samples.back()};
}

/**
 * @brief Measures amortized end-to-end mixed-flow latency over fixed batches.
 */
[[nodiscard]] BatchLatencyResult measure_end_to_end_mixed_batches(
    const std::vector<std::string>& preload_commands,
    const std::vector<std::string>& commands,
    std::size_t reserve_order_capacity,
    std::size_t batch_size,
    std::size_t sample_count,
    std::size_t warmup_batches) {
    matching_engine::Parser parser;
    matching_engine::Exchange exchange{reserve_order_capacity};
    std::vector<matching_engine::Event> events;
    std::string formatted_output;
    std::vector<double> samples;
    std::size_t command_index = 0;
    std::uint64_t measured_ns = 0;

    // Reserve reusable buffers so timed samples focus on parser/exchange/book/format work.
    events.reserve(16);
    formatted_output.reserve(batch_size * 192);
    samples.reserve(sample_count);

    // Preload uses the public parser and exchange path, but is setup outside measured batches.
    run_end_to_end_script(preload_commands, parser, exchange, events, formatted_output);
    formatted_output.clear();

    for (std::size_t batch = 0; batch < warmup_batches; ++batch) {
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            // Warmup advances the deterministic stream without recording a sample.
            run_end_to_end_command(commands[command_index++],
                                   parser,
                                   exchange,
                                   events,
                                   formatted_output);
        }
        formatted_output.clear();
    }

    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            // Each recorded batch includes parse, exchange route, book mutation, and formatting.
            run_end_to_end_command(commands[command_index++],
                                   parser,
                                   exchange,
                                   events,
                                   formatted_output);
        }
        benchmark::DoNotOptimize(formatted_output.data());
        benchmark::DoNotOptimize(formatted_output.size());
        benchmark::ClobberMemory();
        const auto end = Clock::now();

        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        measured_ns += static_cast<std::uint64_t>(elapsed_ns);
        samples.push_back(static_cast<double>(elapsed_ns) / static_cast<double>(batch_size));
        formatted_output.clear();
    }

    return BatchLatencyResult{.percentiles = summarize_samples(std::move(samples)),
                              .measured_ns = measured_ns};
}

/**
 * @brief Measures full parser/exchange/order-book/formatter throughput.
 */
void BM_EndToEnd_PassiveInsert_Throughput(benchmark::State& state) {
    const auto command_count = state.range(0);
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Submit/cancel/modify-heavy workloads usually have
    // far fewer concurrent live orders than total messages, so mixed and
    // end-to-end use 10% as the current benchmark-tuned proxy.
    const auto reserve_order_capacity =
        std::max<std::size_t>(1024, static_cast<std::size_t>(command_count) / 10);
    const auto commands = make_parse_process_format_script(command_count);

    for (auto _ : state) {
        state.PauseTiming();
        matching_engine::Parser parser;
        matching_engine::Exchange exchange{reserve_order_capacity};
        std::vector<matching_engine::Event> events;
        std::string formatted_output;
        events.reserve(8);
        formatted_output.reserve(commands.size() * 96);
        state.ResumeTiming();

        // Script data is already in memory; timing covers parse, process, and format work.
        run_end_to_end_script(commands, parser, exchange, events, formatted_output);
    }

    state.SetItemsProcessed(state.iterations() * command_count);
}

/**
 * @brief Measures full-pipeline throughput for true mixed order flow.
 */
void BM_EndToEnd_TrueMixed_Throughput(benchmark::State& state) {
    const auto operation_count = static_cast<std::size_t>(state.range(0));
    const auto workload =
        matching_engine::benchmark_workloads::make_true_mixed_workload(operation_count);
    const auto preload_commands = make_end_to_end_mixed_preload_script(workload.preload_orders);
    const auto commands = make_end_to_end_mixed_command_script(workload.operations);

    for (auto _ : state) {
        state.PauseTiming();
        matching_engine::Parser parser;
        matching_engine::Exchange exchange{workload.reserve_order_capacity};
        std::vector<matching_engine::Event> events;
        std::string formatted_output;
        events.reserve(16);
        formatted_output.reserve(commands.size() * 192);

        // Seed active liquidity through the public path before measuring mixed commands.
        run_end_to_end_script(preload_commands, parser, exchange, events, formatted_output);
        formatted_output.clear();
        state.ResumeTiming();

        // The timed stream is already in memory and includes parser through formatter work.
        run_end_to_end_script(commands, parser, exchange, events, formatted_output);
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["preload_orders"] = static_cast<double>(workload.preload_orders.size());
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
}

/**
 * @brief Reports amortized end-to-end mixed-flow batch latency.
 */
void BM_EndToEnd_TrueMixed_BatchLatency(benchmark::State& state) {
    const auto batch_size = static_cast<std::size_t>(state.range(0));
    const auto operation_count =
        (kEndToEndLatencySampleCount + kEndToEndLatencyWarmupBatches) * batch_size;
    const auto workload =
        matching_engine::benchmark_workloads::make_true_mixed_workload(operation_count);
    const auto preload_commands = make_end_to_end_mixed_preload_script(workload.preload_orders);
    const auto commands = make_end_to_end_mixed_command_script(workload.operations);
    BatchLatencyResult result;

    // Label the row so benchmark output does not read like true single-operation latency.
    state.SetLabel("amortized end-to-end batch latency");

    for (auto _ : state) {
        // Manual timing records only sampled batches and excludes stream generation/preload setup.
        result = measure_end_to_end_mixed_batches(preload_commands,
                                                  commands,
                                                  workload.reserve_order_capacity,
                                                  batch_size,
                                                  kEndToEndLatencySampleCount,
                                                  kEndToEndLatencyWarmupBatches);
        state.SetIterationTime(static_cast<double>(result.measured_ns) / 1'000'000'000.0);
    }

    state.SetItemsProcessed(state.iterations() * kEndToEndLatencySampleCount *
                            static_cast<std::int64_t>(batch_size));
    state.counters["p50_ns_per_op"] = result.percentiles.p50;
    state.counters["p95_ns_per_op"] = result.percentiles.p95;
    state.counters["p99_ns_per_op"] = result.percentiles.p99;
    state.counters["max_ns_per_op"] = result.percentiles.max;
    state.counters["latency_samples"] = static_cast<double>(kEndToEndLatencySampleCount);
    state.counters["warmup_batches"] = static_cast<double>(kEndToEndLatencyWarmupBatches);
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
}

BENCHMARK(BM_EndToEnd_PassiveInsert_Throughput)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_EndToEnd_TrueMixed_Throughput)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_EndToEnd_TrueMixed_BatchLatency)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1'024)
    ->UseManualTime()
    ->Iterations(1);

} // namespace
