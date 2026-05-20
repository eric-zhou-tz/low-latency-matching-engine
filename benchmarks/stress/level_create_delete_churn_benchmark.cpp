#include "book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace {

using matching_engine::CancelResult;
using matching_engine::Event;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::Price;
using matching_engine::Side;
using matching_engine::TimeInForce;

constexpr Price kStableBidBase = 900'000;
constexpr Price kStableAskBase = 2'100'000;
constexpr Price kBidChurnBase = 1'000'000;
constexpr Price kAskChurnBase = 1'500'000;
constexpr Price kPriceStride = 17;
constexpr std::size_t kStableLevelsPerSide = 64;
constexpr std::size_t kPriceSlotsPerSide = 131'071;
constexpr std::uint64_t kFirstChurnOrderId = 1'000'000;
constexpr std::uint64_t kFirstMatcherOrderId = 2'000'000'000;
constexpr std::uint32_t kDepthSeed = 0x1EAF'CAFE;

enum class ActionKind {
    Submit,
    Cancel
};

/**
 * @brief Direct OrderBook action used by the churn replay loop.
 */
struct Action {
    ActionKind kind{ActionKind::Submit};
    Order order{};
    matching_engine::OrderId cancel_id{};
};

/**
 * @brief Pre-generated level churn stream and metadata for one benchmark size.
 */
struct Workload {
    std::vector<Order> preload_orders;
    std::vector<Action> actions;
    std::size_t reserve_order_capacity{};
    std::size_t max_live_orders{};
    std::size_t churn_cycles{};
    std::size_t bid_levels{};
    std::size_t ask_levels{};
    std::size_t cancel_deleted_levels{};
    std::size_t match_deleted_levels{};
};

/**
 * @brief Builds a passive GTC order for direct book submission.
 *
 * @param order_id Unique order id.
 * @param side Book side for the order.
 * @param price Limit price.
 * @return Order ready for the OrderBook hot path.
 */
[[nodiscard]] Order make_gtc_order(std::uint64_t order_id, Side side, Price price) {
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = 1,
                 .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Builds a one-lot IOC order that intentionally removes a churn level.
 *
 * @param order_id Unique taker order id.
 * @param resting_side Side currently resting at the churn price.
 * @param price Limit price that crosses exactly the churned level.
 * @return Matching order that will not rest.
 */
[[nodiscard]] Order make_level_delete_matcher(std::uint64_t order_id,
                                              Side resting_side,
                                              Price price) {
    const auto taker_side = resting_side == Side::Buy ? Side::Sell : Side::Buy;
    return Order{.id = order_id,
                 .side = taker_side,
                 .price = price,
                 .quantity = 1,
                 .time_in_force = TimeInForce::ImmediateOrCancel};
}

/**
 * @brief Maps a cycle index onto a wide deterministic bid or ask churn price.
 *
 * @param cycle_index Sequential churn cycle index.
 * @param side Side whose level will be created.
 * @return Non-crossing price for a fresh temporary level.
 */
[[nodiscard]] Price churn_price(std::size_t cycle_index, Side side) {
    const auto slot = static_cast<Price>((cycle_index * kPriceStride) % kPriceSlotsPerSide);
    if (side == Side::Buy) {
        return kBidChurnBase + slot;
    }

    return kAskChurnBase + slot;
}

/**
 * @brief Seeds stable non-crossing liquidity away from the churn price ranges.
 *
 * @return Passive bid and ask levels that keep both sides of the book populated.
 */
[[nodiscard]] std::vector<Order> make_preload_orders() {
    std::vector<Order> orders;
    orders.reserve(kStableLevelsPerSide * 2);

    for (std::size_t index = 0; index < kStableLevelsPerSide; ++index) {
        // Stable bids sit below bid churn prices, so sell-side match deletes do not drain them.
        orders.push_back(make_gtc_order(static_cast<std::uint64_t>(index + 1),
                                        Side::Buy,
                                        kStableBidBase - static_cast<Price>(index)));

        // Stable asks sit above ask churn prices, so buy-side match deletes do not drain them.
        orders.push_back(make_gtc_order(static_cast<std::uint64_t>(index + 1 + kStableLevelsPerSide),
                                        Side::Sell,
                                        kStableAskBase + static_cast<Price>(index)));
    }

    return orders;
}

/**
 * @brief Appends one create/delete cycle with a tiny FIFO queue.
 *
 * @param workload Workload being generated.
 * @param side Side whose price level is churned.
 * @param price Price level to create and erase.
 * @param depth Number of resting orders to place at the level.
 * @param delete_by_match Whether the final order is removed through matching.
 * @param next_order_id Next unique passive order id.
 * @param next_matcher_id Next unique matching order id.
 */
void append_churn_cycle(Workload& workload,
                        Side side,
                        Price price,
                        std::size_t depth,
                        bool delete_by_match,
                        std::uint64_t& next_order_id,
                        std::uint64_t& next_matcher_id) {
    std::vector<std::uint64_t> resting_ids;
    resting_ids.reserve(depth);

    for (std::size_t index = 0; index < depth; ++index) {
        // Creating consecutive orders at one price builds the small FIFO queue under test.
        const auto order_id = next_order_id++;
        resting_ids.push_back(order_id);
        workload.actions.push_back(Action{.kind = ActionKind::Submit,
                                          .order = make_gtc_order(order_id, side, price)});
    }

    const auto cancel_count = delete_by_match ? depth - 1 : depth;
    for (std::size_t index = 0; index < cancel_count; ++index) {
        // Cancels shrink the temporary level; the final cancel path erases the map node.
        workload.actions.push_back(Action{.kind = ActionKind::Cancel,
                                          .cancel_id = resting_ids[index]});
    }

    if (delete_by_match) {
        // The final one-lot taker removes the last resting order and triggers empty-level cleanup.
        workload.actions.push_back(Action{.kind = ActionKind::Submit,
                                          .order = make_level_delete_matcher(
                                              next_matcher_id++, side, price)});
    }
}

/**
 * @brief Chooses a reserve hint from the modeled peak live order count.
 *
 * @param max_live_orders Highest live depth reached by preload plus churn levels.
 * @return Reserve hint used for the order-id index and order pool.
 */
[[nodiscard]] std::size_t reserve_order_capacity(std::size_t max_live_orders) {
    // Existing hot-path benchmarks reserve for expected live orders, not total
    // operations, so transient churn does not over-size the order pool.
    return std::max<std::size_t>(1024, max_live_orders);
}

/**
 * @brief Generates an interleaved bid/ask level create-delete workload.
 *
 * @param operation_count Number of timed OrderBook calls to generate.
 * @return Deterministic workload stream and benchmark metadata.
 */
[[nodiscard]] Workload make_workload(std::size_t operation_count) {
    Workload workload;
    workload.preload_orders = make_preload_orders();
    workload.actions.reserve(operation_count);

    std::mt19937 rng{kDepthSeed};
    std::uniform_int_distribution<int> depth_distribution{1, 3};
    std::uint64_t next_order_id = kFirstChurnOrderId;
    std::uint64_t next_matcher_id = kFirstMatcherOrderId;
    std::size_t cycle_index = 0;
    std::size_t live_orders = workload.preload_orders.size();

    while (workload.actions.size() + 2 <= operation_count) {
        const auto remaining = operation_count - workload.actions.size();
        const auto max_depth_for_remaining = std::min<std::size_t>(3, remaining / 2);
        const auto random_depth = static_cast<std::size_t>(depth_distribution(rng));
        const auto depth = std::max<std::size_t>(1, std::min(random_depth, max_depth_for_remaining));
        const auto side = (cycle_index % 2 == 0) ? Side::Buy : Side::Sell;
        const auto price = churn_price(cycle_index, side);
        const auto delete_by_match = (cycle_index % 4) >= 2;

        // Each cycle completes the temporary level before the next price is touched.
        append_churn_cycle(workload,
                           side,
                           price,
                           depth,
                           delete_by_match,
                           next_order_id,
                           next_matcher_id);

        live_orders += depth;
        workload.max_live_orders = std::max(workload.max_live_orders, live_orders);
        live_orders -= depth;

        ++workload.churn_cycles;
        workload.bid_levels += side == Side::Buy ? 1 : 0;
        workload.ask_levels += side == Side::Sell ? 1 : 0;
        workload.cancel_deleted_levels += delete_by_match ? 0 : 1;
        workload.match_deleted_levels += delete_by_match ? 1 : 0;
        ++cycle_index;
    }

    workload.max_live_orders = std::max(workload.max_live_orders, workload.preload_orders.size());
    workload.reserve_order_capacity = reserve_order_capacity(workload.max_live_orders);

    return workload;
}

/**
 * @brief Preloads stable liquidity outside the measured loop.
 *
 * @param book Book to seed.
 * @param preload_orders Stable orders that should survive the timed workload.
 * @param events Caller-owned event buffer reused for all submissions.
 */
void preload_book(OrderBook& book,
                  const std::vector<Order>& preload_orders,
                  std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload establishes guard liquidity but is excluded from benchmark timing.
        book.submit(order, events);
    }
}

