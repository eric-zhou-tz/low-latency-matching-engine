#pragma once

#include "book/order_pool.hpp"
#include "book/order_queue.hpp"
#include "core/event.hpp"
#include "core/order.hpp"

#include <ankerl/unordered_dense.h>
#include <absl/container/btree_map.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace matching_engine {

/**
 * @brief Stores orders for a single symbol and emits domain events.
 *
 * The book is organized as side-specific price levels. Bids and asks are stored
 * separately because they have opposite best-price rules, while each price level
 * preserves FIFO order for price-time priority matching.
 */
class OrderBook {
public:
    /**
     * @brief Creates an empty order book.
     */
    OrderBook();

    /**
     * @brief Creates an empty order book with reserved order capacity.
     *
     * @param reserve_order_capacity Caller-selected preallocation hint.
     */
    explicit OrderBook(std::size_t reserve_order_capacity);

    /**
     * @brief Creates a reserved book with vector-backed ladder price storage.
     *
     * @param reserve_order_capacity Caller-selected preallocation hint.
     * @param base_tick Center tick used to derive the ladder window.
     * @param tick_range Distance from the base tick to each side of the window.
     */
    OrderBook(std::size_t reserve_order_capacity, PriceTick base_tick, PriceTick tick_range);

    /**
     * @brief Copies a book and rebuilds intrusive order links.
     *
     * @param other Book to copy.
     */
    OrderBook(const OrderBook& other);

    /**
     * @brief Copies a book and rebuilds intrusive order links.
     *
     * @param other Book to copy.
     * @return This book.
     */
    OrderBook& operator=(const OrderBook& other);

    /**
     * @brief Moves a book while preserving raw order pointers into owned blocks.
     *
     * @param other Book to move.
     */
    OrderBook(OrderBook&& other) noexcept;

    /**
     * @brief Moves a book while preserving raw order pointers into owned blocks.
     *
     * @param other Book to move.
     * @return This book.
     */
    OrderBook& operator=(OrderBook&& other) noexcept;

    /**
     * @brief Destroys the order book.
     */
    ~OrderBook();

    /**
     * @brief Adds an order to the book.
     *
     * @param order Order to store.
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void submit(Order order, std::vector<Event>& out);

    /**
     * @brief Matches a market order without resting any unfilled remainder.
     *
     * @param order Market order to match immediately.
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void submit_market(Order order, std::vector<Event>& out);

    /**
     * @brief Cancels an existing order by id.
     *
     * @param order_id Identifier to cancel.
     * @return Single event describing whether the cancel succeeded.
     */
    [[nodiscard]] CancelResult cancel(OrderId order_id);

    /**
     * @brief Modifies a resting order using exchange-style priority rules.
     *
     * @param order_id Identifier of the resting order to modify.
     * @param new_price Replacement limit price.
     * @param new_quantity Replacement total remaining quantity.
     * @param out Caller-owned event buffer filled with the operation result.
     */
    void modify(OrderId order_id, Price new_price, Quantity new_quantity, std::vector<Event>& out);

    /**
     * @brief Checks whether an order id is currently resting in this book.
     *
     * @param order_id Identifier to look up.
     * @return True when the order is live in this book.
     */
    [[nodiscard]] bool contains_order(std::uint64_t order_id) const;

    /**
     * @brief Returns the configured price-level storage mode.
     *
     * @return Tree for map-backed books or Ladder for vector-backed books.
     */
    [[nodiscard]] PriceLevelMode price_level_mode() const noexcept;

    /**
     * @brief Returns the ladder base tick metadata.
     *
     * @return Center tick configured when ladder mode was selected.
     */
    [[nodiscard]] PriceTick base_tick() const noexcept;

    /**
     * @brief Returns the ladder range metadata.
     *
     * @return Tick distance from the base to each side of the ladder window.
     */
    [[nodiscard]] PriceTick tick_range() const noexcept;

    /**
     * @brief Returns the internally derived minimum ladder tick.
     *
     * @return base_tick - tick_range for ladder-backed books.
     */
    [[nodiscard]] PriceTick min_tick() const noexcept;

    /**
     * @brief Returns the internally derived maximum ladder tick.
     *
     * @return base_tick + tick_range for ladder-backed books.
     */
    [[nodiscard]] PriceTick max_tick() const noexcept;

    /**
     * @brief Returns the number of slots in a ladder-backed book.
     *
     * @return Configured ladder slot count, or zero for tree-backed books.
     */
    [[nodiscard]] std::size_t ladder_size() const noexcept;

    /**
     * @brief Builds a compact textual snapshot of the current book.
     *
     * @return Human-readable book summary.
     */
    [[nodiscard]] std::string snapshot() const;

