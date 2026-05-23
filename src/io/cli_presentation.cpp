#include "io/cli_presentation.hpp"

#include "core/action.hpp"
#include "core/event.hpp"
#include "exchange.hpp"
#include "io/local_benchmark_comparison.hpp"
#include "io/parser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace matching_engine {
namespace {

constexpr auto trace_line_delay = std::chrono::milliseconds{20};

/**
 * @brief One guided presentation step in the demo.
 */
struct DemoStep {
    std::string title;
    std::string explanation;
    std::vector<std::string> commands;
    std::string expected;
};

/**
 * @brief Presentation copy of a single visible resting order.
 */
struct VisibleOrder {
    std::string symbol;
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
};

/**
 * @brief Presentation counters derived from live book snapshots.
 */
struct BookTraceCounts {
    std::size_t symbols{};
    std::size_t resting_orders{};
    std::size_t bid_levels{};
    std::size_t ask_levels{};
};

/**
 * @brief Local benchmark executable and arguments shown in the CLI runner.
 */
struct LocalBenchmarkSuite {
    std::string_view title;
    std::string_view executable_name;
    std::vector<std::string_view> arguments;
    std::string_view description;
};

/**
 * @brief Returns a trimmed copy of a command-line input string.
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

    // Keep the user's command spelling intact after removing outer whitespace.
    return std::string(first, last);
}

/**
 * @brief Converts menu and control commands to uppercase for comparison.
 */
[[nodiscard]] std::string uppercase_copy(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });

    return result;
}

/**
 * @brief Prints visual separation without relying on terminal escape support.
 */
void clear_screen(std::ostream& output) {
    // Newlines are portable and keep the demo readable in redirected terminals.
    for (int line = 0; line < 40; ++line) {
        output << '\n';
    }
}

/**
 * @brief Waits for one Enter press from the user.
 */
bool wait_for_enter(std::istream& input, std::ostream& output, std::string_view prompt) {
    output << prompt;
    output.flush();

    std::string ignored;
    return static_cast<bool>(std::getline(input, ignored));
}

/**
 * @brief Waits for demo navigation input and detects a request to return to the menu.
 */
bool should_continue_demo(std::istream& input, std::ostream& output, std::string_view prompt) {
    output << prompt;
    output.flush();

    std::string response;
    if (!std::getline(input, response)) {
        return false;
    }

    const std::string control = uppercase_copy(trim(response));
    if (control == "Q" || control == "QUIT" || control == "EXIT" || control == "MENU") {
        output << "\nReturning to main menu.\n";
        return false;
    }

    // Any other response behaves like Enter so the demo stays easy to drive.
    return true;
}

/**
 * @brief Prints the top-level menu shown when the binary launches.
 */
void print_main_menu(std::ostream& output) {
    output << "============================================================\n"
           << "        Low-Latency Matching Engine\n"
           << "============================================================\n\n"
           << "1) Interactive guided demo\n"
           << "2) Benchmark comparison: optimized engine vs std baseline\n"
           << "3) Run full local benchmark suite\n"
           << "4) Manual command mode\n"
           << "5) Replay commands from file\n"
           << "6) Help\n"
           << "7) Exit\n\n"
           << "Select option: ";
    output.flush();
}

/**
 * @brief Prints the shared command syntax help.
 */
void print_help(std::ostream& output) {
    output << "Supported commands:\n\n"
           << "SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity> [GTC|IOC|FOK]\n"
           << "  Submit a limit order.\n"
           << "  Example: SUBMIT 1001 AAPL BUY 100 50 GTC\n\n"
           << "MARKET <id> <symbol> <BUY|SELL> <quantity>\n"
           << "  Submit a market order.\n"
           << "  Example: MARKET 2001 AAPL BUY 250\n\n"
           << "CANCEL <id>\n"
           << "  Cancel a resting order.\n"
           << "  Example: CANCEL 1001\n\n"
           << "MODIFY <id> <new_price> <new_quantity>\n"
           << "  Modify price and quantity of a resting order.\n"
           << "  Example: MODIFY 1001 101 25\n\n"
           << "PRINT\n"
           << "  Print all visible order books.\n\n"
           << "HELP\n"
           << "  Print this help menu.\n\n"
           << "EXIT\n"
           << "  In manual command mode, return to the main menu.\n"
           << "  From the main menu, use option 7 to exit the program.\n";
}

/**
 * @brief Returns all benchmark suites that can be launched from the CLI.
 */
[[nodiscard]] std::vector<LocalBenchmarkSuite> local_benchmark_suites() {
    return {
        LocalBenchmarkSuite{
            .title = "Core hot-path throughput",
            .executable_name = "core_hot_path_benchmark",
            .arguments = {"--benchmark_repetitions=1"},
            .description = "Passive insert, crossing match, and cancel lookup Google Benchmarks.",
        },
        LocalBenchmarkSuite{
            .title = "Core hot-path batch latency",
            .executable_name = "core_hot_path_latency_benchmark",
            .arguments = {"--output-dir=local-benchmark-results",
                          "--samples=128",
                          "--warmup=32",
                          "--trials=1"},
            .description = "Local fixed-batch latency runner for optimized OrderBook paths.",
        },
        LocalBenchmarkSuite{
            .title = "Realistic flow throughput",
            .executable_name = "realistic_flow_benchmark",
            .arguments = {"--benchmark_repetitions=1"},
            .description = "True mixed flow and end-to-end parse/process/format benchmarks.",
        },
        LocalBenchmarkSuite{
            .title = "Stress throughput",
            .executable_name = "stress_benchmark",
            .arguments = {"--benchmark_repetitions=1"},
            .description = "Best-level churn, price-level churn, and mixed GTC stress workloads.",
        },
        LocalBenchmarkSuite{
            .title = "Determinism replay throughput",
            .executable_name = "determinism_replay_benchmark",
            .arguments = {"--benchmark_repetitions=1"},
            .description = "Golden replay command tapes through the public parser/exchange path.",
        },
        LocalBenchmarkSuite{
            .title = "Experimental reserve sweep",
            .executable_name = "experimental_reserve_sweep_benchmark",
            .arguments = {"--benchmark_repetitions=1"},
            .description = "Reserve-capacity sweep for cancel lookup behavior.",
        },
    };
}

/**
 * @brief Appends a directory once while preserving search order.
 */
void append_unique_directory(std::vector<std::filesystem::path>& directories,
                             const std::filesystem::path& directory) {
    if (directory.empty()) {
        return;
    }

    const auto normalized = std::filesystem::absolute(directory).lexically_normal();
    const auto already_present =
        std::find(directories.begin(), directories.end(), normalized) != directories.end();
    if (!already_present) {
        // Preserve the first useful location so Release outputs stay preferred.
        directories.push_back(normalized);
    }
}

