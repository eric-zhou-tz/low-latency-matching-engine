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

constexpr Price kMidPrice = 1'000'000'000;
constexpr Price kPriceStride = 17;
constexpr Quantity kBaseQuantity = 10;
constexpr std::size_t kPreloadPriceLevels = 50'000;
constexpr std::size_t kCrossingLevelsPerOrder = 4;
constexpr std::size_t kReserveSlack = 1'024;
constexpr std::uint32_t kSeed = 0xD33F'5A7EU;
constexpr OrderId kPreloadIdBase = 40'000'000'000ULL;
constexpr OrderId kTimedIdBase = 50'000'000'000ULL;

/**
 * @brief Primary operation kind requested by the deep sparse GTC workload.
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
 * @brief Full deterministic deep sparse workload and metadata.
 */
struct Workload {
    std::vector<Order> preload_orders;
    std::vector<Action> actions;
    std::size_t primary_operations{};
    std::size_t reserve_order_capacity{};
    std::size_t max_live_orders{};
    std::size_t max_price_levels{};
};

/**
 * @brief Lightweight generation model for sparse price levels and live ids.
 */
class DeepSparseBookModel {
public:
    /**
     * @brief Adds a modeled resting GTC order to live id and price-level indexes.
     *
     * @param order Passive order that should become cancel/modify eligible.
     */
    void add_order(const Order& order) {
        // Keep random id selection and side-specific FIFO levels in sync.
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
        // Reserve sizing and generation safety depend on the current live set.
        return live_ids_.size();
    }

    /**
     * @brief Returns the current occupied price-level count.
     */
    [[nodiscard]] std::size_t price_level_count() const {
        // The workload tracks level depth separately from order count.
        return bids_.size() + asks_.size();
    }

    /**
     * @brief Chooses a random live order id for cancel or modify generation.
     *
     * @param rng Fixed-seed RNG used for deterministic interleaving.
     * @return Live order id that exists in the modeled book.
     */
    [[nodiscard]] OrderId random_live_order_id(std::mt19937& rng) const {
        // Uniform id selection exercises random lookup and removal paths.
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
        // Modify and crossing generation need the latest modeled quantity.
        return live_by_id_.at(order_id);
    }

    /**
     * @brief Collects FIFO orders from several best opposite sparse levels.
     *
     * @param incoming_side Side of the incoming crossing order.
     * @param max_price_levels Number of opposite price levels to walk.
     * @return Resting orders that the generated crossing order should fill.
     */
    [[nodiscard]] std::vector<Order> best_opposite_orders(Side incoming_side,
                                                          std::size_t max_price_levels) const {
        std::vector<Order> orders;
        orders.reserve(max_price_levels * 2);

        if (incoming_side == Side::Buy) {
            collect_best_orders(asks_, max_price_levels, orders);
        } else {
            collect_best_orders(bids_, max_price_levels, orders);
        }

        return orders;
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

        // Remove the id from the sparse price level before dropping metadata.
        const auto order = found->second;
        if (order.side == Side::Buy) {
            remove_from_level(bids_, order.price, order_id);
        } else {
            remove_from_level(asks_, order.price, order_id);
        }

        erase_live_metadata(order_id);
    }

    /**
     * @brief Applies cancel-replace style repricing in the generation model.
     *
     * @param order_id Live order being modified.
     * @param new_price Replacement sparse price.
     * @param new_quantity Replacement remaining quantity.
     */
    void modify_order(OrderId order_id, Price new_price, Quantity new_quantity) {
        auto replacement = live_order(order_id);

        // Deep sparse modifies intentionally move to a different level.
        remove_order(order_id);
        replacement.price = new_price;
        replacement.quantity = new_quantity;
        replacement.prev = nullptr;
        replacement.next = nullptr;
        add_order(replacement);
    }

private:
    /**
     * @brief Copies orders from the first several levels of one side.
     *
     * @param levels Side-specific FIFO levels.
     * @param max_price_levels Number of levels to include.
     * @param orders Output collection for matched resting orders.
     */
    template <typename Levels>
    void collect_best_orders(const Levels& levels,
                             std::size_t max_price_levels,
                             std::vector<Order>& orders) const {
        std::size_t levels_seen = 0;
        for (auto level = levels.begin(); level != levels.end() && levels_seen < max_price_levels;
             ++level, ++levels_seen) {
            // Sparse levels usually contain one order; keep the logic valid for two.
            for (const auto order_id : level->second) {
                orders.push_back(live_by_id_.at(order_id));
            }
        }
    }

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

