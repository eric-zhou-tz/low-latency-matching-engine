#include "core/event.hpp"
#include "exchange.hpp"
#include "io/parser.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBaseQuantity = 10;

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
 * @brief Adds a replay cycle covering supported public command types.
 */
void add_replay_cycle(std::vector<std::string>& commands,
                      std::uint64_t& next_id,
                      std::size_t cycle_index) {
    const std::string symbol =
        (cycle_index % 3 == 0) ? "AAPL" : (cycle_index % 3 == 1) ? "MSFT" : "NVDA";
    const auto price_shift = static_cast<std::int64_t>(cycle_index % 7);
    const auto bid_id = next_id++;
    const auto ask_id = next_id++;
    const auto modify_id = next_id++;
    const auto cancel_id = next_id++;
    const auto sweep_id = next_id++;
    const auto market_id = next_id++;
    const auto ioc_id = next_id++;
    const auto fok_success_id = next_id++;
    const auto fok_reject_id = next_id++;

    // Seed both sides so crosses, market orders, IOC, and FOK have realistic liquidity.
    add_command(commands, submit_line(bid_id, symbol, "BUY", 99 - price_shift, 20));
    add_command(commands, submit_line(ask_id, symbol, "SELL", 101 + price_shift, 20));
    add_command(commands, submit_line(modify_id, symbol, "BUY", 98 - price_shift, 12));
    add_command(commands, modify_line(modify_id, 97 - price_shift, 15));
    add_command(commands, submit_line(cancel_id, symbol, "SELL", 103 + price_shift, 10));
    add_command(commands, cancel_line(cancel_id));

    // Aggressive flow creates trades while still leaving later commands deterministic.
    add_command(commands, submit_line(sweep_id, symbol, "BUY", 105 + price_shift, 5));
    add_command(commands, market_line(market_id, symbol, "SELL", 4));
    add_command(commands, submit_line(ioc_id, symbol, "BUY", 106 + price_shift, 4, "IOC"));
    add_command(commands, submit_line(fok_success_id, symbol, "SELL", 97 - price_shift, 3, "FOK"));
    add_command(
        commands, submit_line(fok_reject_id, symbol, "BUY", 106 + price_shift, 10'000, "FOK"));
}

/**
 * @brief Generates a realistic deterministic replay-style command stream.
 */
[[nodiscard]] std::vector<std::string> make_replay_script(std::int64_t count) {
    std::vector<std::string> commands;
    commands.reserve(static_cast<std::size_t>(count));

    std::uint64_t next_id = 1;
    std::size_t cycle_index = 0;
    while (commands.size() < static_cast<std::size_t>(count)) {
        // Each cycle touches inserts, crosses, cancels, modifies, market, IOC, FOK, and symbols.
        add_replay_cycle(commands, next_id, cycle_index);
        ++cycle_index;
    }

    commands.resize(static_cast<std::size_t>(count));
    return commands;
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
        // Parse each public command line exactly as the CLI boundary does.
        const auto action = parser.parse_line(command);
        if (!action) {
            formatted_output += "REJECTED invalid command\n";
            continue;
        }

        // Process the typed action and format every emitted event inside timing.
        exchange.process(*action, events);
        for (const auto& event : events) {
            formatted_output += matching_engine::format_event(event);
            formatted_output.push_back('\n');
        }
    }

    benchmark::DoNotOptimize(formatted_output.data());
    benchmark::DoNotOptimize(formatted_output.size());
    benchmark::ClobberMemory();
}

/**
 * @brief Measures full parser/exchange/order-book/formatter throughput.
 */
void BM_EndToEnd_ParseProcessFormat(benchmark::State& state) {
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
 * @brief Measures full-pipeline throughput for replay-style mixed traffic.
 */
void BM_EndToEnd_ReplayScenario(benchmark::State& state) {
    const auto command_count = state.range(0);
    // Reserve capacity is sized to expected peak live/resting orders, not total
    // processed operations. Submit/cancel/modify-heavy workloads usually have
    // far fewer concurrent live orders than total messages, so mixed and
    // end-to-end use 10% as the current benchmark-tuned proxy.
    const auto reserve_order_capacity =
        std::max<std::size_t>(1024, static_cast<std::size_t>(command_count) / 10);
    const auto commands = make_replay_script(command_count);

    for (auto _ : state) {
        state.PauseTiming();
        matching_engine::Parser parser;
        matching_engine::Exchange exchange{reserve_order_capacity};
        std::vector<matching_engine::Event> events;
        std::string formatted_output;
        events.reserve(8);
        formatted_output.reserve(commands.size() * 128);
        state.ResumeTiming();

        // A fresh exchange per replay keeps book state deterministic across benchmark iterations.
        run_end_to_end_script(commands, parser, exchange, events, formatted_output);
    }

    state.SetItemsProcessed(state.iterations() * command_count);
}

BENCHMARK(BM_EndToEnd_ParseProcessFormat)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_EndToEnd_ReplayScenario)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
