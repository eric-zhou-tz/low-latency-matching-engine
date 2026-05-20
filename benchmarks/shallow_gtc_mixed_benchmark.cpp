#include "book/order_book.hpp"
#include "core/event.hpp"
#include "core/order.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using matching_engine::CancelResult;
using matching_engine::Event;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::OrderId;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::RejectedEvent;
using matching_engine::RejectReason;
using matching_engine::Side;
using matching_engine::TimeInForce;

constexpr Price kMidPrice = 10'000;
constexpr Quantity kBaseQuantity = 100;
constexpr std::size_t kTargetLiveOrders = 512;
constexpr std::size_t kMinimumReserveOrderCapacity = 1'024;
constexpr std::uint32_t kSeed = 0x5A11'0B00U;
constexpr OrderId kPreloadIdBase = 20'000'000'000ULL;
constexpr OrderId kTimedIdBase = 30'000'000'000ULL;

/**
 * @brief Primary operation kind requested by the shallow GTC workload.
 */
enum class PrimaryKind {
    GtcLimit,
    Cancel,
    Modify,
    CrossingGtc
};

/**
 * @brief Direct OrderBook action emitted by the deterministic workload generator.
 */
enum class ActionKind {
    Submit,
    Cancel,
    Modify
};

/**
 * @brief One replayable OrderBook action with no parser or exchange state.
 */
struct Action {
    ActionKind kind{};
    Order order{};
    OrderId target_order_id{};
    Price new_price{};
    Quantity new_quantity{};
};

/**
 * @brief Full deterministic shallow-book workload and metadata.
 */
struct Workload {
    std::vector<Order> preload_orders;
    std::vector<Action> actions;
    std::size_t primary_operations{};
    std::size_t reserve_order_capacity{};
    std::size_t max_live_orders{};
};

/**
 * @brief Lightweight generation model for valid live ids and FIFO crossing fills.
 */
class ShallowBookModel {
public:
    /**
     * @brief Adds a modeled resting GTC order to all live lookup structures.
     *
     * @param order Passive order that should become cancel/modify eligible.
     */
    void add_order(const Order& order) {
        // Keep id selection, order metadata, and FIFO price levels in sync.
        live_by_id_.emplace(order.id, order);
        live_ids_.push_back(order.id);
        live_index_by_id_.emplace(order.id, live_ids_.size() - 1);

        if (order.side == Side::Buy) {
            bids_[order.price].push_back(order.id);
        } else {
            asks_[order.price].push_back(order.id);
        }
    }

    /**
     * @brief Returns the current modeled resting order count.
     */
    [[nodiscard]] std::size_t live_order_count() const {
        // The benchmark uses this to keep the book pinned near the target depth.
        return live_ids_.size();
    }

    /**
     * @brief Chooses a random live order id for cancel or modify generation.
     *
     * @param rng Fixed-seed RNG used for deterministic interleaving.
     * @return Live order id that exists in the modeled book.
     */
    [[nodiscard]] OrderId random_live_order_id(std::mt19937& rng) const {
        // Uniform id selection exercises random hash lookups instead of only FIFO fronts.
        std::uniform_int_distribution<std::size_t> distribution{0, live_ids_.size() - 1};
        return live_ids_[distribution(rng)];
    }

    /**
     * @brief Returns current modeled state for a live order.
     *
     * @param order_id Live order id to inspect.
     * @return Copy of the live order.
     */
    [[nodiscard]] Order live_order(OrderId order_id) const {
        // Modify and crossing generation need the latest modeled remaining quantity.
        return live_by_id_.at(order_id);
    }

    /**
     * @brief Finds the FIFO-front order on the opposite side for a crossing submit.
     *
     * @param incoming_side Side of the incoming order.
     * @return Best opposite resting order id, if any exists.
     */
    [[nodiscard]] std::optional<OrderId> best_opposite_order_id(Side incoming_side) const {
        // Crossing GTC flow consumes one cache-hot best-level order at a time.
        if (incoming_side == Side::Buy) {
            if (asks_.empty()) {
                return std::nullopt;
            }
            return asks_.begin()->second.front();
        }

        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->second.front();
    }