        // Generation is outside the timed loop, so deque erase is acceptable here.
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
        // Swap-remove keeps random live-id selection compact and deterministic.
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
 * @brief Creates a passive sparse GTC order that cannot cross the spread.
 *
 * @param order_id Stable order id.
 * @param side Resting side.
 * @param price Sparse price selected by the generator.
 * @param rng Fixed-seed RNG for deterministic quantity variation.
 * @return Passive limit order.
 */
[[nodiscard]] Order make_passive_order(OrderId order_id,
                                       Side side,
                                       Price price,
                                       std::mt19937& rng) {
    std::uniform_int_distribution<int> quantity_distribution{0, 3};

    // Small quantities let crossing orders consume several sparse levels.
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = kBaseQuantity + static_cast<Quantity>(quantity_distribution(rng)),
                 .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Returns the next unique sparse price on a side.
 *
 * @param side Side receiving a new passive price.
 * @param next_bid_index Monotonic bid-side sparse index.
 * @param next_ask_index Monotonic ask-side sparse index.
 * @param rng Fixed-seed RNG for deterministic spacing jitter.
 * @return Unique passive price that stays away from crossing the midpoint.
 */
[[nodiscard]] Price next_sparse_price(Side side,
                                      std::size_t& next_bid_index,
                                      std::size_t& next_ask_index,
                                      std::mt19937& rng) {
    std::uniform_int_distribution<int> jitter_distribution{0, 3};
    const auto jitter = static_cast<Price>(jitter_distribution(rng));

    if (side == Side::Buy) {
        // New bid levels move farther below the midpoint while preserving uniqueness.
        const auto index = static_cast<Price>(++next_bid_index);
        return kMidPrice - (index * kPriceStride) - jitter;
    }

    // New ask levels move farther above the midpoint while preserving uniqueness.
    const auto index = static_cast<Price>(++next_ask_index);
    return kMidPrice + (index * kPriceStride) + jitter;
}

/**
 * @brief Converts requested operation shares into exact integer counts.
 *
 * @param operation_count Number of primary operations.
 * @return Counts for GTC submit, cancel, modify, and crossing GTC.
 */
[[nodiscard]] std::array<std::size_t, 4> primary_operation_counts(std::size_t operation_count) {
    constexpr std::array<std::size_t, 4> weights{45, 30, 15, 10};
    std::array<std::size_t, 4> counts{};
    std::array<std::pair<std::size_t, std::size_t>, 4> remainders{};
    std::size_t assigned = 0;

    for (std::size_t index = 0; index < weights.size(); ++index) {
        // Integer scaling gives exact shares for the standard benchmark sizes.
        const auto scaled = operation_count * weights[index];
        counts[index] = scaled / 100;
        remainders[index] = {scaled % 100, index};
        assigned += counts[index];
    }

    std::ranges::sort(remainders, std::greater<>{});
    for (std::size_t index = 0; assigned < operation_count; ++index, ++assigned) {
        // Largest remainders receive leftover operations for non-multiple sizes.
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
 * @brief Appends a passive sparse submit action and updates modeled live state.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param next_order_id Next unique order id.
 * @param next_bid_index Monotonic bid price index.
 * @param next_ask_index Monotonic ask price index.
 * @param rng Fixed-seed RNG for deterministic order attributes.
 */
void append_passive_submit(Workload& workload,
                           DeepSparseBookModel& model,
                           OrderId& next_order_id,
                           std::size_t& next_bid_index,
                           std::size_t& next_ask_index,
                           std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    const auto side = side_distribution(rng) ? Side::Buy : Side::Sell;
    const auto price = next_sparse_price(side, next_bid_index, next_ask_index, rng);
    auto order = make_passive_order(next_order_id++, side, price, rng);

    // New sparse prices keep per-level queues short while the tree grows deep.
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
void append_cancel(Workload& workload, DeepSparseBookModel& model, OrderId order_id) {
    // Remove now so later generated actions cannot target dead liquidity.
    model.remove_order(order_id);
    workload.actions.push_back(Action{.kind = ActionKind::Cancel, .target_order_id = order_id});
}

/**
 * @brief Appends a modify action that moves an order to a different sparse level.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param next_bid_index Monotonic bid price index.
 * @param next_ask_index Monotonic ask price index.
 * @param rng Fixed-seed RNG for deterministic modify attributes.
 */
void append_modify(Workload& workload,
                   DeepSparseBookModel& model,
                   std::size_t& next_bid_index,
                   std::size_t& next_ask_index,
                   std::mt19937& rng) {
    const auto order_id = model.random_live_order_id(rng);
    const auto existing = model.live_order(order_id);
    const auto new_price = next_sparse_price(existing.side, next_bid_index, next_ask_index, rng);
    const auto new_quantity = existing.quantity + 1;

    // Repricing preserves the live id but moves tree membership to a new level.
    model.modify_order(order_id, new_price, new_quantity);
    workload.actions.push_back(Action{.kind = ActionKind::Modify,
                                      .target_order_id = order_id,
                                      .new_price = new_price,
                                      .new_quantity = new_quantity});
}

/**
 * @brief Appends a crossing GTC submit that consumes several sparse levels.
 *
 * @param workload Workload receiving the replay action.
 * @param model Generation model to update.
 * @param next_order_id Next transient incoming order id.
 * @param rng Fixed-seed RNG for deterministic side choice.
 */
void append_crossing_submit(Workload& workload,
                            DeepSparseBookModel& model,
                            OrderId& next_order_id,
                            std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    auto side = side_distribution(rng) ? Side::Buy : Side::Sell;
    auto matched_orders = model.best_opposite_orders(side, kCrossingLevelsPerOrder);

    if (matched_orders.empty()) {
        // A missing side is unexpected at this depth, but flipping keeps the stream valid.
        side = side == Side::Buy ? Side::Sell : Side::Buy;
        matched_orders = model.best_opposite_orders(side, kCrossingLevelsPerOrder);
    }
    if (matched_orders.empty()) {
        return;
    }

    Quantity crossing_quantity = 0;
    for (const auto& resting : matched_orders) {
        // Exact aggregate quantity lets the crossing GTC order walk levels without resting.
        crossing_quantity += resting.quantity;
    }

    const auto limit_price = matched_orders.back().price;
    auto order = Order{.id = next_order_id++,
                       .side = side,
                       .price = limit_price,
                       .quantity = crossing_quantity,
                       .time_in_force = TimeInForce::GoodTilCancel};

    for (const auto& resting : matched_orders) {
        // Generation state mirrors fills so later cancels/modifies stay valid.
        model.remove_order(resting.id);
    }

    workload.actions.push_back(Action{.kind = ActionKind::Submit, .order = order});
}

/**
 * @brief Builds deterministic preload liquidity outside the timed loop.
 *
 * @param model Generation model receiving the same live state.
 * @param next_bid_index Monotonic bid price index initialized by preload.
 * @param next_ask_index Monotonic ask price index initialized by preload.
 * @return Preload orders to submit before timing starts.
 */
[[nodiscard]] std::vector<Order> make_preload(DeepSparseBookModel& model,
                                              std::size_t& next_bid_index,
                                              std::size_t& next_ask_index) {
    std::mt19937 rng{kSeed ^ 0xBEEF'CAFEU};
    std::vector<Order> preload_orders;
    preload_orders.reserve(kPreloadPriceLevels);

    for (std::size_t index = 0; index < kPreloadPriceLevels; ++index) {
        // Alternating sides creates a two-sided book with one order per level.
        const auto side = index % 2 == 0 ? Side::Buy : Side::Sell;
        const auto price = next_sparse_price(side, next_bid_index, next_ask_index, rng);
        auto order = make_passive_order(kPreloadIdBase + index, side, price, rng);
        model.add_order(order);
        preload_orders.push_back(order);
    }

    return preload_orders;
}

/**
 * @brief Builds a replayable deep sparse GTC mixed workload.
 *
 * @param operation_count Number of primary operations requested by the benchmark argument.
 * @return Preload orders, replay actions, and benchmark metadata.
 */
[[nodiscard]] Workload make_workload(std::size_t operation_count) {
    Workload workload;
    DeepSparseBookModel model;
    std::mt19937 rng{kSeed};
    auto remaining = primary_operation_counts(operation_count);
    auto next_order_id = kTimedIdBase;
    std::size_t next_bid_index = 0;
    std::size_t next_ask_index = 0;

    workload.primary_operations = operation_count;
    workload.preload_orders = make_preload(model, next_bid_index, next_ask_index);
    workload.actions.reserve(operation_count);
    workload.max_live_orders = model.live_order_count();
    workload.max_price_levels = model.price_level_count();

    for (std::size_t index = 0; index < operation_count; ++index) {
        const auto kind = choose_primary_kind(remaining, rng);
        --remaining[static_cast<std::size_t>(kind)];

        if (kind == PrimaryKind::GtcLimit) {
            append_passive_submit(
                workload, model, next_order_id, next_bid_index, next_ask_index, rng);
        } else if (kind == PrimaryKind::Cancel) {
            append_cancel(workload, model, model.random_live_order_id(rng));
        } else if (kind == PrimaryKind::Modify) {
            append_modify(workload, model, next_bid_index, next_ask_index, rng);
        } else {
            append_crossing_submit(workload, model, next_order_id, rng);
        }

        // Track high-water marks so the real book reserves for peak live state.
        workload.max_live_orders = std::max(workload.max_live_orders, model.live_order_count());
        workload.max_price_levels = std::max(workload.max_price_levels, model.price_level_count());
    }

    workload.reserve_order_capacity = workload.max_live_orders + kReserveSlack;
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
        // Preload establishes deep sparse liquidity outside the measured loop.
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
        // Submit covers both passive sparse adds and crossing GTC taker orders.
        book.submit(action.order, events);
    } else if (action.kind == ActionKind::Cancel) {
        // Cancel uses the direct single-event hot path.
        return book.cancel(action.target_order_id);
    } else {
        // Modify reuses the caller-owned event buffer for replace-at-new-level cases.
        book.modify(action.target_order_id, action.new_price, action.new_quantity, events);
    }

    return RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = 0};
}

/**
 * @brief Measures deep sparse GTC churn directly on OrderBook.
 */
void BM_DeepSparseGtcMixed(benchmark::State& state) {
    const auto operation_count = static_cast<std::size_t>(state.range(0));
    const auto workload = make_workload(operation_count);
    std::optional<OrderBook> book;
    std::vector<Event> events;
    events.reserve(16);

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
    state.counters["preload_price_levels"] = static_cast<double>(kPreloadPriceLevels);
    state.counters["crossing_levels_per_order"] =
        static_cast<double>(kCrossingLevelsPerOrder);
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
    state.counters["max_price_levels"] = static_cast<double>(workload.max_price_levels);
}

BENCHMARK(BM_DeepSparseGtcMixed)->Arg(1'000)->Arg(10'000)->Arg(100'000);

} // namespace