/**
 * @brief Returns the directory implied by argv[0].
 */
[[nodiscard]] std::filesystem::path executable_directory(std::string_view executable_path) {
    if (executable_path.empty()) {
        return {};
    }

    const std::filesystem::path path{std::string{executable_path}};
    if (path.has_parent_path()) {
        // Relative argv[0] values like build/matching_engine should resolve from the cwd.
        return path.is_absolute() ? path.parent_path()
                                  : std::filesystem::current_path() / path.parent_path();
    }

    return std::filesystem::current_path();
}

/**
 * @brief Builds likely locations for benchmark executables.
 */
[[nodiscard]] std::vector<std::filesystem::path> benchmark_search_directories(
    std::string_view executable_path) {
    std::vector<std::filesystem::path> directories;
    const auto current = std::filesystem::current_path();

    append_unique_directory(directories, current / "build-release");
    append_unique_directory(directories, current / "build-linux");

#ifdef NDEBUG
    // Release launchers may be packaged beside benchmarks or built in a custom directory.
    append_unique_directory(directories, executable_directory(executable_path));
    append_unique_directory(directories, current);
    append_unique_directory(directories, current / "build");
#else
    // Debug launchers should not silently run Debug benchmark binaries when Release is absent.
    (void)executable_path;
#endif

    return directories;
}

/**
 * @brief Checks whether one directory contains every local benchmark executable.
 */
[[nodiscard]] bool has_complete_benchmark_suite(
    const std::filesystem::path& directory,
    const std::vector<LocalBenchmarkSuite>& suites) {
    if (directory.empty()) {
        return false;
    }

    for (const auto& suite : suites) {
        if (!std::filesystem::exists(directory / suite.executable_name)) {
            // A complete directory avoids mixing Debug and Release benchmark binaries.
            return false;
        }
    }

    return true;
}

/**
 * @brief Finds the preferred directory containing the full local benchmark suite.
 */
[[nodiscard]] std::filesystem::path find_benchmark_suite_directory(
    const std::vector<LocalBenchmarkSuite>& suites,
    std::string_view executable_path) {
    for (const auto& directory : benchmark_search_directories(executable_path)) {
        if (has_complete_benchmark_suite(directory, suites)) {
            // The first complete directory wins so local Release builds beat Debug builds.
            return directory;
        }
    }

    return {};
}

/**
 * @brief Returns the benchmark suites whose executables are not available.
 */
[[nodiscard]] std::vector<LocalBenchmarkSuite> missing_benchmark_suites(
    const std::vector<LocalBenchmarkSuite>& suites,
    std::string_view executable_path) {
    std::vector<LocalBenchmarkSuite> missing;

    for (const auto& suite : suites) {
        bool found = false;
        for (const auto& directory : benchmark_search_directories(executable_path)) {
            if (std::filesystem::exists(directory / suite.executable_name)) {
                // Report only executables that are absent from every searched location.
                found = true;
                break;
            }
        }

        if (!found) {
            // Missing suite names are printed together so the user sees the exact build gap.
            missing.push_back(suite);
        }
    }

    return missing;
}

/**
 * @brief Escapes one shell word for std::system.
 */
