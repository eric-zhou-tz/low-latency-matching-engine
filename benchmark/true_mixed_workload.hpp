#pragma once

#include "book/order_book.hpp"
#include "core/event.hpp"
#include "core/order.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace matching_engine::benchmark_workloads {

constexpr std::int64_t kTrueMixedMidPrice = 10'000;
constexpr std::uint64_t kTrueMixedRestingQuantity = 1'000;
constexpr std::uint64_t kTrueMixedIncomingIdBase = 10'000'000'000ULL;
constexpr std::uint64_t kTrueMixedPreloadIdBase = 20'000'000'000ULL;
constexpr std::uint32_t kTrueMixedSeed = 0x71A3'5EEDU;
constexpr std::size_t kTrueMixedMinimumReserve = 1'024;

/**
 * @brief Operation kind for the OrderBook-only true mixed benchmark.
 */
enum class TrueMixedOperationKind {
    GtcLimit,
    Cancel,
    Modify,
    IocLimit,
    Market,
    FokLimit
};

/**
 * @brief One pre-generated direct OrderBook operation.
 */
struct TrueMixedOperation {
    TrueMixedOperationKind kind{};
    Order order{};
    OrderId target_order_id{};
    Price new_price{};
    Quantity new_quantity{};
};

/**
 * @brief Complete deterministic true mixed workload.
 */
struct TrueMixedWorkload {
    std::vector<Order> preload_orders;
    std::vector<TrueMixedOperation> operations;
    std::size_t reserve_order_capacity{};
    std::size_t max_live_orders{};
};

/**
 * @brief Lightweight generation-only book model for valid cancel/modify ids.
 */
class TrueMixedBookModel {
public:
    /**
     * @brief Adds one resting order to the model.
     *
     * @param order Resting GTC order to index.
     */
    void add_resting_order(const Order& order) {
        // Track all lookup surfaces so random cancel/modify targets stay live.
        live_by_id_.emplace(order.id, order);
        live_ids_.push_back(order.id);
        live_index_by_id_.emplace(order.id, live_ids_.size() - 1);

        // Preserve FIFO order inside each modeled price level for fill simulation.
        if (order.side == Side::Buy) {
            bids_[order.price].push_back(order.id);
        } else {
            asks_[order.price].push_back(order.id);
        }
    }

    /**
     * @brief Returns whether at least one resting order is available.
     */
    [[nodiscard]] bool has_live_orders() const {
        // Cancel and modify generation are only valid when this live set is non-empty.
        return !live_ids_.empty();
    }

    /**
     * @brief Returns the current number of modeled resting orders.
     */
    [[nodiscard]] std::size_t live_order_count() const {
        // The benchmark records this as peak live depth metadata.
        return live_ids_.size();
    }

    /**
     * @brief Selects a random live resting order id.
     *
     * @param rng Deterministic generator used for the workload.
     * @return Live order id that can be canceled or modified.
     */
    [[nodiscard]] OrderId random_live_order_id(std::mt19937& rng) const {
        // Uniform selection avoids canceling or modifying only FIFO fronts.
        std::uniform_int_distribution<std::size_t> distribution{0, live_ids_.size() - 1};
        return live_ids_[distribution(rng)];
    }

    /**
     * @brief Returns a copy of a live order by id.
     *
     * @param order_id Live order id to read.
     * @return Current modeled order state.
     */
    [[nodiscard]] Order live_order(OrderId order_id) const {
        // The generator uses the current price and quantity to build realistic modifies.
        return live_by_id_.at(order_id);
    }

    /**
     * @brief Removes a live order from all model indexes.
     *
     * @param order_id Live order id to remove.
     */
    void remove_order(OrderId order_id) {
        const auto found = live_by_id_.find(order_id);
        if (found == live_by_id_.end()) {
            return;
        }

        // Remove the id from its FIFO price level before erasing order metadata.
        const auto order = found->second;
        if (order.side == Side::Buy) {
            auto level = bids_.find(order.price);
            if (level != bids_.end()) {
                auto& queue = level->second;
                const auto position = std::ranges::find(queue, order_id);
                if (position != queue.end()) {
                    queue.erase(position);
                }
                if (queue.empty()) {
                    bids_.erase(level);
                }
            }
        } else {
            auto level = asks_.find(order.price);
            if (level != asks_.end()) {
                auto& queue = level->second;
                const auto position = std::ranges::find(queue, order_id);
                if (position != queue.end()) {
                    queue.erase(position);
                }
                if (queue.empty()) {
                    asks_.erase(level);
                }
            }
        }

        erase_live_metadata(order_id);
    }

