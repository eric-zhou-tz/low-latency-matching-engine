#include "book/order_book.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace matching_engine {
namespace {

constexpr float kOrderIdMaxLoadFactor = 0.80F;

/**
 * @brief Computes the number of vector slots required for a ladder range.
 */
[[nodiscard]] std::size_t ladder_size_from_range(PriceTick tick_range) noexcept {
    // Negative ranges are invalid at the exchange boundary; keep direct construction bounded.
    if (tick_range < 0) {
        return 0;
    }

    return static_cast<std::size_t>((tick_range * 2) + 1);
}

} // namespace

/**
 * @brief Creates an empty book with fixed order-id lookup tuning.
 */
OrderBook::OrderBook() : bids_(TreeLevels{}), asks_(TreeLevels{}) {
    // Benchmark sweeps selected 0.8 as the stable order-id hash-table density.
    configure_order_id_lookup();
}

/**
 * @brief Creates an empty book and applies a reserve-capacity tuning hint.
 */
OrderBook::OrderBook(std::size_t reserve_order_capacity) : bids_(TreeLevels{}), asks_(TreeLevels{}) {
    // Configure lookup density before reserving so bucket sizing uses the fixed policy.
    configure_order_id_lookup();
    this->reserve_order_capacity(reserve_order_capacity);
}

/**
 * @brief Creates a reserved ladder-backed book with preallocated price slots.
 */
OrderBook::OrderBook(std::size_t reserve_order_capacity,
                     PriceTick base_tick,
                     PriceTick tick_range)
    : bids_(LadderLevels(ladder_size_from_range(tick_range))),
      asks_(LadderLevels(ladder_size_from_range(tick_range))) {
    // Configure metadata and lookup density before reserving storage for live orders.
    configure_order_id_lookup();
    configure_ladder_metadata(base_tick, tick_range);
    this->reserve_order_capacity(reserve_order_capacity);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook::OrderBook(const OrderBook& other) {
    // Copy metadata before live orders so the clone has the same storage-mode declaration.
    copy_from(other);
}

/**
 * @brief Copies price levels and rebuilds intrusive links for the new book.
 */
OrderBook& OrderBook::operator=(const OrderBook& other) {
    if (this == &other) {
        return *this;
    }

    clear();
    copy_from(other);

    return *this;
}

/**
 * @brief Moves a book and preserves pointers into transferred order blocks.
 */
OrderBook::OrderBook(OrderBook&& other) noexcept
    : price_level_mode_(other.price_level_mode_),
      base_tick_(other.base_tick_),
      tick_range_(other.tick_range_),
      min_tick_(other.min_tick_),
      max_tick_(other.max_tick_),
      bids_(std::move(other.bids_)),
      asks_(std::move(other.asks_)),
      orders_by_id_(std::move(other.orders_by_id_)),
      order_pool_(std::move(other.order_pool_)) {
    // Raw order pointers still point into order_pool_ blocks transferred above.
}

/**
 * @brief Moves a book and preserves pointers into transferred order blocks.
 */
OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    price_level_mode_ = other.price_level_mode_;
    base_tick_ = other.base_tick_;
    tick_range_ = other.tick_range_;
    min_tick_ = other.min_tick_;
    max_tick_ = other.max_tick_;
    bids_ = std::move(other.bids_);
    asks_ = std::move(other.asks_);
    orders_by_id_ = std::move(other.orders_by_id_);
    order_pool_ = std::move(other.order_pool_);

    return *this;
}

/**
 * @brief Destroys all owned order storage.
 */
OrderBook::~OrderBook() {
    clear();
}

/**
 * @brief Adds an order to the book, matching any immediately executable volume.
 *
 * Limit orders trade against the best opposite-side prices while they cross the
 * incoming limit. Any unfilled quantity is appended to the relevant resting
 * queue, preserving FIFO priority at that price level.
 */