[[nodiscard]] std::string shell_quote(std::string_view value) {
    std::string quoted{"'"};
    for (const char character : value) {
        if (character == '\'') {
            // Close, escape the quote, and reopen so paths with quotes stay literal.
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

/**
 * @brief Builds the shell command used to launch one local benchmark suite.
 */
[[nodiscard]] std::string benchmark_command(const std::filesystem::path& executable,
                                            const LocalBenchmarkSuite& suite) {
    std::string command = shell_quote(executable.string());
    for (const auto argument : suite.arguments) {
        // Arguments are fixed by the program, but quoting keeps the command builder uniform.
        command += ' ';
        command += shell_quote(argument);
    }

    return command;
}

/**
 * @brief Prints guidance when the full local benchmark suite is not built.
 */
void print_missing_benchmark_guidance(std::ostream& output,
                                      const std::vector<LocalBenchmarkSuite>& missing,
                                      std::string_view executable_path) {
    output << "The full local Release benchmark suite is not built yet.\n\n"
           << "Missing executables:\n";

    for (const auto& suite : missing) {
        output << "  " << suite.executable_name << '\n';
    }

    output << "\nBuild the Release benchmark targets, then run this menu again:\n"
           << "  cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
           << "  cmake --build build-release\n\n"
           << "Searched:\n";

    for (const auto& directory : benchmark_search_directories(executable_path)) {
        // Printing search paths makes packaging mistakes quick to spot.
        output << "  " << directory << '\n';
    }
}

/**
 * @brief Prints the local benchmark runner preflight warning.
 */
void print_local_benchmark_runner_warning(std::ostream& output,
                                          const std::filesystem::path& suite_directory) {
    output << "FULL LOCAL BENCHMARK SUITE -- results are from this machine only.\n"
           << "Use this for quick checks, not release benchmark claims.\n\n"
           << "Benchmark binaries: " << suite_directory << "\n";

#ifndef NDEBUG
    output << "NOTE: this launcher is a Debug build, but Release benchmark binaries are preferred.\n";
#endif

    output << "\nBatch-latency artifacts are written to local-benchmark-results/.\n\n";
}

/**
 * @brief Confirms the full local benchmark suite before launching external processes.
 */
[[nodiscard]] bool confirm_full_local_benchmark_suite(std::istream& input, std::ostream& output) {
    output << "This runs every local benchmark executable in sequence.\n"
           << "It can take several minutes on a laptop.\n\n"
           << "Press Enter to start the full local suite, or type q to return to the main menu: ";
    output.flush();

    std::string response;
    if (!std::getline(input, response)) {
        return false;
    }

    const std::string command = uppercase_copy(trim(response));
    if (command == "Q" || command == "QUIT" || command == "EXIT" || command == "BACK") {
        output << "\nReturning to main menu.\n";
        return false;
    }

    return true;
}

/**
 * @brief Runs one benchmark process and reports whether it exited cleanly.
 */
bool run_local_benchmark_process(const LocalBenchmarkSuite& suite,
                                 const std::filesystem::path& suite_directory,
                                 std::ostream& output) {
    const auto executable = suite_directory / suite.executable_name;
    if (!std::filesystem::exists(executable)) {
        // A stale or partial build directory should fail the suite clearly.
        output << "\nCould not find " << executable << ".\n";
        return false;
    }

    const std::string command = benchmark_command(executable, suite);
    output << "\nRunning: " << command << "\n\n";
    output.flush();

    const int status = std::system(command.c_str());
    output << '\n';
    if (status == 0) {
        output << suite.title << " completed.\n";
        return true;
    }

    output << suite.title << " failed with process status " << status << ".\n";
    return false;
}

/**
 * @brief Pauses after the full local benchmark suite completes.
 */
void pause_after_full_local_benchmark_suite(std::istream& input, std::ostream& output) {
    wait_for_enter(input, output, "\nPress Enter to return to the main menu...");
    output << '\n';
}

/**
 * @brief Runs every standalone local benchmark suite in menu order.
 */
void run_all_local_benchmark_suites(const std::vector<LocalBenchmarkSuite>& suites,
                                    const std::filesystem::path& suite_directory,
                                    std::ostream& output) {
    bool all_passed = true;

    for (const auto& suite : suites) {
        all_passed = run_local_benchmark_process(suite, suite_directory, output) && all_passed;
    }

    output << "\nFull local benchmark suite "
           << (all_passed ? "completed." : "finished with at least one failed benchmark process.")
           << '\n';
}

/**
 * @brief Runs the full local benchmark executable suite.
 */
void run_local_benchmark_runner(std::istream& input,
                                std::ostream& output,
                                std::string_view executable_path) {
    const auto suites = local_benchmark_suites();
    const auto suite_directory = find_benchmark_suite_directory(suites, executable_path);
    const auto missing = suite_directory.empty() ? missing_benchmark_suites(suites, executable_path)
                                                : std::vector<LocalBenchmarkSuite>{};

    if (suite_directory.empty()) {
        print_missing_benchmark_guidance(output, missing, executable_path);
        wait_for_enter(input, output, "\nPress Enter to return to the main menu...");
        output << '\n';
        return;
    }

    print_local_benchmark_runner_warning(output, suite_directory);
    if (!confirm_full_local_benchmark_suite(input, output)) {
        return;
    }

    run_all_local_benchmark_suites(suites, suite_directory, output);
    pause_after_full_local_benchmark_suite(input, output);
}

/**
 * @brief Flattens the current exchange state into a search-friendly order list.
 */
[[nodiscard]] std::vector<VisibleOrder> visible_orders(const Exchange& exchange) {
    std::vector<VisibleOrder> orders;

    for (const auto& symbol_book : exchange.debug_snapshots()) {
        for (const auto& level : symbol_book.book.bids) {
            for (const auto& order : level.orders) {
                // Preserve the symbol so cancel and modify events can show useful context.
                orders.push_back(VisibleOrder{.symbol = symbol_book.symbol,
                                              .id = order.id,
                                              .side = order.side,
                                              .price = order.price,
                                              .quantity = order.quantity});
            }
        }

        for (const auto& level : symbol_book.book.asks) {
            for (const auto& order : level.orders) {
                // Ask-side orders share the same presentation fields as bids.
                orders.push_back(VisibleOrder{.symbol = symbol_book.symbol,
                                              .id = order.id,
                                              .side = order.side,
                                              .price = order.price,
                                              .quantity = order.quantity});
            }
        }
    }

    return orders;
}

/**
 * @brief Finds a visible order by id in a flattened snapshot.
 */
[[nodiscard]] const VisibleOrder* find_visible_order(const std::vector<VisibleOrder>& orders,
                                                     OrderId order_id) {
    const auto found = std::find_if(orders.begin(), orders.end(), [order_id](const auto& order) {
        return order.id == order_id;
    });

    return found == orders.end() ? nullptr : &*found;
}

/**
 * @brief Returns the concrete action name produced by the parser.
 */
[[nodiscard]] std::string action_type_name(const Action& action) {
    struct NameVisitor {
        /**
         * @brief Names a parsed limit submit.
         */
        [[nodiscard]] std::string operator()(const SubmitOrderAction&) const {
            return "SubmitOrderAction";
        }

        /**
         * @brief Names a parsed market submit.
         */
        [[nodiscard]] std::string operator()(const MarketOrderAction&) const {
            return "MarketOrderAction";
        }

        /**
         * @brief Names a parsed cancel.
         */
        [[nodiscard]] std::string operator()(const CancelOrderAction&) const {
            return "CancelOrderAction";
        }

        /**
         * @brief Names a parsed modify.
         */
        [[nodiscard]] std::string operator()(const ModifyOrderAction&) const {
            return "ModifyOrderAction";
        }

        /**
         * @brief Names a parsed book snapshot request.
         */
        [[nodiscard]] std::string operator()(const PrintBookAction&) const {
            return "PrintBookAction";
        }
    };

    return std::visit(NameVisitor{}, action);
}

/**
 * @brief Returns the symbol implied by a parsed action.
 */
[[nodiscard]] std::string action_symbol(const Action& action,
                                        const std::vector<VisibleOrder>& before_orders) {
    struct SymbolVisitor {
        const std::vector<VisibleOrder>& before_orders;

        /**
         * @brief Reads the symbol carried by a submit action.
         */
        [[nodiscard]] std::string operator()(const SubmitOrderAction& submit) const {
            return submit.symbol;
        }

        /**
         * @brief Reads the symbol carried by a market action.
         */
        [[nodiscard]] std::string operator()(const MarketOrderAction& market) const {
            return market.symbol;
        }

        /**
         * @brief Looks up the canceled order's previous symbol.
         */
        [[nodiscard]] std::string operator()(const CancelOrderAction& cancel) const {
            const VisibleOrder* order = find_visible_order(before_orders, cancel.order_id);
            return order == nullptr ? "" : order->symbol;
        }

        /**
         * @brief Looks up the modified order's previous symbol.
         */
        [[nodiscard]] std::string operator()(const ModifyOrderAction& modify) const {
            const VisibleOrder* order = find_visible_order(before_orders, modify.order_id);
            return order == nullptr ? "" : order->symbol;
        }

        /**
         * @brief Print commands are not tied to one symbol.
         */
        [[nodiscard]] std::string operator()(const PrintBookAction&) const {
            return "";
        }
    };

    return std::visit(SymbolVisitor{before_orders}, action);
}

/**
 * @brief Returns the side implied by an action that can generate trades.
 */
[[nodiscard]] Side action_side(const Action& action, const std::vector<VisibleOrder>& before_orders) {
    struct SideVisitor {
        const std::vector<VisibleOrder>& before_orders;

        /**
         * @brief Reads the side carried by a submit action.
         */
        [[nodiscard]] Side operator()(const SubmitOrderAction& submit) const {
            return submit.side;
        }

        /**
         * @brief Reads the side carried by a market action.
         */
        [[nodiscard]] Side operator()(const MarketOrderAction& market) const {
            return market.side;
        }

        /**
         * @brief Cancel actions do not trade, so the default is unused.
         */
        [[nodiscard]] Side operator()(const CancelOrderAction&) const {
            return Side::Buy;
        }

        /**
         * @brief Modified orders keep their original side.
         */
        [[nodiscard]] Side operator()(const ModifyOrderAction& modify) const {
            const VisibleOrder* order = find_visible_order(before_orders, modify.order_id);
            return order == nullptr ? Side::Buy : order->side;
        }

        /**
         * @brief Print actions do not trade, so the default is unused.
         */
        [[nodiscard]] Side operator()(const PrintBookAction&) const {
            return Side::Buy;
        }
    };

    return std::visit(SideVisitor{before_orders}, action);
}

/**
 * @brief Returns the submitted quantity for actions that can execute.
 */
[[nodiscard]] Quantity action_quantity(const Action& action,
                                       const std::vector<VisibleOrder>& before_orders) {
    struct QuantityVisitor {
        const std::vector<VisibleOrder>& before_orders;

        /**
         * @brief Reads limit-order quantity.
         */
        [[nodiscard]] Quantity operator()(const SubmitOrderAction& submit) const {
            return submit.quantity;
        }

        /**
         * @brief Reads market-order quantity.
         */
        [[nodiscard]] Quantity operator()(const MarketOrderAction& market) const {
            return market.quantity;
        }

        /**
         * @brief Reads the remaining quantity before a cancel.
         */
        [[nodiscard]] Quantity operator()(const CancelOrderAction& cancel) const {
            const VisibleOrder* order = find_visible_order(before_orders, cancel.order_id);
            return order == nullptr ? 0 : order->quantity;
        }

        /**
         * @brief Reads replacement quantity for a modify.
         */
        [[nodiscard]] Quantity operator()(const ModifyOrderAction& modify) const {
            return modify.new_quantity;
        }

        /**
         * @brief Print actions do not carry quantity.
         */
        [[nodiscard]] Quantity operator()(const PrintBookAction&) const {
            return 0;
        }
    };

    return std::visit(QuantityVisitor{before_orders}, action);
}

/**
 * @brief Returns the submitted price for limit-style actions.
 */
[[nodiscard]] Price action_price(const Action& action) {
    struct PriceVisitor {
        /**
         * @brief Reads the submitted limit price.
         */
        [[nodiscard]] Price operator()(const SubmitOrderAction& submit) const {
            return submit.price;
        }

        /**
         * @brief Market orders do not carry a limit price.
         */
        [[nodiscard]] Price operator()(const MarketOrderAction&) const {
            return 0;
        }

        /**
         * @brief Cancel actions do not carry a price.
         */
        [[nodiscard]] Price operator()(const CancelOrderAction&) const {
            return 0;
        }

        /**
         * @brief Reads the replacement limit price.
         */
        [[nodiscard]] Price operator()(const ModifyOrderAction& modify) const {
            return modify.new_price;
        }

        /**
         * @brief Print actions do not carry a price.
         */
        [[nodiscard]] Price operator()(const PrintBookAction&) const {
            return 0;
        }
    };

    return std::visit(PriceVisitor{}, action);
}

/**
 * @brief Returns the submitted time-in-force for limit orders.
 */
[[nodiscard]] TimeInForce action_time_in_force(const Action& action) {
    if (const auto* submit = std::get_if<SubmitOrderAction>(&action)) {
        // Only submit actions expose time-in-force; other actions use this as a harmless default.
        return submit->time_in_force;
    }

    return TimeInForce::GoodTilCancel;
}

/**
 * @brief Returns the id targeted or created by an action.
 */
[[nodiscard]] OrderId action_order_id(const Action& action) {
    struct IdVisitor {
        /**
         * @brief Reads the submitted order id.
         */
        [[nodiscard]] OrderId operator()(const SubmitOrderAction& submit) const {
            return submit.id;
        }

        /**
         * @brief Reads the market order id.
         */
        [[nodiscard]] OrderId operator()(const MarketOrderAction& market) const {
            return market.id;
        }

        /**
         * @brief Reads the canceled order id.
         */
        [[nodiscard]] OrderId operator()(const CancelOrderAction& cancel) const {
            return cancel.order_id;
        }

        /**
         * @brief Reads the modified order id.
         */
        [[nodiscard]] OrderId operator()(const ModifyOrderAction& modify) const {
            return modify.order_id;
        }

        /**
         * @brief Print actions do not carry an order id.
         */
        [[nodiscard]] OrderId operator()(const PrintBookAction&) const {
            return 0;
        }
    };

    return std::visit(IdVisitor{}, action);
}

/**
 * @brief Counts executed quantity for the incoming order represented by an action.
 */
[[nodiscard]] Quantity incoming_fill_quantity(const Action& action,
                                              const std::vector<Event>& events) {
    const OrderId incoming_id = action_order_id(action);
    Quantity filled_quantity = 0;

    for (const auto& event : events) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (trade != nullptr && trade->incoming_order_id == incoming_id) {
            // Sum only fills for the action currently being formatted.
            filled_quantity += trade->quantity;
        }
    }

    return filled_quantity;
}

/**
 * @brief Checks whether an accepted event was emitted.
 */
[[nodiscard]] bool has_acceptance(const std::vector<Event>& events) {
    return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return std::holds_alternative<AcceptedEvent>(event);
    });
}

