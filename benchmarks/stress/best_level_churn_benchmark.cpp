#include "book/order_book.hpp"
#include "core/event.hpp"
#include "core/order.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
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
using matching_engine::Side;
using matching_engine::TimeInForce;

constexpr Price kMidPrice = 100'000;
constexpr Price kInitialBestBid = 100'000;
constexpr Price kInitialBestAsk = 100'010;
constexpr Quantity kRestingQuantity = 100;
constexpr std::size_t kInitialBestDepth = 4;
constexpr std::size_t kMinSideOrders = 8;
constexpr std::size_t kMinimumReserveOrderCapacity = 1'024;
constexpr std::uint32_t kSeed = 0xB357'1EAFU;
constexpr OrderId kPreloadIdBase = 40'000'000'000ULL;
constexpr OrderId kTimedIdBase = 50'000'000'000ULL;

/**
 * @brief Direct OrderBook operation emitted by the best-level churn generator.
 */
enum class ActionKind {
    Submit,
    SubmitMarket,
    Cancel,
    Modify
};

/**
 * @brief One pre-generated OrderBook hot-path action.
 */
struct Action {
    ActionKind kind{};
    Order order{};
    OrderId target_order_id{};
    Price new_price{};
    Quantity new_quantity{};
};

/**
 * @brief Deterministic workload plus metadata for one benchmark size.
 */
struct Workload {
    std::vector<Order> preload_orders;
    std::vector<Action> actions;
    std::size_t reserve_order_capacity{};
    std::size_t max_live_orders{};
};

/**
 * @brief Lightweight generation-only model of best-level liquidity.
 */
class BestLevelModel {
public:
    /**
     * @brief Adds a resting order to the modeled book.
     *
     * @param order Order that should become live and targetable.
     */
    void add_resting_order(const Order& order) {
        // Keep the live lookup and FIFO price level in sync for later actions.
        live_by_id_.emplace(order.id, order);
        if (order.side == Side::Buy) {
            bids_[order.price].push_back(order.id);
            ++bid_order_count_;
        } else {
            asks_[order.price].push_back(order.id);
            ++ask_order_count_;
        }
    }

    /**
     * @brief Returns the current live resting order count.
     */
    [[nodiscard]] std::size_t live_order_count() const {
        // The benchmark reports peak live orders as reserve/debug metadata.
        return bid_order_count_ + ask_order_count_;
    }

    /**
     * @brief Returns true when both sides have executable opposite liquidity.
     */
    [[nodiscard]] bool has_two_sided_liquidity() const {
        // Best-level churn needs both sides so marketable flow can keep rotating the inside.
        return !bids_.empty() && !asks_.empty();
    }

    /**
     * @brief Returns true when a side is below the replenishment threshold.
     *
     * @param side Side to inspect.
     */
    [[nodiscard]] bool side_is_thin(Side side) const {
        // Thin sides are replenished before they can make cancel or market actions invalid.
        return side == Side::Buy ? bid_order_count_ < kMinSideOrders
                                 : ask_order_count_ < kMinSideOrders;
    }

    /**
     * @brief Returns the current best price for one side.
     *
     * @param side Side to inspect.
     * @return Best bid or ask price, or the initial inside when empty.
     */
    [[nodiscard]] Price best_price(Side side) const {
        // Empty-side fallback is only used while generating immediate replenishment.
        if (side == Side::Buy) {
            return bids_.empty() ? kInitialBestBid : bids_.begin()->first;
        }
        return asks_.empty() ? kInitialBestAsk : asks_.begin()->first;
    }

    /**
     * @brief Returns one FIFO-front order id at the current best price.
     *
     * @param side Side to inspect.
     * @return Best-level order id when available.
     */
    [[nodiscard]] std::optional<OrderId> best_order_id(Side side) const {
        // Cancels intentionally target the inside level instead of random deep-book orders.
        if (side == Side::Buy) {
            if (bids_.empty()) {
                return std::nullopt;
            }
            return bids_.begin()->second.front();
        }

        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->second.front();
    }

    /**
     * @brief Returns one non-front best-level order for priority-loss modifies.
     *
     * @param side Side to inspect.
     * @return Second best-level order id when the level has enough depth.
     */
    [[nodiscard]] std::optional<OrderId> second_best_order_id(Side side) const {
        // Modifying the second order makes priority loss visible without draining the level.
        const auto* queue = best_queue(side);
        if (queue == nullptr || queue->size() < 2) {
            return std::nullopt;
        }
        return (*queue)[1];
    }