void OrderBook::submit(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out)) {
        return;
    }

    // fok must prove full execution before acceptance so partial fills cannot leak.
    if (order.time_in_force == TimeInForce::FillOrKill && !can_fully_fill(order)) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::InsufficientLiquidity, .order_id = order.id});
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});

    execute_incoming_order(order, out);
}

/**
 * @brief Matches a market order and expires any leftover quantity.
 *
 * Market orders reuse the limit matching loops by giving the incoming order an
 * unbounded crossing price. The only market-specific behavior is that an
 * unmatched remainder produces a rejection event instead of resting.
 */
void OrderBook::submit_market(Order order, std::vector<Event>& out) {
    if (!prepare_incoming_order(order, out, false)) {
        return;
    }

    out.push_back(AcceptedEvent{.order_id = order.id});

    // market orders use sentinel prices to reuse the same crossing loops but never rest.
    if (order.side == Side::Buy) {
        order.price = std::numeric_limits<Price>::max();
        match_buy_order(order, out);
    } else {
        order.price = std::numeric_limits<Price>::min();
        match_sell_order(order, out);
    }

    if (order.quantity > 0) {
        out.push_back(
            RejectedEvent{.reason = RejectReason::InsufficientLiquidity, .order_id = order.id});
    }
}

/**
 * @brief Removes an order from the current book by id.
 *
 * The id index points directly at the intrusive order node. Cancellation still
 * finds the price level for aggregate cleanup, but FIFO removal is just pointer
 * rewiring and does not allocate or scan through same-price orders.
 */
CancelResult OrderBook::cancel(OrderId order_id) {
    Order* order = find_resting_order(order_id);
    if (order == nullptr) {
        return RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id};
    }

    remove_resting_order(order);
    return CanceledEvent{.order_id = order_id};
}

/**
 * @brief Modifies a live resting order.
 */
void OrderBook::modify(OrderId order_id,
                       Price new_price,
                       Quantity new_quantity,
                       std::vector<Event>& out) {
    out.clear();

    Order* existing = find_resting_order(order_id);
    if (existing == nullptr) {
        out.push_back(RejectedEvent{.reason = RejectReason::UnknownOrderId, .order_id = order_id});
        return;
    }

    if (new_price <= 0 || new_quantity == 0) {
        out.push_back(RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = order_id});
        return;
    }

    if (!price_in_range(new_price)) {
        out.push_back(RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = order_id});
        return;
    }

    const Price old_price = existing->price;
    const Quantity old_quantity = existing->quantity;
    const Side side = existing->side;

    if (new_price == old_price && new_quantity < old_quantity) {
        // a pure size reduction preserves the order's FIFO priority.
        const Quantity reduced_by = old_quantity - new_quantity;
        require_level(side, old_price).total_volume -= reduced_by;

        existing->quantity = new_quantity;
        out.push_back(ModifiedEvent{.order_id = order_id,
                                    .old_price = old_price,
                                    .new_price = new_price,
                                    .old_quantity = old_quantity,
                                    .new_quantity = new_quantity});
        return;
    }

    // size increases and price changes are cancel-replace operations that lose FIFO priority.
    Order replacement{.id = order_id,
                      .side = side,
                      .price = new_price,
                      .quantity = new_quantity,
                      .time_in_force = TimeInForce::GoodTilCancel};
    remove_resting_order(existing);

    out.push_back(ReplacedEvent{.old_order_id = order_id,
                                .new_order_id = order_id,
                                .old_price = old_price,
                                .new_price = new_price,
                                .old_quantity = old_quantity,
                                .new_quantity = new_quantity});

    execute_incoming_order(replacement, out);
}

/**
 * @brief Checks whether an order id is currently live in this book.
 */
bool OrderBook::contains_order(std::uint64_t order_id) const {
    return orders_by_id_.contains(order_id);
}

/**
 * @brief Returns the configured price-level storage mode.
 */