    /**
     * @brief Removes a modeled resting order from levels and live-id indexes.
     *
     * @param order_id Live order id to remove.
     */
    void remove_order(OrderId order_id) {
        const auto found = live_by_id_.find(order_id);
        if (found == live_by_id_.end()) {
            return;
        }

        // Remove the id from its side-specific FIFO level before dropping metadata.
        const auto order = found->second;
        if (order.side == Side::Buy) {
            remove_from_level(bids_, order.price, order_id);
        } else {
            remove_from_level(asks_, order.price, order_id);
        }

        erase_live_metadata(order_id);
    }

    /**
     * @brief Applies a modeled modify while preserving OrderBook priority rules.
     *
     * @param order_id Live order being modified.
     * @param new_price Replacement price.
     * @param new_quantity Replacement remaining quantity.
     */
    void modify_order(OrderId order_id, Price new_price, Quantity new_quantity) {
        auto existing = live_order(order_id);

        if (new_price == existing.price && new_quantity < existing.quantity) {
            // Pure reductions stay in place and keep FIFO priority.
            live_by_id_.at(order_id).quantity = new_quantity;
            return;
        }

        // Repricing and size increases use cancel-replace semantics at the new level.
        remove_order(order_id);
        existing.price = new_price;
        existing.quantity = new_quantity;
        existing.prev = nullptr;
        existing.next = nullptr;
        add_order(existing);
    }

private:
    /**
     * @brief Removes an id from one modeled price-level map.
     *
     * @param levels Side-specific FIFO levels.
     * @param price Price level to update.
     * @param order_id Order id to erase from the level.
     */
    template <typename Levels>
    void remove_from_level(Levels& levels, Price price, OrderId order_id) {
        auto level = levels.find(price);
        if (level == levels.end()) {
            return;
        }

        // The generation model is off the timed path, so simple deque erase is fine here.
        auto& queue = level->second;
        const auto found = std::ranges::find(queue, order_id);
        if (found != queue.end()) {
            queue.erase(found);
        }
        if (queue.empty()) {
            levels.erase(level);
        }
    }

    /**
     * @brief Removes an id from random-selection and order metadata.
     *
     * @param order_id Live order id to erase.
     */
    void erase_live_metadata(OrderId order_id) {
        // Swap-remove keeps the live id vector compact for O(1) random targeting.
        const auto live_index = live_index_by_id_.at(order_id);
        const auto moved_id = live_ids_.back();
        live_ids_[live_index] = moved_id;
        live_index_by_id_[moved_id] = live_index;
        live_ids_.pop_back();

        live_index_by_id_.erase(order_id);
        live_by_id_.erase(order_id);
    }

    std::map<Price, std::deque<OrderId>, std::greater<Price>> bids_;
    std::map<Price, std::deque<OrderId>> asks_;
    std::unordered_map<OrderId, Order> live_by_id_;
    std::unordered_map<OrderId, std::size_t> live_index_by_id_;
    std::vector<OrderId> live_ids_;
};

/**
 * @brief Computes the current mixed-workload reserve heuristic.
 *
 * @param operation_count Requested primary operation count.
 * @return Caller-selected OrderBook reserve capacity.
 */
[[nodiscard]] std::size_t reserve_order_capacity(std::size_t operation_count) {
    // Keep the existing 10% reserve heuristic so this benchmark compares cleanly.
    return std::max<std::size_t>(kMinimumReserveOrderCapacity, operation_count / 10);
}

/**
 * @brief Creates a passive GTC order near the midpoint without crossing.
 *
 * @param order_id Stable order id.
 * @param side Resting side.
 * @param rng Fixed-seed RNG for deterministic price/quantity variation.
 * @return Passive limit order.
 */