/**
 * @brief Counts trade events emitted by a processed action.
 */
[[nodiscard]] std::size_t trade_count(const std::vector<Event>& events) {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [](const auto& event) {
            return std::holds_alternative<TradeEvent>(event);
        }));
}

/**
 * @brief Counts visible symbols, orders, and price levels after execution.
 */
[[nodiscard]] BookTraceCounts book_trace_counts(const Exchange& exchange) {
    BookTraceCounts counts;

    for (const auto& symbol_book : exchange.debug_snapshots()) {
        ++counts.symbols;
        counts.bid_levels += symbol_book.book.bids.size();
        counts.ask_levels += symbol_book.book.asks.size();

        for (const auto& level : symbol_book.book.bids) {
            // Count live bid orders from the structured snapshot, not cached CLI state.
            counts.resting_orders += level.orders.size();
        }

        for (const auto& level : symbol_book.book.asks) {
            // Count live ask orders from the structured snapshot, not cached CLI state.
            counts.resting_orders += level.orders.size();
        }
    }

    return counts;
}

/**
 * @brief Derives the user-facing lifecycle status from events and live state.
 */
[[nodiscard]] std::string lifecycle_status(const Action& action,
                                           const std::vector<Event>& events,
                                           const std::vector<VisibleOrder>& before_orders,
                                           const Exchange& exchange) {
    for (const auto& event : events) {
        if (std::holds_alternative<RejectedEvent>(event) && !has_acceptance(events)) {
            // Rejections before acceptance leave no order lifecycle to inspect.
            return "REJECTED";
        }
    }

    if (std::holds_alternative<CancelOrderAction>(action)) {
        return has_acceptance(events) ? "ACCEPTED" : "CANCELED";
    }

    const OrderId order_id = action_order_id(action);
    const auto after_orders = visible_orders(exchange);
    if (find_visible_order(after_orders, order_id) != nullptr) {
        // A live order after processing means the action left resting liquidity.
        return "RESTING";
    }

    const Quantity requested_quantity = action_quantity(action, before_orders);
    const Quantity filled_quantity = incoming_fill_quantity(action, events);

    if (std::holds_alternative<SubmitOrderAction>(action) ||
        std::holds_alternative<MarketOrderAction>(action) ||
        std::holds_alternative<ModifyOrderAction>(action)) {
        if (requested_quantity > 0 && filled_quantity >= requested_quantity) {
            return "FILLED";
        }

        if (has_acceptance(events) || filled_quantity > 0) {
            return filled_quantity > 0 ? "PARTIAL" : "EXPIRED";
        }
    }

    if (std::holds_alternative<PrintBookAction>(action)) {
        return "SNAPSHOT";
    }

    return "APPLIED";
}