PriceLevelMode OrderBook::price_level_mode() const noexcept {
    // Matching still uses the map containers regardless of this preparation metadata.
    return price_level_mode_;
}

/**
 * @brief Returns the configured ladder base tick.
 */
PriceTick OrderBook::base_tick() const noexcept {
    // Tree-backed books keep this at zero because no ladder window was configured.
    return base_tick_;
}

/**
 * @brief Returns the configured ladder tick range.
 */
PriceTick OrderBook::tick_range() const noexcept {
    // Tree-backed books keep this at zero because no ladder window was configured.
    return tick_range_;
}

/**
 * @brief Returns the internally computed minimum ladder tick.
 */
PriceTick OrderBook::min_tick() const noexcept {
    // The value is derived from base_tick_ and tick_range_ at construction time.
    return min_tick_;
}

/**
 * @brief Returns the internally computed maximum ladder tick.
 */
PriceTick OrderBook::max_tick() const noexcept {
    // The value is derived from base_tick_ and tick_range_ at construction time.
    return max_tick_;
}

/**
 * @brief Returns the current ladder slot count.
 */
std::size_t OrderBook::ladder_size() const noexcept {
    const auto* ladder = std::get_if<LadderLevels>(&bids_);
    if (ladder == nullptr) {
        return 0;
    }

    // Bids and asks are constructed with the same slot count.
    return ladder->size();
}

/**
 * @brief Produces a compact representation of resting orders.
 */