    /**
     * @brief Applies a generated modify to the model.
     *
     * @param order_id Live order id being modified.
     * @param new_price Replacement price.
     * @param new_quantity Replacement quantity.
     */
    void modify_order(OrderId order_id, Price new_price, Quantity new_quantity) {
        auto existing = live_order(order_id);

        if (new_price == existing.price && new_quantity < existing.quantity) {
            // Pure reductions keep FIFO priority and only update modeled quantity.
            live_by_id_.at(order_id).quantity = new_quantity;
            return;
        }

        // Price changes and size increases are modeled as cancel-replace operations.
        remove_order(order_id);
        existing.price = new_price;
        existing.quantity = new_quantity;
        existing.prev = nullptr;
        existing.next = nullptr;
        add_resting_order(existing);
    }

    /**
     * @brief Applies a transient aggressive order to the model.
     *
     * @param order Transient incoming order.
     * @param is_market True when market matching should ignore limit prices.
     * @param is_fok True when full-fill preflight should avoid mutation on rejection.
     */
    void execute_aggressive_order(Order order, bool is_market, bool is_fok) {
        if (is_market) {
            // Market orders use unbounded crossing prices in the generation model too.
            order.price = order.side == Side::Buy ? std::numeric_limits<Price>::max()
                                                  : std::numeric_limits<Price>::min();
        }

        if (is_fok && !can_fully_fill(order)) {
            // FOK rejections must leave resting liquidity untouched.
            return;
        }

        // IOC, market, and successful FOK all consume modeled resting liquidity.
        match(order);
    }

    /**
     * @brief Returns total opposite liquidity available at an order's crossing price.
     *
     * @param order Incoming order to test.
     * @return Visible quantity that can currently trade.
     */
    [[nodiscard]] Quantity available_crossing_quantity(const Order& order) const {
        Quantity available = 0;

        if (order.side == Side::Buy) {
            for (const auto& [price, queue] : asks_) {
                if (price > order.price) {
                    break;
                }
                available += queue_quantity(queue);
            }
        } else {
            for (const auto& [price, queue] : bids_) {
                if (price < order.price) {
                    break;
                }
                available += queue_quantity(queue);
            }
        }

        return available;
    }

    /**
     * @brief Returns the best opposite price for an incoming side.
     *
     * @param side Incoming side.
     * @return Best opposite price, or null price when that side is empty.
     */
    [[nodiscard]] Price best_opposite_price(Side side) const {
        // The generator uses this to make taker flow cross by a realistic amount.
        if (side == Side::Buy) {
            return asks_.empty() ? 0 : asks_.begin()->first;
        }
        return bids_.empty() ? 0 : bids_.begin()->first;
    }

    /**
     * @brief Returns visible quantity at the best opposite price.
     *
     * @param side Incoming side.
     * @return Quantity available at the first crossing level.
     */
    [[nodiscard]] Quantity best_opposite_level_quantity(Side side) const {
        // Partial IOC generation targets one level so it does not drain the whole book.
        if (side == Side::Buy) {
            return asks_.empty() ? 0 : queue_quantity(asks_.begin()->second);
        }
        return bids_.empty() ? 0 : queue_quantity(bids_.begin()->second);
    }

private:
    /**
     * @brief Sums modeled quantity for one FIFO price level.
     *
     * @param queue Resting order ids at the price.
     * @return Current visible quantity at the level.
     */
    [[nodiscard]] Quantity queue_quantity(const std::deque<OrderId>& queue) const {
        Quantity total = 0;
        for (const auto order_id : queue) {
            // Quantities are stored in the live order map so modifies stay reflected.
            total += live_by_id_.at(order_id).quantity;
        }
        return total;
    }

    /**
     * @brief Checks whether current crossing liquidity can fully fill an order.
     *
     * @param order Incoming order to test.
     * @return True when all quantity is available now.
     */
    [[nodiscard]] bool can_fully_fill(const Order& order) const {
        // Reuse aggregate modeled liquidity so FOK generation mirrors OrderBook behavior.
        return available_crossing_quantity(order) >= order.quantity;
    }

    /**
     * @brief Executes modeled matching against the opposite side.
     *
     * @param incoming Incoming order whose remaining quantity is reduced.
     */
    void match(Order& incoming) {
        if (incoming.side == Side::Buy) {
            match_against_asks(incoming);
        } else {
            match_against_bids(incoming);
        }
    }