/**
 * @brief Executes one pre-generated OrderBook action.
 *
 * @param book Book under test.
 * @param action Action to replay.
 * @param events Caller-owned event buffer reused by submit actions.
 * @return Cancel result when the action is a cancel.
 */
[[nodiscard]] std::optional<CancelResult> run_action(OrderBook& book,
                                                     const Action& action,
                                                     std::vector<Event>& events) {
    if (action.kind == ActionKind::Cancel) {
        return book.cancel(action.cancel_id);
    }

    book.submit(action.order, events);
    return std::nullopt;
}

/**
 * @brief Measures repeated creation and deletion of whole price levels.
 */
void BM_Stress_LevelCreateDeleteChurn_Throughput(benchmark::State& state) {
    const auto operation_count = static_cast<std::size_t>(state.range(0));
    const auto workload = make_workload(operation_count);
    std::optional<OrderBook> book;
    std::vector<Event> events;
    events.reserve(8);

    for (auto _ : state) {
        state.PauseTiming();
        book.emplace();
        book->reserve_order_capacity(workload.reserve_order_capacity);
        preload_book(*book, workload.preload_orders, events);
        state.ResumeTiming();

        for (const auto& action : workload.actions) {
            auto cancel_result = run_action(*book, action, events);
            benchmark::DoNotOptimize(cancel_result);
            benchmark::DoNotOptimize(events.data());
            benchmark::DoNotOptimize(events.size());
        }

        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(*book);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(workload.actions.size()));
    state.counters["preload_orders"] = static_cast<double>(workload.preload_orders.size());
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
    state.counters["churn_cycles"] = static_cast<double>(workload.churn_cycles);
    state.counters["bid_levels"] = static_cast<double>(workload.bid_levels);
    state.counters["ask_levels"] = static_cast<double>(workload.ask_levels);
    state.counters["cancel_deleted_levels"] =
        static_cast<double>(workload.cancel_deleted_levels);
    state.counters["match_deleted_levels"] = static_cast<double>(workload.match_deleted_levels);
}

BENCHMARK(BM_Stress_LevelCreateDeleteChurn_Throughput)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Arg(1'000'000);

} // namespace