/**
 * @brief Formats live parse, route, match, lifecycle, and snapshot trace lines.
 */
[[nodiscard]] std::vector<std::string> format_execution_trace(
    const Action& action,
    const std::vector<Event>& events,
    const std::vector<VisibleOrder>& before_orders,
    const Exchange& exchange) {
    std::vector<std::string> lines;
    const std::string symbol = action_symbol(action, before_orders);
    const BookTraceCounts counts = book_trace_counts(exchange);

    lines.push_back("parse: OK -> " + action_type_name(action));
    lines.push_back(symbol.empty() ? "route: symbol=(none)" : "route: symbol=" + symbol);

    std::ostringstream match_line;
    match_line << "match: trades=" << trade_count(events)
               << " filled_quantity=" << incoming_fill_quantity(action, events);
    lines.push_back(match_line.str());

    lines.push_back("lifecycle: status=" +
                    lifecycle_status(action, events, before_orders, exchange));

    std::ostringstream snapshot_line;
    snapshot_line << "book_snapshot: symbols=" << counts.symbols
                  << " resting_orders=" << counts.resting_orders
                  << " bid_levels=" << counts.bid_levels << " ask_levels=" << counts.ask_levels;
    lines.push_back(snapshot_line.str());

    return lines;
}

/**
 * @brief Prints trace lines with a consistent indentation.
 */
void print_trace_lines(const std::vector<std::string>& lines,
                       std::ostream& output,
                       std::string_view indent) {
    for (const auto& line : lines) {
        // Indentation keeps traces readable inside both manual and guided output.
        output << indent << line << '\n';
        output.flush();

        // A short pause makes each derived pipeline step visible without feeling sluggish.
        std::this_thread::sleep_for(trace_line_delay);
    }
}

/**
 * @brief Converts a rejection reason to stable presentation text.
 */
[[nodiscard]] std::string presentation_reject_reason(const RejectedEvent& rejected,
                                                     const Action& action) {
    if (const auto* submit = std::get_if<SubmitOrderAction>(&action);
        submit != nullptr && submit->time_in_force == TimeInForce::FillOrKill &&
        rejected.reason == RejectReason::InsufficientLiquidity) {
        return "FOK_NOT_FILLABLE";
    }

    switch (rejected.reason) {
    case RejectReason::DuplicateOrderId:
        return "DUPLICATE_ORDER_ID";
    case RejectReason::UnknownOrderId:
        return "UNKNOWN_ORDER_ID";
    case RejectReason::InsufficientLiquidity:
        return "INSUFFICIENT_LIQUIDITY";
    case RejectReason::InvalidOrder:
        return "INVALID_ORDER";
    }

    return "UNKNOWN_REJECTION";
}

/**
 * @brief Formats one trade with explicit buy and sell ids.
 */
[[nodiscard]] std::string format_trade_event(const TradeEvent& trade,
                                             const Action& action,
                                             const std::vector<VisibleOrder>& before_orders) {
    const Side incoming_side = action_side(action, before_orders);
    const OrderId buy_id =
        incoming_side == Side::Buy ? trade.incoming_order_id : trade.resting_order_id;
    const OrderId sell_id =
        incoming_side == Side::Sell ? trade.incoming_order_id : trade.resting_order_id;

    std::ostringstream output;
    output << "TRADE symbol=" << action_symbol(action, before_orders) << " buy_id=" << buy_id
           << " sell_id=" << sell_id << " price=" << trade.price
           << " quantity=" << trade.quantity;
    return output.str();
}

/**
 * @brief Formats an accepted limit or market action with a readable status.
 */
[[nodiscard]] std::string format_accepted_event(const AcceptedEvent& accepted,
                                                const Action& action,
                                                const std::vector<Event>& events,
                                                const std::vector<VisibleOrder>& before_orders) {
    const Quantity filled_quantity = incoming_fill_quantity(action, events);
    const Quantity requested_quantity = action_quantity(action, before_orders);
    const bool completely_filled = requested_quantity > 0 && filled_quantity >= requested_quantity;

    if (const auto* submit = std::get_if<SubmitOrderAction>(&action)) {
        std::string status = completely_filled ? "FILLED" : "RESTING";
        if (submit->time_in_force == TimeInForce::ImmediateOrCancel && !completely_filled) {
            // IOC leftovers expire immediately, so a partial IOC is not a resting order.
            status = filled_quantity == 0 ? "EXPIRED" : "PARTIAL";
        }

        std::ostringstream output;
        output << "ACCEPTED order_id=" << accepted.order_id << " symbol=" << submit->symbol
               << " side=" << to_string(submit->side) << " price=" << submit->price
               << " quantity=" << submit->quantity << " tif=" << to_string(submit->time_in_force)
               << " status=" << status;
        return output.str();
    }

    if (const auto* market = std::get_if<MarketOrderAction>(&action)) {
        const std::string status = completely_filled ? "FILLED" : "PARTIAL";

        std::ostringstream output;
        output << "ACCEPTED order_id=" << accepted.order_id << " symbol=" << market->symbol
               << " side=" << to_string(market->side) << " type=MARKET"
               << " quantity=" << market->quantity << " status=" << status;
        return output.str();
    }

    return "ACCEPTED order_id=" + std::to_string(accepted.order_id);
}

/**
 * @brief Formats a successful cancel with the pre-cancel quantity.
 */