    /**
     * @brief Immutable order metadata used by tests to validate book structure.
     */
    struct DebugOrder {
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity quantity{};
    };

    /**
     * @brief Immutable price-level metadata used by tests to validate queues.
     */
    struct DebugPriceLevel {
        Price price{};
        Quantity total_volume{};
        std::vector<DebugOrder> orders;
    };

    /**
     * @brief Immutable book metadata used by tests to validate indexes and levels.
     */
    struct DebugSnapshot {
        std::vector<DebugPriceLevel> bids;
        std::vector<DebugPriceLevel> asks;
        std::unordered_set<OrderId> indexed_order_ids;
    };

    /**
     * @brief Copies internal book structure into a validation-only snapshot.
     *
     * @return Snapshot of price levels, live orders, and order-id index entries.
     */
    [[nodiscard]] DebugSnapshot debug_snapshot() const;

    /**
     * @brief Reserves order-id lookup and order-pool capacity.
     *
     * @param reserve_order_capacity Caller-selected preallocation hint.
     */
    void reserve_order_capacity(std::size_t reserve_order_capacity);

private:
    using TreeLevels = absl::btree_map<PriceTick, OrderQueue>;
    using LadderLevels = std::vector<OrderQueue>;
    using PriceLevels = std::variant<TreeLevels, LadderLevels>;

    /**
     * @brief Mutable reference to one price level selected by helper logic.
     */
    struct PriceLevelRef {
        PriceTick price{};
        OrderQueue* queue{};
    };

    /**
     * @brief Immutable reference to one price level selected by helper logic.
     */
    struct ConstPriceLevelRef {
        PriceTick price{};
        const OrderQueue* queue{};
    };

    /**
     * @brief Returns the configured price levels for a side.
     *
     * @param side Side whose price levels should be selected.
     * @return Mutable price levels.
     */
    [[nodiscard]] PriceLevels& levels_for(Side side);

    /**
     * @brief Returns the configured price levels for a side.
     *
     * @param side Side whose price levels should be selected.
     * @return Immutable price levels.
     */
    [[nodiscard]] const PriceLevels& levels_for(Side side) const;

    /**
     * @brief Checks whether a price belongs to the configured ladder window.
     *
     * @param price Price tick to inspect.
     * @return True when tree mode is active or the ladder can index the price.
     */
    [[nodiscard]] bool price_in_range(PriceTick price) const noexcept;

    /**
     * @brief Converts an in-range price tick to a ladder slot.
     *
     * @param price Price tick to index.
     * @return Zero-based ladder index.
     */
    [[nodiscard]] std::size_t ladder_index(PriceTick price) const noexcept;

    /**
     * @brief Finds an existing price level without creating it.
     *
     * @param side Side whose levels should be searched.
     * @param price Limit price to find.
     * @return Mutable queue pointer, or nullptr when absent.
     */
    [[nodiscard]] OrderQueue* find_level(Side side, PriceTick price);

    /**
     * @brief Finds an existing price level without creating it.
     *
     * @param side Side whose levels should be searched.
     * @param price Limit price to find.
     * @return Immutable queue pointer, or nullptr when absent.
     */
    [[nodiscard]] const OrderQueue* find_level(Side side, PriceTick price) const;

    /**
     * @brief Finds or creates the price level for a resting order.
     *
     * @param side Side whose levels should be updated.
     * @param price Limit price to find or create.
     * @return Mutable queue for the requested level.
     */
    [[nodiscard]] OrderQueue* get_or_create_level(Side side, PriceTick price);

    /**
     * @brief Returns an existing level that book indexes already require to exist.
     *
     * @param side Side whose levels should be searched.
     * @param price Limit price to require.
     * @return Mutable queue for the requested level.
     */
    [[nodiscard]] OrderQueue& require_level(Side side, PriceTick price);

    /**
     * @brief Returns an existing level that book indexes already require to exist.
     *
     * @param side Side whose levels should be searched.
     * @param price Limit price to require.
     * @return Immutable queue for the requested level.
     */
    [[nodiscard]] const OrderQueue& require_level(Side side, PriceTick price) const;

    /**
     * @brief Removes a price level if it exists.
     *
     * @param side Side whose levels should be updated.
     * @param price Limit price to erase.
     */
    void erase_level(Side side, PriceTick price);

    /**
     * @brief Removes a price level only when its queue has become empty.
     *
     * @param side Side whose levels should be updated.
     * @param price Limit price to inspect.
     */
    void erase_level_if_empty(Side side, PriceTick price);

    /**
     * @brief Returns the currently best price level for a side.
     *
     * @param side Side whose best level should be selected.
     * @return Mutable best-level reference, or empty when the side has no liquidity.
     */
    [[nodiscard]] std::optional<PriceLevelRef> best_level(Side side);