    /**
     * @brief Returns current visible volume at the best level.
     *
     * @param side Side to inspect.
     */
    [[nodiscard]] Quantity best_level_quantity(Side side) const {
        // Marketable actions size themselves from live inside liquidity.
        const auto* queue = best_queue(side);
        if (queue == nullptr) {
            return 0;
        }

        Quantity total = 0;
        for (const auto order_id : *queue) {
            total += live_by_id_.at(order_id).quantity;
        }
        return total;
    }

    /**
     * @brief Removes one modeled resting order by id.
     *
     * @param order_id Live order id to erase.
     */
    void remove_order(OrderId order_id) {
        const auto found = live_by_id_.find(order_id);
        if (found == live_by_id_.end()) {
            return;
        }

        // Remove the id from its current level before deleting order metadata.
        const auto order = found->second;
        if (order.side == Side::Buy) {
            remove_from_level(bids_, order.price, order_id);
            --bid_order_count_;
        } else {
            remove_from_level(asks_, order.price, order_id);
            --ask_order_count_;
        }
        live_by_id_.erase(found);
    }

    /**
     * @brief Updates the model for a priority-losing passive modify.
     *
     * @param order_id Live order being modified.
     * @param new_price Replacement price.
     * @param new_quantity Replacement quantity.
     */
    void modify_passive(OrderId order_id, Price new_price, Quantity new_quantity) {
        const auto existing = live_by_id_.at(order_id);

        // Reprice/size-increase modifies are cancel-replace operations at the new level.
        remove_order(order_id);
        add_resting_order(Order{.id = order_id,
                                .side = existing.side,
                                .price = new_price,
                                .quantity = new_quantity,
                                .time_in_force = TimeInForce::GoodTilCancel});
    }

    /**
     * @brief Updates the model for an incoming order that trades at the inside.
     *
     * @param incoming Aggressive order to apply.
     */
    void execute_aggressive(Order incoming) {
        // Marketable flow consumes FIFO liquidity and erases empty inside levels.
        if (incoming.side == Side::Buy) {
            match_against_asks(incoming);
        } else {
            match_against_bids(incoming);
        }
    }

private:
    /**
     * @brief Returns a pointer to the current best queue for one side.
     *
     * @param side Side to inspect.
     */
    [[nodiscard]] const std::deque<OrderId>* best_queue(Side side) const {
        // Queue access is centralized so best-level helpers use the same side rules.
        if (side == Side::Buy) {
            return bids_.empty() ? nullptr : &bids_.begin()->second;
        }
        return asks_.empty() ? nullptr : &asks_.begin()->second;
    }

    /**
     * @brief Removes an order id from a modeled side-level map.
     *
     * @param levels Side-specific price levels.
     * @param price Price level that owns the order.
     * @param order_id Order id to remove.
     */
    template <typename Levels>
    void remove_from_level(Levels& levels, Price price, OrderId order_id) {
        auto level = levels.find(price);
        if (level == levels.end()) {
            return;
        }

        // Generation is off the timed path, so linear erase keeps the model simple.
        auto& queue = level->second;
        const auto position = std::ranges::find(queue, order_id);
        if (position != queue.end()) {
            queue.erase(position);
        }
        if (queue.empty()) {
            levels.erase(level);
        }
    }

    /**
     * @brief Matches a buy order against modeled asks.
     *
     * @param incoming Buy order with remaining quantity.
     */
    void match_against_asks(Order& incoming) {
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto best = asks_.begin();
            if (best->first > incoming.price) {
                break;
            }

            // Consume the current best ask and remove the price level if it empties.
            consume_queue(incoming, best->second, ask_order_count_);
            if (best->second.empty()) {
                asks_.erase(best);
            }
        }
    }

    /**
     * @brief Matches a sell order against modeled bids.
     *
     * @param incoming Sell order with remaining quantity.
     */
    void match_against_bids(Order& incoming) {
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto best = bids_.begin();
            if (best->first < incoming.price) {
                break;
            }

            // Consume the current best bid and remove the price level if it empties.
            consume_queue(incoming, best->second, bid_order_count_);
            if (best->second.empty()) {
                bids_.erase(best);
            }
        }
    }

    /**
     * @brief Trades against one FIFO queue until the incoming order is done.
     *
     * @param incoming Incoming order with remaining quantity.
     * @param queue FIFO resting ids at the best price.
     * @param side_order_count Live-order count for the resting side.
     */
    void consume_queue(Order& incoming,
                       std::deque<OrderId>& queue,
                       std::size_t& side_order_count) {
        while (incoming.quantity > 0 && !queue.empty()) {
            const auto resting_id = queue.front();
            auto& resting = live_by_id_.at(resting_id);
            const auto traded = std::min(incoming.quantity, resting.quantity);

            // Reduce modeled quantities so later actions see the post-trade state.
            incoming.quantity -= traded;
            resting.quantity -= traded;

            if (resting.quantity == 0) {
                // Fully filled resting orders leave both the FIFO level and live lookup.
                queue.pop_front();
                live_by_id_.erase(resting_id);
                --side_order_count;
            }
        }
    }

    std::map<Price, std::deque<OrderId>, std::greater<Price>> bids_;
    std::map<Price, std::deque<OrderId>> asks_;
    std::unordered_map<OrderId, Order> live_by_id_;
    std::size_t bid_order_count_{};
    std::size_t ask_order_count_{};
};