[[nodiscard]] std::string format_canceled_event(const CanceledEvent& canceled,
                                                const std::vector<VisibleOrder>& before_orders) {
    const VisibleOrder* order = find_visible_order(before_orders, canceled.order_id);
    const Quantity remaining_quantity = order == nullptr ? 0 : order->quantity;

    std::ostringstream output;
    output << "CANCELED order_id=" << canceled.order_id
           << " remaining_quantity=" << remaining_quantity;
    return output.str();
}

/**
 * @brief Formats an in-place quantity reduction.
 */
[[nodiscard]] std::string format_modified_event(const ModifiedEvent& modified) {
    std::ostringstream output;
    output << "MODIFIED order_id=" << modified.order_id << " old_price=" << modified.old_price
           << " new_price=" << modified.new_price << " old_quantity=" << modified.old_quantity
           << " new_quantity=" << modified.new_quantity;
    return output.str();
}

/**
 * @brief Formats a cancel-replace modification as one presentation-level modify.
 */
[[nodiscard]] std::string format_replaced_event(const ReplacedEvent& replaced) {
    std::ostringstream output;
    output << "MODIFIED order_id=" << replaced.old_order_id << " old_price=" << replaced.old_price
           << " new_price=" << replaced.new_price << " old_quantity=" << replaced.old_quantity
           << " new_quantity=" << replaced.new_quantity;
    return output.str();
}

/**
 * @brief Formats a rejected action with quoted machine-readable reason text.
 */
[[nodiscard]] std::string format_rejected_event(const RejectedEvent& rejected,
                                                const Action& action) {
    std::ostringstream output;
    output << "REJECTED order_id=" << rejected.order_id << " reason=\""
           << presentation_reject_reason(rejected, action) << '"';
    return output.str();
}

/**
 * @brief Formats a synthetic expiry event for non-resting remainders.
 */
[[nodiscard]] std::string format_expired_event(OrderId order_id,
                                               std::string_view reason,
                                               Quantity remaining_quantity) {
    std::ostringstream output;
    output << "EXPIRED order_id=" << order_id << " reason=\"" << reason
           << "\" remaining_quantity=" << remaining_quantity;
    return output.str();
}

/**
 * @brief Builds presentation lines for one processed action.
 */
[[nodiscard]] std::vector<std::string> format_execution_events(
    const Action& action,
    const std::vector<Event>& events,
    const std::vector<VisibleOrder>& before_orders) {
    std::vector<std::string> lines;
    const Quantity filled_quantity = incoming_fill_quantity(action, events);
    const Quantity requested_quantity = action_quantity(action, before_orders);
    const bool accepted = has_acceptance(events);

    for (const auto& event : events) {
        if (const auto* modified = std::get_if<ModifiedEvent>(&event)) {
            // Show priority-preserving quantity reductions before any other action output.
            lines.push_back(format_modified_event(*modified));
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            // Present cancel-replace modify semantics as one user-facing modification.
            lines.push_back(format_replaced_event(*replaced));
        } else if (const auto* canceled = std::get_if<CanceledEvent>(&event)) {
            // Include the previous remaining quantity so cancel output is self-contained.
            lines.push_back(format_canceled_event(*canceled, before_orders));
        }
    }

    for (const auto& event : events) {
        if (const auto* trade = std::get_if<TradeEvent>(&event)) {
            // Trades are grouped before acceptance so execution details are prominent.
            lines.push_back(format_trade_event(*trade, action, before_orders));
        }
    }

    for (const auto& event : events) {
        if (const auto* accepted_event = std::get_if<AcceptedEvent>(&event)) {
            // Acceptance lines include derived status instead of exposing raw internal events.
            lines.push_back(format_accepted_event(*accepted_event, action, events, before_orders));
        }
    }

    for (const auto& event : events) {
        const auto* rejected = std::get_if<RejectedEvent>(&event);
        if (rejected == nullptr) {
            continue;
        }

        if (accepted && std::holds_alternative<MarketOrderAction>(action)) {
            const Quantity remainder =
                requested_quantity > filled_quantity ? requested_quantity - filled_quantity : 0;
            lines.push_back(
                format_expired_event(rejected->order_id, "MARKET_REMAINDER_CANCELED", remainder));
            continue;
        }

        // Pre-acceptance rejections, such as failed FOK checks, are real rejects.
        lines.push_back(format_rejected_event(*rejected, action));
    }

    if (const auto* submit = std::get_if<SubmitOrderAction>(&action);
        submit != nullptr && submit->time_in_force == TimeInForce::ImmediateOrCancel &&
        accepted && requested_quantity > filled_quantity) {
        // The core keeps IOC logic lean; the presentation layer names the expired remainder.
        lines.push_back(format_expired_event(submit->id,
                                             "IOC_REMAINDER_CANCELED",
                                             requested_quantity - filled_quantity));
    }

    return lines;
}

/**
 * @brief Formats one resting order for a fixed-width book column.
 */
[[nodiscard]] std::string format_book_order(const OrderBook::DebugOrder& order) {
    std::ostringstream output;
    output << "price=" << std::left << std::setw(4) << order.price << " qty=" << std::setw(4)
           << order.quantity << " id=" << order.id;
    return output.str();
}

/**
 * @brief Appends all side orders in price-level and FIFO order.
 */
[[nodiscard]] std::vector<std::string> flatten_book_side(
    const std::vector<OrderBook::DebugPriceLevel>& levels) {
    std::vector<std::string> orders;

    for (const auto& level : levels) {
        for (const auto& order : level.orders) {
            // Level order follows best price first; order vectors preserve FIFO within price.
            orders.push_back(format_book_order(order));
        }
    }

    return orders;
}

/**
 * @brief Prints all visible books in aligned bid/ask columns.
 */
void print_books(const Exchange& exchange, std::ostream& output) {
    const auto snapshots = exchange.debug_snapshots();
    bool printed_any_order = false;

    if (snapshots.empty()) {
        output << "No visible order books.\n";
        return;
    }

    for (const auto& symbol_book : snapshots) {
        const auto bids = flatten_book_side(symbol_book.book.bids);
        const auto asks = flatten_book_side(symbol_book.book.asks);
        printed_any_order = printed_any_order || !bids.empty() || !asks.empty();

        output << "Book: " << symbol_book.symbol << "\n\n"
               << std::left << std::setw(38) << "BIDS" << "ASKS\n"
               << "------------------------------------------------------------\n";

        const std::size_t row_count = std::max(bids.size(), asks.size());
        if (row_count == 0) {
            output << "(empty)\n";
        }

        for (std::size_t row = 0; row < row_count; ++row) {
            const std::string bid = row < bids.size() ? bids[row] : "";
            const std::string ask = row < asks.size() ? asks[row] : "";
            output << std::left << std::setw(38) << bid << ask << '\n';
        }

        output << '\n';
    }

    if (!printed_any_order) {
        // Make fully empty books explicit without hiding the symbols that were touched.
        output << "All visible books are empty.\n";
    }
}