    /**
     * @brief Consumes modeled asks for a buy order.
     *
     * @param incoming Buy order being matched.
     */
    void match_against_asks(Order& incoming) {
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto best = asks_.begin();
            if (best->first > incoming.price) {
                break;
            }

            // Consume FIFO resting asks at the best crossing price.
            consume_best_level(incoming, best->second);
            if (best->second.empty()) {
                asks_.erase(best);
            }
        }
    }

    /**
     * @brief Consumes modeled bids for a sell order.
     *
     * @param incoming Sell order being matched.
     */
    void match_against_bids(Order& incoming) {
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto best = bids_.begin();
            if (best->first < incoming.price) {
                break;
            }

            // Consume FIFO resting bids at the best crossing price.
            consume_best_level(incoming, best->second);
            if (best->second.empty()) {
                bids_.erase(best);
            }
        }
    }

    /**
     * @brief Trades against the front order of one modeled price level.
     *
     * @param incoming Incoming order with remaining quantity.
     * @param queue FIFO ids at the crossing price.
     */
    void consume_best_level(Order& incoming, std::deque<OrderId>& queue) {
        while (incoming.quantity > 0 && !queue.empty()) {
            const auto resting_id = queue.front();
            auto& resting = live_by_id_.at(resting_id);
            const auto trade_quantity = std::min(incoming.quantity, resting.quantity);

            // Reduce both sides so future cancel/modify targets reflect fills.
            incoming.quantity -= trade_quantity;
            resting.quantity -= trade_quantity;

            if (resting.quantity == 0) {
                // Fully filled resting orders leave both FIFO and id-selection indexes.
                queue.pop_front();
                erase_live_metadata(resting_id);
            }
        }
    }

    /**
     * @brief Removes an id from live lookup metadata after queue cleanup.
     *
     * @param order_id Live order id to erase.
     */
    void erase_live_metadata(OrderId order_id) {
        // Swap-remove keeps random selection compact without preserving order.
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
 * @param operation_count Timed operation count.
 * @return Predicted live-order reserve capacity.
 */
[[nodiscard]] inline std::size_t true_mixed_reserve_order_capacity(std::size_t operation_count) {
    // This preserves the current mixed benchmark 10% live-order heuristic.
    return std::max<std::size_t>(kTrueMixedMinimumReserve, operation_count / 10);
}

/**
 * @brief Creates one passive order around the mid-price.
 *
 * @param order_id Stable order id.
 * @param side Book side.
 * @param rng Deterministic generator used for price offsets.
 * @return Non-crossing GTC limit order.
 */
[[nodiscard]] inline Order make_passive_order(OrderId order_id, Side side, std::mt19937& rng) {
    std::uniform_int_distribution<int> offset_distribution{1, 4};
    const auto offset = offset_distribution(rng);
    const auto price = side == Side::Buy ? kTrueMixedMidPrice - offset
                                         : kTrueMixedMidPrice + offset;

    // Large resting quantity lets aggressive flow match without immediately draining all ids.
    return Order{.id = order_id,
                 .side = side,
                 .price = price,
                 .quantity = kTrueMixedRestingQuantity,
                 .time_in_force = TimeInForce::GoodTilCancel};
}

/**
 * @brief Builds deterministic preload liquidity outside the timed operation mix.
 *
 * @param reserve_order_capacity Reserve heuristic used to size the seed book.
 * @param model Generation model to receive preload state.
 * @return Preload orders to submit before timing starts.
 */
[[nodiscard]] inline std::vector<Order> make_true_mixed_preload(std::size_t reserve_order_capacity,
                                                                TrueMixedBookModel& model) {
    // Seed enough two-sided liquidity for early random taker flow and live-target operations.
    const auto preload_count = std::clamp<std::size_t>(reserve_order_capacity / 4, 64, 100'000);
    std::mt19937 rng{kTrueMixedSeed ^ 0xB00BU};
    std::vector<Order> preload_orders;
    preload_orders.reserve(preload_count);

    for (std::size_t index = 0; index < preload_count; ++index) {
        const auto side = index % 2 == 0 ? Side::Buy : Side::Sell;
        auto order = make_passive_order(kTrueMixedPreloadIdBase + index, side, rng);

        // Preloaded GTC liquidity is part of the live set used for valid targets.
        model.add_resting_order(order);
        preload_orders.push_back(order);
    }

    return preload_orders;
}

/**
 * @brief Converts operation-count shares into exact integer counts.
 *
 * @param operation_count Number of timed operations.
 * @return Counts for GTC, cancel, modify, IOC, market, and FOK.
 */
[[nodiscard]] inline std::array<std::size_t, 6> true_mixed_operation_counts(
    std::size_t operation_count) {
    constexpr std::array<std::size_t, 6> weights{25, 25, 20, 15, 10, 5};
    std::array<std::size_t, 6> counts{};
    std::array<std::pair<std::size_t, std::size_t>, 6> remainders{};
    std::size_t assigned = 0;

    for (std::size_t index = 0; index < weights.size(); ++index) {
        const auto scaled = operation_count * weights[index];
        counts[index] = scaled / 100;
        remainders[index] = {scaled % 100, index};
        assigned += counts[index];
    }

    std::ranges::sort(remainders, std::greater<>{});
    for (std::size_t index = 0; assigned < operation_count; ++index, ++assigned) {
        // Largest remainders receive leftover operations to keep the mix exact.
        ++counts[remainders[index].second];
    }

    return counts;
}

/**
 * @brief Picks the next operation kind from remaining counts and live-state rules.
 *
 * @param remaining Remaining operation counts by kind.
 * @param has_live_orders True when cancel and modify can target the book.
 * @param rng Deterministic generator for interleaving.
 * @return Selected operation kind.
 */
[[nodiscard]] inline TrueMixedOperationKind choose_true_mixed_kind(
    const std::array<std::size_t, 6>& remaining,
    bool has_live_orders,
    std::mt19937& rng) {
    std::array<std::size_t, 6> weights = remaining;
    if (!has_live_orders) {
        // Cancels and modifies must never be generated without a currently live target.
        weights[1] = 0;
        weights[2] = 0;
    }

    std::size_t total_weight = 0;
    for (const auto weight : weights) {
        // Sum only currently valid remaining kinds for weighted random interleaving.
        total_weight += weight;
    }
    if (total_weight == 0) {
        throw std::logic_error{"true mixed generation ran out of valid operations"};
    }

    std::uniform_int_distribution<std::size_t> distribution{1, total_weight};
    auto pick = distribution(rng);
    for (std::size_t index = 0; index < weights.size(); ++index) {
        if (pick <= weights[index]) {
            return static_cast<TrueMixedOperationKind>(index);
        }
        pick -= weights[index];
    }

    return TrueMixedOperationKind::GtcLimit;
}

/**
 * @brief Creates a passive GTC operation and updates generation state.
 *
 * @param model Generation model to update.
 * @param next_resting_id Next timed GTC id.
 * @param rng Deterministic generator.
 * @return Generated operation.
 */
[[nodiscard]] inline TrueMixedOperation make_gtc_operation(TrueMixedBookModel& model,
                                                          OrderId& next_resting_id,
                                                          std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    auto order =
        make_passive_order(next_resting_id++, side_distribution(rng) ? Side::Buy : Side::Sell, rng);

    // Timed GTC submits rest passively and become future cancel/modify targets.
    model.add_resting_order(order);
    return TrueMixedOperation{.kind = TrueMixedOperationKind::GtcLimit, .order = order};
}

/**
 * @brief Creates a live cancel operation and updates generation state.
 *
 * @param model Generation model to update.
 * @param rng Deterministic generator.
 * @return Generated operation.
 */
[[nodiscard]] inline TrueMixedOperation make_cancel_operation(TrueMixedBookModel& model,
                                                             std::mt19937& rng) {
    const auto order_id = model.random_live_order_id(rng);

    // Remove the target immediately so later generated operations cannot reuse a dead id.
    model.remove_order(order_id);
    return TrueMixedOperation{.kind = TrueMixedOperationKind::Cancel, .target_order_id = order_id};
}

/**
 * @brief Creates a live modify operation and updates generation state.
 *
 * @param model Generation model to update.
 * @param rng Deterministic generator.
 * @return Generated operation.
 */
[[nodiscard]] inline TrueMixedOperation make_modify_operation(TrueMixedBookModel& model,
                                                             std::mt19937& rng) {
    const auto order_id = model.random_live_order_id(rng);
    const auto existing = model.live_order(order_id);
    std::uniform_int_distribution<int> mode_distribution{0, 2};
    const auto mode = mode_distribution(rng);
    Price new_price = existing.price;
    Quantity new_quantity = existing.quantity;

    if (mode == 0 && existing.quantity > 1) {
        // Same-price reductions exercise the in-place priority-preserving modify path.
        new_quantity = std::max<Quantity>(1, existing.quantity / 2);
    } else {
        // Passive repricing and size increases exercise cancel-replace without crossing.
        const auto side = existing.side;
        std::uniform_int_distribution<int> offset_distribution{1, 4};
        const auto offset = offset_distribution(rng);
        new_price = side == Side::Buy ? kTrueMixedMidPrice - offset
                                      : kTrueMixedMidPrice + offset;
        new_quantity = existing.quantity + static_cast<Quantity>(1 + mode);
    }

    model.modify_order(order_id, new_price, new_quantity);
    return TrueMixedOperation{.kind = TrueMixedOperationKind::Modify,
                              .target_order_id = order_id,
                              .new_price = new_price,
                              .new_quantity = new_quantity};
}

/**
 * @brief Creates an aggressive limit or market order.
 *
 * @param model Generation model used to choose realistic prices and quantities.
 * @param kind Aggressive operation kind.
 * @param next_transient_id Next transient order id.
 * @param rng Deterministic generator.
 * @return Generated operation.
 */
[[nodiscard]] inline TrueMixedOperation make_aggressive_operation(TrueMixedBookModel& model,
                                                                 TrueMixedOperationKind kind,
                                                                 OrderId& next_transient_id,
                                                                 std::mt19937& rng) {
    std::bernoulli_distribution side_distribution{0.5};
    std::uniform_int_distribution<int> outcome_distribution{0, 15};
    const auto side = side_distribution(rng) ? Side::Buy : Side::Sell;
    const auto outcome = outcome_distribution(rng);
    const auto best_opposite = model.best_opposite_price(side);
    const bool has_opposite = best_opposite != 0;
    const auto best_level_quantity = model.best_opposite_level_quantity(side);
    Price price = side == Side::Buy ? kTrueMixedMidPrice + 2 : kTrueMixedMidPrice - 2;
    Quantity quantity = 1;

    if (kind != TrueMixedOperationKind::Market) {
        // Some limit takers are priced not to cross, producing clean no-fill rejects/cancels.
        if (!has_opposite || outcome == 3) {
            price = side == Side::Buy ? kTrueMixedMidPrice - 1 : kTrueMixedMidPrice + 1;
        } else {
            price = side == Side::Buy ? best_opposite + 1 : best_opposite - 1;
        }
    }

    Order probe{.id = next_transient_id, .side = side, .price = price, .quantity = 1};
    const auto available = kind == TrueMixedOperationKind::Market
                               ? model.available_crossing_quantity(Order{
                                     .id = next_transient_id,
                                     .side = side,
                                     .price = side == Side::Buy
                                                  ? std::numeric_limits<Price>::max()
                                                  : std::numeric_limits<Price>::min(),
                                     .quantity = 1})
                               : model.available_crossing_quantity(probe);

    if (kind == TrueMixedOperationKind::IocLimit && outcome >= 2) {
        // Rejected IOC orders use a non-crossing price so they do not mutate liquidity.
        price = side == Side::Buy ? kTrueMixedMidPrice - 1 : kTrueMixedMidPrice + 1;
        quantity = kTrueMixedRestingQuantity;
    } else if (kind == TrueMixedOperationKind::FokLimit && outcome != 0) {
        // Rejected FOK orders prove insufficient liquidity without mutating the model.
        quantity = available + kTrueMixedRestingQuantity;
    } else if (kind == TrueMixedOperationKind::IocLimit && outcome == 1 &&
               best_level_quantity > 0) {
        // Oversized IOC flow consumes at most around one best level, then expires the remainder.
        quantity = std::min<Quantity>(best_level_quantity, kTrueMixedRestingQuantity) + 1;
        price = best_opposite;
    } else if (kind == TrueMixedOperationKind::Market && available > 0) {
        // Market flow stays small so it exercises taker matching without exhausting live ids.
        quantity = std::min<Quantity>(available, 5);
    } else if (outcome == 0 && available > 0) {
        // Small taker quantities usually fully execute without draining whole price levels.
        quantity = std::min<Quantity>(available, 5);
    } else {
        // Empty or non-crossing conditions produce deterministic insufficient-liquidity results.
        quantity = available + kTrueMixedRestingQuantity;
    }

    auto order = Order{.id = next_transient_id++,
                       .side = side,
                       .price = price,
                       .quantity = quantity,
                       .time_in_force = TimeInForce::GoodTilCancel};

    if (kind == TrueMixedOperationKind::IocLimit) {
        order.time_in_force = TimeInForce::ImmediateOrCancel;
    } else if (kind == TrueMixedOperationKind::FokLimit) {
        order.time_in_force = TimeInForce::FillOrKill;
    }

    model.execute_aggressive_order(order,
                                   kind == TrueMixedOperationKind::Market,
                                   kind == TrueMixedOperationKind::FokLimit);
    return TrueMixedOperation{.kind = kind, .order = order};
}

/**
 * @brief Builds a deterministic OrderBook-only true mixed workload.
 *
 * @param operation_count Number of timed operations to generate.
 * @return Preload orders, operation stream, reserve hint, and live-depth metadata.
 */
[[nodiscard]] inline TrueMixedWorkload make_true_mixed_workload(std::size_t operation_count) {
    TrueMixedWorkload workload;
    TrueMixedBookModel model;
    std::mt19937 rng{kTrueMixedSeed};
    auto remaining = true_mixed_operation_counts(operation_count);
    auto next_resting_id = OrderId{1};
    auto next_transient_id = kTrueMixedIncomingIdBase;

    workload.reserve_order_capacity = true_mixed_reserve_order_capacity(operation_count);
    workload.preload_orders = make_true_mixed_preload(workload.reserve_order_capacity, model);
    workload.operations.reserve(operation_count);
    workload.max_live_orders = model.live_order_count();

    for (std::size_t index = 0; index < operation_count; ++index) {
        const auto kind = choose_true_mixed_kind(remaining, model.has_live_orders(), rng);
        auto& remaining_count = remaining[static_cast<std::size_t>(kind)];
        --remaining_count;

        if (kind == TrueMixedOperationKind::GtcLimit) {
            workload.operations.push_back(make_gtc_operation(model, next_resting_id, rng));
        } else if (kind == TrueMixedOperationKind::Cancel) {
            workload.operations.push_back(make_cancel_operation(model, rng));
        } else if (kind == TrueMixedOperationKind::Modify) {
            workload.operations.push_back(make_modify_operation(model, rng));
        } else {
            workload.operations.push_back(
                make_aggressive_operation(model, kind, next_transient_id, rng));
        }

        // Track the modeled peak live depth to validate the reserve heuristic later.
        workload.max_live_orders = std::max(workload.max_live_orders, model.live_order_count());
    }

    return workload;
}

/**
 * @brief Preloads deterministic GTC liquidity into a real OrderBook.
 *
 * @param book Book to populate before timing.
 * @param preload_orders Pre-generated preload orders.
 * @param events Reusable caller-owned event buffer.
 */
inline void preload_true_mixed_book(OrderBook& book,
                                    const std::vector<Order>& preload_orders,
                                    std::vector<Event>& events) {
    for (const auto& order : preload_orders) {
        // Preload is setup work and keeps the timed stream focused on hot-path operations.
        book.submit(order, events);
    }
}

/**
 * @brief Executes one pre-generated true mixed operation directly on OrderBook.
 *
 * @param book Book under test.
 * @param operation Operation to apply.
 * @param events Reusable caller-owned event buffer.
 * @return Cancel result when the operation is cancel, otherwise a default result.
 */
[[nodiscard]] inline CancelResult run_true_mixed_operation(OrderBook& book,
                                                          const TrueMixedOperation& operation,
                                                          std::vector<Event>& events) {
    if (operation.kind == TrueMixedOperationKind::Cancel) {
        // Cancel uses the direct single-result path and does not allocate an event vector.
        return book.cancel(operation.target_order_id);
    }

    if (operation.kind == TrueMixedOperationKind::Modify) {
        // Modify reuses the caller-owned event buffer for both in-place and replace events.
        book.modify(operation.target_order_id, operation.new_price, operation.new_quantity, events);
    } else if (operation.kind == TrueMixedOperationKind::Market) {
        // Market orders are transient taker flow and never rest in the live target set.
        book.submit_market(operation.order, events);
    } else {
        // GTC, IOC, and FOK limit submits share the OrderBook submit hot path.
        book.submit(operation.order, events);
    }

    return RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = 0};
}

} // namespace matching_engine::benchmark_workloads