/**
 * @brief Computes the reserve capacity used by the churn workload.
 *
 * @param operation_count Timed action count.
 * @return Caller-selected OrderBook reserve capacity.
 */
[[nodiscard]] std::size_t reserve_order_capacity(std::size_t operation_count) {
    // Existing mixed workloads use roughly 10% live-order depth for reservation.
    return std::max<std::size_t>(kMinimumReserveOrderCapacity, operation_count / 10);
}

/**
 * @brief Creates a passive GTC order.
 *
 * @param order_id Stable order id.
 * @param side Book side.
 * @param price Limit price.
 * @param quantity Resting quantity.
 * @return Order ready for direct OrderBook submission.
 */
[[nodiscard]] Order make_gtc(OrderId order_id, Side side, Price price, Quantity quantity) {
    // GTC orders are the only actions that can become future cancel/modify targets.
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = quantity,
                 .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Creates a transient market order.
 *
 * @param order_id Stable transient id.
 * @param side Incoming side.
 * @param quantity Quantity to match.
 * @return Market order for direct OrderBook submission.
 */
[[nodiscard]] Order make_market(OrderId order_id, Side side, Quantity quantity) {
    // The OrderBook market path ignores price but the struct still carries a valid field.
    return Order{.id = order_id,
                 .side = side,
                 .price = side == Side::Buy ? kInitialBestAsk : kInitialBestBid,
                 .quantity = quantity,
                 .time_in_force = TimeInForce::ImmediateOrCancel};
}

/**
 * @brief Adds a preload order to the workload and generation model.
 *
 * @param workload Workload receiving the preload order.
 * @param model Generation model to update.
 * @param order Order to preload before timing.
 */
void add_preload_order(Workload& workload, BestLevelModel& model, const Order& order) {
    // Preload establishes tight two-sided liquidity outside the measured loop.
    workload.preload_orders.push_back(order);
    model.add_resting_order(order);
}

/**
 * @brief Adds one timed action to the workload.
 *
 * @param workload Workload receiving the action.
 * @param action Direct OrderBook action to replay.
 */
void add_action(Workload& workload, const Action& action) {
    // The action count is the benchmark item count, so every append is timed work.
    workload.actions.push_back(action);
}

/**
 * @brief Appends a passive submit that improves the current best price by one tick.
 *
 * @param workload Workload receiving the action.
 * @param model Generation model to update.
 * @param next_order_id Next resting order id.
 * @param side Side to improve.
 */
void append_inside_improving_submit(Workload& workload,
                                    BestLevelModel& model,
                                    OrderId& next_order_id,
                                    Side side) {
    const auto old_best = model.best_price(side);
    const auto opposite_best = model.best_price(side == Side::Buy ? Side::Sell : Side::Buy);
    Price new_price = side == Side::Buy ? old_best + 1 : old_best - 1;

    if (side == Side::Buy && new_price >= opposite_best) {
        new_price = opposite_best - 1;
    } else if (side == Side::Sell && new_price <= opposite_best) {
        new_price = opposite_best + 1;
    }

    // One-tick inside improvement forces best-price maintenance without crossing.
    const auto order = make_gtc(next_order_id++, side, new_price, kRestingQuantity);
    add_action(workload, Action{.kind = ActionKind::Submit, .order = order});
    model.add_resting_order(order);
}

/**
 * @brief Appends a cancel against the current best bid or ask.
 *
 * @param workload Workload receiving the action.
 * @param model Generation model to update.
 * @param side Side whose best level should lose one order.
 * @return True when a cancel was appended.
 */
[[nodiscard]] bool append_best_cancel(Workload& workload, BestLevelModel& model, Side side) {
    const auto order_id = model.best_order_id(side);
    if (!order_id.has_value()) {
        return false;
    }

    // Canceling inside FIFO liquidity stresses best-level deletion and id lookup together.
    add_action(workload, Action{.kind = ActionKind::Cancel, .target_order_id = *order_id});
    model.remove_order(*order_id);
    return true;
}

/**
 * @brief Appends a marketable order that removes or shrinks the current best level.
 *
 * @param workload Workload receiving the action.
 * @param model Generation model to update.
 * @param next_order_id Next transient order id.
 * @param resting_side Side whose best liquidity should be consumed.
 */
void append_marketable_best_take(Workload& workload,
                                 BestLevelModel& model,
                                 OrderId& next_order_id,
                                 Side resting_side) {
    const auto available = model.best_level_quantity(resting_side);
    const auto side = resting_side == Side::Buy ? Side::Sell : Side::Buy;
    const auto quantity = std::max<Quantity>(1, available == 0 ? 1 : available / 2 + 1);
    auto order = make_market(next_order_id++, side, quantity);

    // Market flow stays transient and focuses timing on OrderBook matching updates.
    add_action(workload, Action{.kind = ActionKind::SubmitMarket, .order = order});
    model.execute_aggressive(order);
}

/**
 * @brief Appends an occasional modify that loses priority or crosses.
 *
 * @param workload Workload receiving the action.
 * @param model Generation model to update.
 * @param rng Deterministic generator for mode selection.
 * @param side Side to target.
 * @return True when a modify was appended.
 */
[[nodiscard]] bool append_modify(Workload& workload,
                                 BestLevelModel& model,
                                 std::mt19937& rng,
                                 Side side) {
    std::bernoulli_distribution crossing_distribution{0.35};
    const bool crossing = crossing_distribution(rng) && model.has_two_sided_liquidity();
    const auto order_id = crossing ? model.best_order_id(side) : model.second_best_order_id(side);
    if (!order_id.has_value()) {
        return false;
    }

    const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
    Price new_price = model.best_price(side);
    Quantity new_quantity = kRestingQuantity + 1;

    if (crossing) {
        // Crossing modifies exercise cancel-replace followed immediately by matching.
        new_price = model.best_price(opposite);
        new_quantity = std::max<Quantity>(1, model.best_level_quantity(opposite) / 2);
        model.remove_order(*order_id);
        model.execute_aggressive(make_gtc(*order_id, side, new_price, new_quantity));
    } else {
        // Size increase at the same price loses FIFO priority without changing the spread.
        model.modify_passive(*order_id, new_price, new_quantity);
    }

    add_action(workload,
               Action{.kind = ActionKind::Modify,
                      .target_order_id = *order_id,
                      .new_price = new_price,
                      .new_quantity = new_quantity});
    return true;
}

/**
 * @brief Appends a passive replenish action when a side is too thin.
 *
 * @param workload Workload receiving the action.
 * @param model Generation model to update.
 * @param next_order_id Next resting order id.
 * @param side Side to replenish.
 */
void append_replenish(Workload& workload,
                      BestLevelModel& model,
                      OrderId& next_order_id,
                      Side side) {
    const auto opposite_best = model.best_price(side == Side::Buy ? Side::Sell : Side::Buy);
    const auto offset = side == Side::Buy ? -2 : 2;
    const auto price = side == Side::Buy ? std::min(model.best_price(side), opposite_best - 1)
                                         : std::max(model.best_price(side), opposite_best + 1);
    const auto order = make_gtc(next_order_id++, side, price + offset, kRestingQuantity);

    // Replenishment is timed because it is part of keeping the churn stream live.
    add_action(workload, Action{.kind = ActionKind::Submit, .order = order});
    model.add_resting_order(order);
}

/**
 * @brief Builds the deterministic preload book around the mid price.
 *
 * @param workload Workload receiving preload orders.
 * @param model Generation model to update.
 */
void seed_tight_book(Workload& workload, BestLevelModel& model) {
    OrderId next_id = kPreloadIdBase;

    for (Price price = kMidPrice - 10; price <= kMidPrice; ++price) {
        const auto depth = price == kInitialBestBid ? kInitialBestDepth : 1;
        for (std::size_t depth_index = 0; depth_index < depth; ++depth_index) {
            // Bid levels 99_990..100_000 provide tight initial inside liquidity.
            add_preload_order(
                workload, model, make_gtc(next_id++, Side::Buy, price, kRestingQuantity));
        }
    }

    for (Price price = kMidPrice + 10; price <= kMidPrice + 20; ++price) {
        const auto depth = price == kInitialBestAsk ? kInitialBestDepth : 1;
        for (std::size_t depth_index = 0; depth_index < depth; ++depth_index) {
            // Ask levels 100_010..100_020 mirror the bid seed around the mid price.
            add_preload_order(
                workload, model, make_gtc(next_id++, Side::Sell, price, kRestingQuantity));
        }
    }
}

/**
 * @brief Creates a deterministic best-level churn workload.
 *
 * @param operation_count Number of timed OrderBook actions to generate.
 * @return Replayable workload and reserve metadata.
 */
[[nodiscard]] Workload make_workload(std::size_t operation_count) {
    Workload workload;
    BestLevelModel model;
    std::mt19937 rng{kSeed ^ static_cast<std::uint32_t>(operation_count)};
    OrderId next_order_id = kTimedIdBase;

    workload.reserve_order_capacity = reserve_order_capacity(operation_count);
    workload.preload_orders.reserve(64);
    workload.actions.reserve(operation_count);
    seed_tight_book(workload, model);
    workload.max_live_orders = model.live_order_count();

    for (std::size_t index = 0; index < operation_count; ++index) {
        const auto side = ((index / 2) + (rng() & 1U)) % 2 == 0 ? Side::Buy : Side::Sell;

        if (model.side_is_thin(Side::Buy)) {
            append_replenish(workload, model, next_order_id, Side::Buy);
        } else if (model.side_is_thin(Side::Sell)) {
            append_replenish(workload, model, next_order_id, Side::Sell);
        } else if (index % 16 == 15 && append_modify(workload, model, rng, side)) {
            // Occasional modifies are interleaved with cancel/insert/take churn.
        } else if (index % 3 == 0) {
            [[maybe_unused]] const bool appended = append_best_cancel(workload, model, side);
        } else if (index % 3 == 1) {
            append_inside_improving_submit(workload, model, next_order_id, side);
        } else {
            append_marketable_best_take(workload, model, next_order_id, side);
        }

        // Track peak live depth so reserve sizing can be audited from benchmark output.
        workload.max_live_orders = std::max(workload.max_live_orders, model.live_order_count());
    }

    return workload;
}

/**
 * @brief Submits preload liquidity outside the timed loop.
 *
 * @param book Order book to seed.
 * @param preload_orders Pre-generated preload orders.
 * @param events Reusable caller-owned event buffer.
 */
void preload_book(OrderBook& book,
                  const std::vector<Order>& preload_orders,
                  std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload uses the same direct OrderBook API but is excluded from timing.
        book.submit(order, events);
    }
}