/**
 * @brief Executes one parsed command and prints presentation-level output.
 */
bool execute_command_line(const std::string& line,
                          Parser& parser,
                          Exchange& exchange,
                          std::ostream& output,
                          bool print_book_for_print_action) {
    const auto action = parser.parse_line(line);
    if (!action) {
        output << "REJECTED invalid command\n";
        output << "Live execution trace:\n"
               << "  parse: ERROR -> invalid command\n";
        return false;
    }

    if (std::holds_alternative<PrintBookAction>(*action)) {
        if (print_book_for_print_action) {
            // PRINT is rendered from structured snapshots instead of raw internal strings.
            print_books(exchange, output);
        }
        return true;
    }

    const auto before_orders = visible_orders(exchange);
    std::vector<Event> events;
    exchange.process(*action, events);

    for (const auto& line_text : format_execution_events(*action, events, before_orders)) {
        output << line_text << '\n';
    }

    output << "Live execution trace:\n";
    print_trace_lines(format_execution_trace(*action, events, before_orders, exchange),
                      output,
                      "  ");

    return true;
}

/**
 * @brief Returns the deterministic guided demo script.
 */
[[nodiscard]] std::vector<DemoStep> make_demo_steps() {
    return {
        DemoStep{
            .title = "Build initial AAPL book",
            .explanation =
                "We start by adding resting limit orders on both sides of the AAPL book.",
            .commands = {"SUBMIT 1001 AAPL BUY 100 50 GTC",
                         "SUBMIT 1002 AAPL BUY 99 40 GTC",
                         "SUBMIT 1003 AAPL SELL 103 30 GTC",
                         "SUBMIT 1004 AAPL SELL 104 40 GTC",
                         "PRINT"},
            .expected = "The book should show two bid levels and two ask levels.",
        },
        DemoStep{
            .title = "Show price-time priority",
            .explanation = "Multiple buy orders are submitted at the same price. Earlier orders "
                           "should fill first when a sell order arrives.",
            .commands = {"SUBMIT 1005 AAPL BUY 100 25 GTC",
                         "SUBMIT 1006 AAPL BUY 100 35 GTC",
                         "PRINT"},
            .expected = "The 100 bid level should preserve FIFO order: 1001, then 1005, then 1006.",
        },
        DemoStep{
            .title = "Cross the spread",
            .explanation = "An incoming sell order crosses the best bid and should match using "
                           "price-time priority.",
            .commands = {"SUBMIT 2001 AAPL SELL 100 60 GTC", "PRINT"},
            .expected = "Order 1001 should fully fill for 50 shares.\n"
                        "Then order 1005 should partially fill for 10 shares.",
        },
        DemoStep{
            .title = "Partial fill with leftover resting",
            .explanation = "A buy order crosses the best ask but is larger than the available "
                           "quantity at that level.",
            .commands = {"SUBMIT 2002 AAPL BUY 103 50 GTC", "PRINT"},
            .expected = "The order should fill 30 shares against SELL 1003 at price 103. The "
                        "remaining BUY quantity of 20 should rest on the book at price 103.",
        },
        DemoStep{
            .title = "Modify an order into an aggressive order",
            .explanation =
                "A resting buy order is modified upward so it crosses the current best ask.",
            .commands = {"MODIFY 1002 104 40", "PRINT"},
            .expected = "The modified order should become aggressive and execute against available "
                        "sell liquidity at price 104 if present.",
        },
        DemoStep{
            .title = "IOC partial execution",
            .explanation = "An IOC sell limit at 100 should execute immediately against bids priced "
                           "100 or better, then cancel any unfilled remainder.",
            .commands = {"SUBMIT 3001 AAPL SELL 100 100 IOC", "PRINT"},
            .expected = "The IOC order should first hit the 103 bid from order 2002, then continue "
                        "into 100 bids. Any leftover quantity should be canceled and should not "
                        "rest on the book.",
        },
        DemoStep{
            .title = "FOK failure",
            .explanation = "A FOK order should execute only if the full requested quantity is "
                           "immediately available.",
            .commands = {"SUBMIT 3002 AAPL BUY 105 1000 FOK", "PRINT"},
            .expected = "If fewer than 1000 shares are available at price 105 or better, the order "
                        "should reject or cancel without mutating the book.",
        },
        DemoStep{
            .title = "Add liquidity for FOK success",
            .explanation = "We add enough sell liquidity so a later FOK buy can fully execute.",
            .commands = {"SUBMIT 4001 AAPL SELL 105 300 GTC",
                         "SUBMIT 4002 AAPL SELL 105 300 GTC",
                         "SUBMIT 4003 AAPL SELL 105 400 GTC",
                         "PRINT"},
            .expected = "The book should now contain 1000 shares of sell liquidity at price 105.",
        },
        DemoStep{
            .title = "FOK success",
            .explanation = "Now that enough liquidity exists, a FOK buy for exactly 1000 shares "
                           "should execute completely.",
            .commands = {"SUBMIT 3003 AAPL BUY 105 1000 FOK", "PRINT"},
            .expected =
                "The full 1000-share order should execute immediately. No part of the FOK order "
                "should rest.",
        },
        DemoStep{
            .title = "Market order sweep",
            .explanation = "A market buy consumes the best available asks across multiple price "
                           "levels.",
            .commands = {"SUBMIT 5001 AAPL SELL 106 100 GTC",
                         "SUBMIT 5002 AAPL SELL 107 100 GTC",
                         "SUBMIT 5003 AAPL SELL 108 100 GTC",
                         "MARKET 6001 AAPL BUY 250",
                         "PRINT"},
            .expected = "The market buy should consume 100 at 106, 100 at 107, and 50 at 108.",
        },
        DemoStep{
            .title = "Cancel resting liquidity",
            .explanation = "We add two resting buy orders and cancel one of them.",
            .commands = {"SUBMIT 7001 AAPL BUY 95 500 GTC",
                         "SUBMIT 7002 AAPL BUY 94 500 GTC",
                         "PRINT",
                         "CANCEL 7001",
                         "PRINT"},
            .expected = "Order 7001 should disappear from the book. Order 7002 should remain.",
        },
        DemoStep{
            .title = "Modify quantity downward",
            .explanation = "A resting order is modified to reduce its quantity while keeping the "
                           "same price.",
            .commands = {"MODIFY 7002 94 200", "PRINT"},
            .expected = "Order 7002 should remain at price 94 with quantity 200.",
        },
        DemoStep{
            .title = "Multi-symbol isolation",
            .explanation = "Orders in MSFT and AAPL should live in separate order books.",
            .commands = {"SUBMIT 8001 MSFT BUY 300 100 GTC",
                         "SUBMIT 8002 MSFT SELL 305 100 GTC",
                         "SUBMIT 8003 AAPL BUY 96 100 GTC",
                         "PRINT"},
            .expected = "The MSFT book should show its own bid/ask. The AAPL book should be "
                        "unaffected except for the new AAPL bid.",
        },
        DemoStep{
            .title = "Cross MSFT only",
            .explanation =
                "This order should trade in the MSFT book only and should not affect AAPL.",
            .commands = {"SUBMIT 8004 MSFT BUY 305 50 GTC", "PRINT"},
            .expected = "MSFT should execute a 50-share trade against order 8002. AAPL should "
                        "remain unchanged.",
        },
        DemoStep{
            .title = "Final AAPL market sweep",
            .explanation = "A large market sell consumes available AAPL bids until either the order "
                           "is filled or no bid liquidity remains.",
            .commands = {"MARKET 9001 AAPL SELL 1000", "PRINT"},
            .expected = "The market sell should sweep the AAPL bid side. If quantity remains after "
                        "all bids are consumed, the remainder should be canceled.",
        },
    };
}