    /**
     * @brief Returns the currently best price level for a side.
     *
     * @param side Side whose best level should be selected.
     * @return Immutable best-level reference, or empty when the side has no liquidity.
     */
    [[nodiscard]] std::optional<ConstPriceLevelRef> best_level(Side side) const;

    /**
     * @brief Reports whether one side has no price levels.
     *
     * @param side Side whose levels should be checked.
     * @return True when the side has no levels.
     */
    [[nodiscard]] bool levels_empty(Side side) const;

    /**
     * @brief Reports how many price levels exist on one side.
     *
     * @param side Side whose levels should be counted.
     * @return Number of non-empty price levels.
     */
    [[nodiscard]] std::size_t level_count(Side side) const;

    /**
     * @brief Returns side levels in matching priority order.
     *
     * @param side Side whose levels should be ordered.
     * @return Price-level references ordered from best to worst.
     */
    [[nodiscard]] std::vector<ConstPriceLevelRef> ordered_levels(Side side) const;

    /**
     * @brief Checks whether crossing levels contain enough visible quantity.
     *
     * @param order Incoming order to preflight.
     * @return True when the opposite side can fully fill the order.
     */
    [[nodiscard]] bool has_crossing_liquidity(const Order& order) const;

    /**
     * @brief Clears both sides of configured price storage.
     */
    void clear_price_levels() noexcept;

    /**
     * @brief Applies fixed hash-table tuning chosen by benchmark validation.
     */
    void configure_order_id_lookup() noexcept;

    /**
     * @brief Stores ladder metadata for vector-backed price levels.
     *
     * @param base_tick Center tick for the ladder window.
     * @param tick_range Tick distance to each side of the window.
     */
    void configure_ladder_metadata(PriceTick base_tick, PriceTick tick_range) noexcept;

    /**
     * @brief Clears all book state and owned order storage.
     */
    void clear() noexcept;

    /**
     * @brief Copies live resting orders from another book.
     *
     * @param other Book to copy from.
     */
    void copy_from(const OrderBook& other);

    /**
     * @brief Adds an unmatched order to its side-specific price level.
     *
     * @param order Order that should rest on the book.
     */
    void add_resting_order(const Order& order);

    /**
     * @brief Finds a live resting order by id.
     *
     * @param order_id Identifier to look up.
     * @return Pointer to the live order, or nullptr when missing.
     */
    [[nodiscard]] Order* find_resting_order(OrderId order_id) const;

    /**
     * @brief Removes one live resting order from its queue, index, and storage.
     *
     * @param order Resting order node to remove.
     */
    void remove_resting_order(Order* order);

    /**
     * @brief Prepares an incoming order and emits the acceptance or rejection.
     *
     * @param order Incoming order whose intrusive links should be reset.
     * @param out Caller-owned event buffer filled with initial events.
     * @param validate_limit_price True when the order's limit price must be ladder-addressable.
     * @return True when matching may continue.
     */
    [[nodiscard]] bool prepare_incoming_order(Order& order,
                                              std::vector<Event>& out,
                                              bool validate_limit_price = true) const;

    /**
     * @brief Checks whether a limit order can execute its full quantity now.
     *
     * @param order Incoming limit order to test against opposite-side liquidity.
     * @return True when crossing price levels hold enough visible quantity.
     */
    [[nodiscard]] bool can_fully_fill(const Order& order) const;

    /**
     * @brief Matches an incoming order and rests any remaining GTC quantity.
     *
     * @param order Incoming order to execute.
     * @param out Output event collection for generated trades.
     */
    void execute_incoming_order(Order order, std::vector<Event>& out);

    /**
     * @brief Matches an incoming buy order against resting asks.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param out Output event collection for generated trades.
     */
    void match_buy_order(Order& incoming, std::vector<Event>& out);

    /**
     * @brief Matches an incoming sell order against resting bids.
     *
     * @param incoming Mutable incoming order whose remaining quantity is reduced.
     * @param out Output event collection for generated trades.
     */
    void match_sell_order(Order& incoming, std::vector<Event>& out);

    PriceLevelMode price_level_mode_ = PriceLevelMode::Tree;
    PriceTick base_tick_ = 0;
    PriceTick tick_range_ = 0;
    PriceTick min_tick_ = 0;
    PriceTick max_tick_ = 0;

    // Price levels are accessed only through helpers so map and ladder semantics stay aligned.
    PriceLevels bids_;
    PriceLevels asks_;

    // order ids point at intrusive nodes owned by the pool, giving cancels direct unlink targets.
    ankerl::unordered_dense::map<std::uint64_t, Order*> orders_by_id_;
    OrderPool order_pool_;
};

} // namespace matching_engine