std::string OrderBook::snapshot() const {
    std::ostringstream output;
    output << "orders=" << orders_by_id_.size();

    for (const auto level : ordered_levels(Side::Buy)) {
        for (const Order* order = level.queue->head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    for (const auto level : ordered_levels(Side::Sell)) {
        for (const Order* order = level.queue->head; order != nullptr; order = order->next) {
            output << " [" << order->id << ' ' << to_string(order->side) << ' ' << order->price
                   << 'x' << order->quantity << ']';
        }
    }

    return output.str();
}

/**
 * @brief Copies internal book structure for invariant tests.
 */
OrderBook::DebugSnapshot OrderBook::debug_snapshot() const {
    DebugSnapshot snapshot;
    snapshot.bids.reserve(level_count(Side::Buy));
    snapshot.asks.reserve(level_count(Side::Sell));
    snapshot.indexed_order_ids.reserve(orders_by_id_.size());

    for (const auto& [order_id, _] : orders_by_id_) {
        // The id set lets tests compare queue contents against the live lookup index.
        snapshot.indexed_order_ids.insert(order_id);
    }

    for (const auto source_level : ordered_levels(Side::Buy)) {
        DebugPriceLevel level{.price = source_level.price,
                              .total_volume = source_level.queue->total_volume};

        for (const Order* order = source_level.queue->head; order != nullptr; order = order->next) {
            // Copy only stable logical fields so tests cannot mutate intrusive links.
            level.orders.push_back(DebugOrder{.id = order->id,
                                              .side = order->side,
                                              .price = order->price,
                                              .quantity = order->quantity});
        }

        snapshot.bids.push_back(std::move(level));
    }

    for (const auto source_level : ordered_levels(Side::Sell)) {
        DebugPriceLevel level{.price = source_level.price,
                              .total_volume = source_level.queue->total_volume};

        for (const Order* order = source_level.queue->head; order != nullptr; order = order->next) {
            // Copy only stable logical fields so tests cannot mutate intrusive links.
            level.orders.push_back(DebugOrder{.id = order->id,
                                              .side = order->side,
                                              .price = order->price,
                                              .quantity = order->quantity});
        }

        snapshot.asks.push_back(std::move(level));
    }

    return snapshot;
}

/**
 * @brief Reserves lookup and pool storage from a caller-provided tuning hint.
 */
void OrderBook::reserve_order_capacity(std::size_t reserve_order_capacity) {
    // Reserve sizing is a cache/locality tuning knob, not a count of operations processed.
    orders_by_id_.reserve(reserve_order_capacity);

    // Over-reserving can spread hot structures over more memory and slow steady-state matching.
    order_pool_.reserve(reserve_order_capacity);
}

/**
 * @brief Applies the fixed max load factor used by the order-id map.
 */
void OrderBook::configure_order_id_lookup() noexcept {
    // Keep the former benchmark winner as a code default instead of a public tuning knob.
    orders_by_id_.max_load_factor(kOrderIdMaxLoadFactor);
}

/**
 * @brief Stores ladder metadata for vector-backed price levels.
 */
void OrderBook::configure_ladder_metadata(PriceTick base_tick, PriceTick tick_range) noexcept {
    // The bounds are derived once so ladder helpers can validate and index in constant time.
    price_level_mode_ = PriceLevelMode::Ladder;
    base_tick_ = base_tick;
    tick_range_ = tick_range;
    min_tick_ = base_tick_ - tick_range_;
    max_tick_ = base_tick_ + tick_range_;
}

/**
 * @brief Appends an order to the appropriate price level.
 */
void OrderBook::add_resting_order(const Order& order) {
    Order* stored_order = order_pool_.create(order);
    OrderQueue* level = get_or_create_level(order.side, order.price);
    if (level == nullptr) {
        // Validation should reject out-of-range ladder orders before storage is touched.
        order_pool_.release(stored_order);
        return;
    }

    // Appending to the level preserves FIFO priority behind older same-price orders.
    level->push_back(stored_order);

    orders_by_id_.emplace(stored_order->id, stored_order);
}

/**
 * @brief Finds the live resting order for an id.
 */
Order* OrderBook::find_resting_order(OrderId order_id) const {
    const auto found = orders_by_id_.find(order_id);
    if (found == orders_by_id_.end()) {
        return nullptr;
    }

    return found->second;
}

/**
 * @brief Unlinks and releases one resting order.
 */
void OrderBook::remove_resting_order(Order* order) {
    const auto side = order->side;
    const auto price = order->price;

    OrderQueue* level = find_level(side, price);
    if (level != nullptr) {
        // Remove the intrusive node, then drop the price level if it lost its final order.
        level->remove(order);
        erase_level_if_empty(side, price);
    }

    orders_by_id_.erase(order->id);
    order_pool_.release(order);
}

/**
 * @brief Resets incoming order links and rejects duplicate live ids.
 */
bool OrderBook::prepare_incoming_order(Order& order,
                                       std::vector<Event>& out,
                                       bool validate_limit_price) const {
    out.clear();

    order.prev = nullptr;
    order.next = nullptr;

    if (orders_by_id_.contains(order.id)) {
        out.push_back(RejectedEvent{.reason = RejectReason::DuplicateOrderId, .order_id = order.id});
        return false;
    }

    if (validate_limit_price && !price_in_range(order.price)) {
        out.push_back(RejectedEvent{.reason = RejectReason::InvalidOrder, .order_id = order.id});
        return false;
    }

    return true;
}

/**
 * @brief Checks whether crossing liquidity can completely fill an order.
 */
bool OrderBook::can_fully_fill(const Order& order) const {
    // The helper owns side ordering so the preflight stays independent of map layout.
    return has_crossing_liquidity(order);
}

/**
 * @brief Executes the shared limit-order match and rest path.
 */
void OrderBook::execute_incoming_order(Order order, std::vector<Event>& out) {
    if (order.side == Side::Buy) {
        match_buy_order(order, out);
    } else {
        match_sell_order(order, out);
    }

    if (order.quantity > 0 && order.time_in_force == TimeInForce::GoodTilCancel) {
        add_resting_order(order);
    }
}

/**
 * @brief Clears all price levels, indexes, and arena storage.
 */
void OrderBook::clear() noexcept {
    clear_price_levels();
    orders_by_id_.clear();

    order_pool_.clear();
}

/**
 * @brief Copies live resting orders from another book.
 */
void OrderBook::copy_from(const OrderBook& other) {
    // Preserve storage-mode metadata so copied books behave as the same configured symbol.
    price_level_mode_ = other.price_level_mode_;
    base_tick_ = other.base_tick_;
    tick_range_ = other.tick_range_;
    min_tick_ = other.min_tick_;
    max_tick_ = other.max_tick_;

    if (price_level_mode_ == PriceLevelMode::Ladder) {
        // Recreate empty ladder vectors before replaying live levels into their slots.
        bids_ = LadderLevels(other.ladder_size());
        asks_ = LadderLevels(other.ladder_size());
    } else {
        // Tree copies start from empty maps so erased source levels are not retained.
        bids_ = TreeLevels{};
        asks_ = TreeLevels{};
    }

    configure_order_id_lookup();
    reserve_order_capacity(other.orders_by_id_.size());

    for (const auto source_level : other.ordered_levels(Side::Buy)) {
        OrderQueue* target_level = get_or_create_level(Side::Buy, source_level.price);
        if (target_level == nullptr) {
            continue;
        }
        for (const Order* source = source_level.queue->head; source != nullptr;
             source = source->next) {
            Order* clone = order_pool_.create(*source);
            target_level->push_back(clone);
            orders_by_id_.emplace(clone->id, clone);
        }
    }

    for (const auto source_level : other.ordered_levels(Side::Sell)) {
        OrderQueue* target_level = get_or_create_level(Side::Sell, source_level.price);
        if (target_level == nullptr) {
            continue;
        }
        for (const Order* source = source_level.queue->head; source != nullptr;
             source = source->next) {
            Order* clone = order_pool_.create(*source);
            target_level->push_back(clone);
            orders_by_id_.emplace(clone->id, clone);
        }
    }
}

/**
 * @brief Consumes resting asks while their price is at or below the buy limit.
 *
 * The front pointer is the oldest order at that price, so matching from front
 * to back preserves FIFO price-time priority.
 */
void OrderBook::match_buy_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !levels_empty(Side::Sell)) {
        const auto best_ask = best_level(Side::Sell);
        if (!best_ask.has_value() || best_ask->price > incoming.price) {
            break;
        }

        auto& resting_orders = *best_ask->queue;
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // executions price at the resting order, preserving exchange-style maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting->id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting->price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        if (resting->quantity == 0) {
            // filled resting orders leave the id index before their pool slot is recycled.
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
        }

        if (resting_orders.empty()) {
            erase_level(Side::Sell, best_ask->price);
        }
    }
}