/**
 * @brief Prints one boxed demo-step preflight screen.
 */
void print_demo_step_intro(const DemoStep& step,
                           std::size_t step_index,
                           std::size_t step_count,
                           std::ostream& output) {
    std::ostringstream title;
    title << " Step " << step_index << '/' << step_count << ": " << step.title;

    output << "+------------------------------------------------------------+\n"
           << '|' << std::left << std::setw(60) << title.str() << "|\n"
           << "+------------------------------------------------------------+\n\n"
           << step.explanation << "\n\n"
           << "Commands to execute:\n";

    for (const auto& command : step.commands) {
        // Showing exact command text makes the demo reproducible for reviewers.
        output << "  " << command << '\n';
    }

    output << "\nExpected:\n";
    std::istringstream expected_lines{step.expected};
    std::string line;
    while (std::getline(expected_lines, line)) {
        output << "  " << line << '\n';
    }

    output << '\n';
}

/**
 * @brief Runs the deterministic guided product demo.
 */
void run_guided_demo(std::istream& input, std::ostream& output) {
    Parser parser;
    Exchange exchange;
    const auto steps = make_demo_steps();

    for (std::size_t index = 0; index < steps.size(); ++index) {
        clear_screen(output);
        print_demo_step_intro(steps[index], index + 1, steps.size(), output);
        if (!should_continue_demo(input, output, "Press Enter to execute this step, or Q to return to the main menu...")) {
            return;
        }

        output << "\nExecuted events:\n";
        bool printed_event = false;
        std::vector<std::string> trace_lines;
        for (const auto& command : steps[index].commands) {
            const auto action = parser.parse_line(command);
            if (!action) {
                output << "  REJECTED invalid command\n";
                trace_lines.push_back("command: " + command);
                trace_lines.push_back("  parse: ERROR -> invalid command");
                printed_event = true;
                continue;
            }

            const auto before_orders = visible_orders(exchange);
            if (std::holds_alternative<PrintBookAction>(*action)) {
                // Demo steps print the final book once after all commands run.
                trace_lines.push_back("command: " + command);
                for (const auto& line :
                     format_execution_trace(*action, {}, before_orders, exchange)) {
                    trace_lines.push_back("  " + line);
                }
                continue;
            }

            std::vector<Event> events;
            exchange.process(*action, events);

            for (const auto& event_line : format_execution_events(*action, events, before_orders)) {
                output << "  " << event_line << '\n';
                printed_event = true;
            }

            trace_lines.push_back("command: " + command);
            for (const auto& line : format_execution_trace(*action, events, before_orders, exchange)) {
                trace_lines.push_back("  " + line);
            }
        }

        if (!printed_event) {
            output << "  (no execution events)\n";
        }

        output << "\nLive execution trace:\n";
        print_trace_lines(trace_lines, output, "  ");

        output << "\nCurrent book:\n\n";
        print_books(exchange, output);

        if (!should_continue_demo(input, output, "Press Enter for next step, or Q to return to the main menu...")) {
            return;
        }
    }

    output << "\nInteractive guided demo complete.\n";
    wait_for_enter(input, output, "Press Enter to return to the main menu...");
}

/**
 * @brief Runs persistent manual command mode until EXIT.
 */
void run_manual_command_mode(std::istream& input, std::ostream& output) {
    Parser parser;
    Exchange exchange;

    output << "Manual command mode.\n"
           << "Type HELP for syntax, PRINT to show books, EXIT to return to menu.\n\n";

    std::string line;
    while (true) {
        output << "> ";
        output.flush();

        if (!std::getline(input, line)) {
            return;
        }

        const std::string command = trim(line);
        if (command.empty()) {
            continue;
        }

        const std::string control = uppercase_copy(command);
        if (control == "EXIT") {
            output << "\nReturning to main menu.\n";
            return;
        }

        if (control == "HELP") {
            print_help(output);
            output << '\n';
            continue;
        }

        execute_command_line(command, parser, exchange, output, true);
        output << '\n';
    }
}

/**
 * @brief Shows a placeholder feature message and pauses for readability.
 */
void print_placeholder(std::istream& input, std::ostream& output, std::string_view message) {
    output << message << "\n\n";
    wait_for_enter(input, output, "Press Enter to return to the main menu...");
}

} // namespace

/**
 * @brief Runs the main interactive CLI presentation loop.
 */
void run_cli_presentation(std::istream& input,
                          std::ostream& output,
                          std::string_view executable_path) {
    while (true) {
        print_main_menu(output);

        std::string selection;
        if (!std::getline(input, selection)) {
            output << '\n';
            return;
        }

        const std::string choice = uppercase_copy(trim(selection));
        output << '\n';

        if (choice == "1") {
            run_guided_demo(input, output);
        } else if (choice == "2") {
            run_local_benchmark_comparison(input, output);
        } else if (choice == "3") {
            run_local_benchmark_runner(input, output, executable_path);
        } else if (choice == "4") {
            run_manual_command_mode(input, output);
        } else if (choice == "5") {
            print_placeholder(input, output, "Replay-from-file mode is not wired yet.");
        } else if (choice == "6" || choice == "HELP") {
            print_help(output);
            output << '\n';
            wait_for_enter(input, output, "Press Enter to return to the main menu...");
        } else if (choice == "7" || choice == "EXIT" || choice == "QUIT" || choice == "Q") {
            output << "Bye bye!\n";
            return;
        } else {
            output << "Invalid selection. Please choose 1 through 7.\n\n";
            wait_for_enter(input, output, "Press Enter to return to the main menu...");
        }

        output << "\n";
    }
}

} // namespace matching_engine