[[nodiscard]] Order make_passive_order(OrderId order_id, Side side, std::mt19937& rng) {
    std::uniform_int_distribution<int> offset_distribution{1, 3};
    std::uniform_int_distribution<int> quantity_distribution{1, 8};
    const auto offset = offset_distribution(rng);
    const auto price = side == Side::Buy ? kMidPrice - offset : kMidPrice + offset;

    // Small quantities keep crossing submits focused on short best-level work.
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = kBaseQuantity + static_cast<Quantity>(quantity_distribution(rng)),
                 .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Creates a random passive order around the tight spread.
 *
 * @param order_id Stable order id.
 * @param rng Fixed-seed RNG for side and order attributes.
 * @return Passive GTC limit order.
 */
[[nodiscard]] Order make_random_passive_order(OrderId order_id, std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    return make_passive_order(order_id, side_distribution(rng) ? Side::Buy : Side::Sell, rng);
}

/**
 * @brief Converts requested operation shares into exact integer counts.
 *
 * @param operation_count Number of primary operations.
 * @return Counts for GTC submit, cancel, modify, and crossing GTC.
 */
[[nodiscard]] std::array<std::size_t, 4> primary_operation_counts(std::size_t operation_count) {
    constexpr std::array<std::size_t, 4> weights{50, 30, 15, 5};
    std::array<std::size_t, 4> counts{};
    std::array<std::pair<std::size_t, std::size_t>, 4> remainders{};
    std::size_t assigned = 0;

    for (std::size_t index = 0; index < weights.size(); ++index) {
        // Integer division gives deterministic exact counts for common sizes.
        const auto scaled = operation_count * weights[index];
        counts[index] = scaled / 100;
        remainders[index] = {scaled % 100, index};
        assigned += counts[index];
    }

    std::ranges::sort(remainders, std::greater<>{});
    for (std::size_t index = 0; assigned < operation_count; ++index, ++assigned) {
        // Largest remainders receive leftover operations when counts are not exact.
        ++counts[remainders[index].second];
    }

    return counts;
}

/**
 * @brief Chooses the next primary operation from the remaining target mix.
 *
 * @param remaining Remaining operation counts by primary kind.
 * @param rng Fixed-seed RNG for deterministic random interleaving.
 * @return Selected primary operation kind.
 */
[[nodiscard]] PrimaryKind choose_primary_kind(const std::array<std::size_t, 4>& remaining,
                                              std::mt19937& rng) {
    std::size_t total_weight = 0;
    for (const auto weight : remaining) {
        // Remaining counts double as weights so the final stream keeps exact shares.
        total_weight += weight;
    }

    std::uniform_int_distribution<std::size_t> distribution{1, total_weight};
    auto pick = distribution(rng);
    for (std::size_t index = 0; index < remaining.size(); ++index) {
        if (pick <= remaining[index]) {
            return static_cast<PrimaryKind>(index);
        }
        pick -= remaining[index];
    }

    return PrimaryKind::GtcLimit;
}

/**
 * @brief Appends a passive submit action and updates modeled live state.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param next_order_id Next unique order id.
 * @param rng Fixed-seed RNG for deterministic order attributes.
 */
void append_passive_submit(Workload& workload,
                           ShallowBookModel& model,
                           OrderId& next_order_id,
                           std::mt19937& rng) {
    auto order = make_random_passive_order(next_order_id++, rng);

    // Passive GTC submits become future live cancel/modify targets.
    model.add_order(order);
    workload.actions.push_back(Action{.kind = ActionKind::Submit, .order = order});
}

/**
 * @brief Appends a cancel action and removes the target from the model.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param order_id Live order id to cancel.
 */
void append_cancel(Workload& workload, ShallowBookModel& model, OrderId order_id) {
    // Remove in generation state now so later actions cannot target a dead order.
    model.remove_order(order_id);
    workload.actions.push_back(Action{.kind = ActionKind::Cancel, .target_order_id = order_id});
}

/**
 * @brief Appends a modify action that stays passive and updates modeled state.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param rng Fixed-seed RNG for deterministic modify shape.
 */
void append_modify(Workload& workload, ShallowBookModel& model, std::mt19937& rng) {
    const auto order_id = model.random_live_order_id(rng);
    const auto existing = model.live_order(order_id);
    std::uniform_int_distribution<int> mode_distribution{0, 2};
    Price new_price = existing.price;
    Quantity new_quantity = existing.quantity;

    if (mode_distribution(rng) == 0 && existing.quantity > 1) {
        // Same-price reductions exercise the in-place priority-preserving path.
        new_quantity = existing.quantity - 1;
    } else {
        // Passive repricing exercises cancel-replace without crossing the spread.
        std::uniform_int_distribution<int> offset_distribution{1, 3};
        const auto offset = offset_distribution(rng);
        new_price = existing.side == Side::Buy ? kMidPrice - offset : kMidPrice + offset;
        new_quantity = existing.quantity + 1;
    }

    model.modify_order(order_id, new_price, new_quantity);
    workload.actions.push_back(Action{.kind = ActionKind::Modify,
                                      .target_order_id = order_id,
                                      .new_price = new_price,
                                      .new_quantity = new_quantity});
}

/**
 * @brief Appends a crossing GTC submit that fully fills one resting order.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param next_order_id Next transient incoming order id.
 * @param rng Fixed-seed RNG for deterministic side choice.
 */
void append_crossing_submit(Workload& workload,
                            ShallowBookModel& model,
                            OrderId& next_order_id,
                            std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    auto side = side_distribution(rng) ? Side::Buy : Side::Sell;
    auto resting_id = model.best_opposite_order_id(side);
    if (!resting_id.has_value()) {
        // A missing side is unlikely at 512 orders, but flip sides to keep the action valid.
        side = side == Side::Buy ? Side::Sell : Side::Buy;
        resting_id = model.best_opposite_order_id(side);
    }
    if (!resting_id.has_value()) {
        // If both sides somehow disappear, replenish rather than generating an invalid target.
        append_passive_submit(workload, model, next_order_id, rng);
        return;
    }

    const auto resting = model.live_order(*resting_id);
    auto order = Order{.id = next_order_id++,
                       .side = side,
                       .price = resting.price,
                       .quantity = resting.quantity,
                       .time_in_force = TimeInForce::GoodTilCancel};

    // The incoming quantity equals the FIFO-front resting quantity, so it never rests.
    model.remove_order(resting.id);
    workload.actions.push_back(Action{.kind = ActionKind::Submit, .order = order});
}

/**
 * @brief Rebalances modeled depth back to the shallow-book target.
 *
 * @param workload Workload receiving maintenance actions.
 * @param model Generation model to update.
 * @param next_order_id Next unique passive order id.
 * @param rng Fixed-seed RNG for deterministic maintenance choices.
 */
void rebalance_to_target(Workload& workload,
                         ShallowBookModel& model,
                         OrderId& next_order_id,
                         std::mt19937& rng) {
    while (model.live_order_count() < kTargetLiveOrders) {
        // Replenishment keeps cancel/match churn from draining the shallow book.
        append_passive_submit(workload, model, next_order_id, rng);
    }

    while (model.live_order_count() > kTargetLiveOrders) {
        // Extra passive submits are trimmed so live depth stays cache-hot and bounded.
        append_cancel(workload, model, model.random_live_order_id(rng));
    }
}

/**
 * @brief Builds deterministic preload liquidity outside the timed loop.
 *
 * @param model Generation model receiving the same live state.
 * @return Preload orders to submit before timing starts.
 */
[[nodiscard]] std::vector<Order> make_preload(ShallowBookModel& model) {
    std::mt19937 rng{kSeed ^ 0xC0FF'EEU};
    std::vector<Order> preload_orders;
    preload_orders.reserve(kTargetLiveOrders);

    for (std::size_t index = 0; index < kTargetLiveOrders; ++index) {
        // Alternating sides guarantees a two-sided book before random churn begins.
        const auto side = index % 2 == 0 ? Side::Buy : Side::Sell;
        auto order = make_passive_order(kPreloadIdBase + index, side, rng);
        model.add_order(order);
        preload_orders.push_back(order);
    }

    return preload_orders;
}

/**
 * @brief Builds a replayable shallow GTC mixed workload.
 *
 * @param operation_count Number of primary operations requested by the benchmark argument.
 * @return Preload orders, replay actions, and benchmark metadata.
 */
[[nodiscard]] Workload make_workload(std::size_t operation_count) {
    Workload workload;
    ShallowBookModel model;
    std::mt19937 rng{kSeed};
    auto remaining = primary_operation_counts(operation_count);
    auto next_order_id = kTimedIdBase;

    workload.primary_operations = operation_count;
    workload.reserve_order_capacity = reserve_order_capacity(operation_count);
    workload.preload_orders = make_preload(model);
    workload.actions.reserve(operation_count * 2);
    workload.max_live_orders = model.live_order_count();

    for (std::size_t index = 0; index < operation_count; ++index) {
        const auto kind = choose_primary_kind(remaining, rng);
        --remaining[static_cast<std::size_t>(kind)];

        if (kind == PrimaryKind::GtcLimit) {
            append_passive_submit(workload, model, next_order_id, rng);
        } else if (kind == PrimaryKind::Cancel) {
            append_cancel(workload, model, model.random_live_order_id(rng));
        } else if (kind == PrimaryKind::Modify) {
            append_modify(workload, model, rng);
        } else {
            append_crossing_submit(workload, model, next_order_id, rng);
        }

        // Record the brief high-water mark before maintenance trims extra live orders.
        workload.max_live_orders = std::max(workload.max_live_orders, model.live_order_count());

        // Maintenance actions keep the timed replay centered on the shallow target depth.
        rebalance_to_target(workload, model, next_order_id, rng);
        workload.max_live_orders = std::max(workload.max_live_orders, model.live_order_count());
    }

    return workload;
}

/**
 * @brief Preloads deterministic resting GTC liquidity into the real OrderBook.
 *
 * @param book Book under test.
 * @param preload_orders Orders to add before timing.
 * @param events Reusable caller-owned event buffer.
 */
void preload_book(OrderBook& book,
                  const std::vector<Order>& preload_orders,
                  std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload establishes the target live depth outside the measured loop.
        book.submit(order, events);
    }
}

/**
 * @brief Replays one direct OrderBook action without parser or exchange layers.
 *
 * @param book Book under test.
 * @param action Pre-generated action to apply.
 * @param events Reusable caller-owned event buffer.
 * @return Cancel result for cancel actions; default rejected result otherwise.
 */
[[nodiscard]] CancelResult run_action(OrderBook& book,
                                      const Action& action,
                                      std::vector<Event>& events) {
    if (action.kind == ActionKind::Submit) {
        // Submit covers both passive GTC adds and crossing GTC taker orders.
        book.submit(action.order, events);
    } else if (action.kind == ActionKind::Cancel) {
        // Cancel uses the direct single-event hot path.
        return book.cancel(action.target_order_id);
    } else {
        // Modify reuses the caller-owned event buffer for in-place and replace cases.
        book.modify(action.target_order_id, action.new_price, action.new_quantity, events);
    }

    return RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = 0};
}

/**
 * @brief Measures cache-hot shallow-book GTC churn directly on OrderBook.
 */
void BM_ShallowGtcMixed(benchmark::State& state) {
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

    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(workload.actions.size()));
    state.counters["primary_operations"] = static_cast<double>(workload.primary_operations);
    state.counters["book_actions"] = static_cast<double>(workload.actions.size());
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["target_live_orders"] = static_cast<double>(kTargetLiveOrders);
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
}

BENCHMARK(BM_ShallowGtcMixed)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