/**
 * @brief Consumes resting bids while their price is at or above the sell limit.
 *
 * The best-level helper selects the highest bid while storage remains ascending.
 */
void OrderBook::match_sell_order(Order& incoming, std::vector<Event>& out) {
    while (incoming.quantity > 0 && !levels_empty(Side::Buy)) {
        const auto best_bid = best_level(Side::Buy);
        if (!best_bid.has_value() || best_bid->price < incoming.price) {
            break;
        }

        auto& resting_orders = *best_bid->queue;
        Order* resting = resting_orders.front();
        const auto trade_quantity = std::min(incoming.quantity, resting->quantity);

        // executions price at the resting order, preserving exchange-style maker priority.
        out.push_back(TradeEvent{.resting_order_id = resting->id,
                                 .incoming_order_id = incoming.id,
                                 .price = resting->price,
                                 .quantity = trade_quantity});

        incoming.quantity -= trade_quantity;
        resting->quantity -= trade_quantity;
        resting_orders.total_volume -= trade_quantity;

        if (resting->quantity == 0) {
            // filled resting orders leave the id index before their pool slot is recycled.
            orders_by_id_.erase(resting->id);
            resting_orders.pop_front();
            order_pool_.release(resting);
        }

        if (resting_orders.empty()) {
            erase_level(Side::Buy, best_bid->price);
        }
    }
}

} // namespace matching_engine