/**
 * @brief Replays one direct OrderBook action.
 *
 * @param book Book under test.
 * @param action Action to execute.
 * @param events Reusable caller-owned event buffer.
 * @return Cancel result when the action is a cancel.
 */
[[nodiscard]] std::optional<CancelResult> run_action(OrderBook& book,
                                                     const Action& action,
                                                     std::vector<Event>& events) {
    switch (action.kind) {
    case ActionKind::Submit:
        book.submit(action.order, events);
        return std::nullopt;
    case ActionKind::SubmitMarket:
        book.submit_market(action.order, events);
        return std::nullopt;
    case ActionKind::Cancel:
        return book.cancel(action.target_order_id);
    case ActionKind::Modify:
        book.modify(action.target_order_id, action.new_price, action.new_quantity, events);
        return std::nullopt;
    }

    return std::nullopt;
}

/**
 * @brief Measures repeated top-of-book mutation on the direct OrderBook hot path.
 */
void BM_Stress_BestLevelChurn_Throughput(benchmark::State& state) {
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

    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["preload_orders"] = static_cast<double>(workload.preload_orders.size());
    state.counters["reserve_order_capacity"] =
        static_cast<double>(workload.reserve_order_capacity);
    state.counters["max_live_orders"] = static_cast<double>(workload.max_live_orders);
}

BENCHMARK(BM_Stress_BestLevelChurn_Throughput)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Arg(1'000'000);

} // namespace
