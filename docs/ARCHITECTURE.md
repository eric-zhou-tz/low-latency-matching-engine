# Matching Engine Architecture

## Overview

This repository implements a low-latency C++ matching engine for exchange-style limit order matching. The engine follows deterministic price-time priority: better prices match first, and orders at the same price match in FIFO arrival order.

Orders are processed synchronously through an event-driven command flow. A parsed action is applied to the exchange, routed to an order book, and converted into domain events that describe the observable result of the operation.

The current implementation is intentionally focused on correctness, deterministic behavior, and clean systems design with targeted hot-path optimizations where they make the matching rules clearer. It uses ordered standard-library maps for price levels, intrusive FIFO queues for per-price order priority, and pooled order storage so cancellation and matching remain easy to inspect while avoiding avoidable node allocation.

## Core Components

### Exchange

`Exchange` is the command-processing boundary. It receives typed `Action` values, owns order books by symbol, routes submissions to the correct book, and writes emitted `Event` values into a caller-owned buffer.

The exchange also maintains a live order index:

```cpp
ankerl::unordered_dense::map<OrderId, OrderBook*> order_to_book_;
```

The index maps each live order ID to the single-symbol `OrderBook` that owns the resting order, so cancel routing can go directly to the correct book instead of scanning all symbols. Symbol books are heap-owned by `std::unique_ptr<OrderBook>`, which keeps the stored `OrderBook*` values stable even if the symbol map rehashes.

### OrderBook

`OrderBook` owns the resting liquidity for a single symbol. It stores bid and
ask price levels, performs crossing checks, executes matching, rests unfilled
quantity, cancels live orders, and writes the events produced by those state
transitions. Submissions can produce an event stream because one incoming order
can accept and then trade against several resting orders. Cancellation produces
exactly one `CancelResult`.

### Parser

`Parser` converts textual commands into typed actions. It keeps input handling separate from exchange logic, which allows matching behavior to be tested independently from command parsing.

Supported commands currently include:

```text
SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity>
CANCEL <id>
PRINT
```

### Actions

Actions represent validated command intent before it mutates exchange state. The current action set is implemented as a `std::variant`:

```cpp
using Action = std::variant<SubmitOrderAction, CancelOrderAction, PrintBookAction>;
```

This keeps command dispatch explicit and makes replay-style processing straightforward.

### Events

Events describe the observable results of applying an action. The matching core writes events instead of printing directly, which separates domain behavior from presentation and makes the engine easier to test, replay, and integrate.

Submissions use a caller-owned reusable event buffer because one submitted order
can emit acceptance plus several trade events:

```cpp
void submit(Order order, std::vector<Event>& out);
```

Cancellation stays on a direct single-result API:

```cpp
CancelResult cancel(OrderId order_id);
```

## Order Book Data Structures

The book is split into independent bid and ask trees:

```cpp
std::map<std::int64_t, OrderQueue, std::greater<>> bids_;
std::map<std::int64_t, OrderQueue> asks_;
```

The bid side uses descending price order, so `bids_.begin()` is the highest bid. The ask side uses ascending price order, so `asks_.begin()` is the lowest ask. This makes best-price access direct and keeps crossing logic simple.

Each price level stores an intrusive FIFO queue:

```cpp
class OrderQueue {
public:
    Order* head;
    Order* tail;
    std::uint64_t total_volume;
};
```

New resting orders are appended at `tail`. Matching consumes from `head`. This preserves time priority for all orders resting at the same price level.

The book also maintains a live order lookup:

```cpp
ankerl::unordered_dense::map<std::uint64_t, Order*> orders_by_id_;
```

`orders_by_id_` stores a raw pointer to the live `Order`. The order embeds its own `prev` and `next` links, so cancellation can unlink the exact resting node without scanning the same-price FIFO queue or allocating a separate `std::list` node.

The order-id lookup uses a flat open-addressing hash map because cancel-heavy
matching-engine workloads tend to be sensitive to cache misses. A node-based
hash map such as `std::unordered_map` stores each element in a separately
allocated node, so a lookup can bounce from the bucket array to unrelated heap
locations before reaching the `Order*`. Flat maps keep control metadata and
key/value storage in contiguous arrays, which is why they are commonly preferred
for hot low-latency indexes when the workload can tolerate their tradeoffs. The
main tradeoff is weaker iterator and reference stability around insert/erase and
rehash operations; this book only stores `Order*` values in the map, so order
object lifetime remains owned by `OrderPool`.

Live orders are owned by `OrderPool`:

```cpp
OrderPool order_pool_;
```

`OrderPool` allocates contiguous blocks of order slots and reuses canceled or filled slots through an internal free list. `OrderBook` owns the pool, while `OrderQueue` and `orders_by_id_` only hold raw non-owning pointers into that pool.

Balanced trees are used because they provide deterministic ordered price levels, efficient insertion/removal, and direct access to the current best price. Intrusive FIFO queues are used because the exchange priority rule within a price level is arrival order, not order ID or quantity, while embedded links keep cancel removal O(1) with lower allocator overhead than node-based standard containers.

Together, these structures implement price-time priority:

1. The tree selects the best available price.
2. The price-level head pointer selects the oldest order at that price.
3. Matching removes or reduces resting orders in that exact priority order.

## Matching Flow

An incoming order follows a synchronous lifecycle:

1. Validate order identity against the live order lookup.
2. Check whether the incoming limit crosses the best opposite-side price.
3. Match against the opposite side while price and quantity allow execution.
4. Generate `TradeEvent` values for each fill.
5. Reduce incoming and resting quantities for partial fills.
6. Rest any remaining incoming quantity on its own side of the book.

### Aggressive Orders

An aggressive order crosses the spread and executes immediately against resting liquidity. A buy order is aggressive when its limit price is greater than or equal to the best ask. A sell order is aggressive when its limit price is less than or equal to the best bid.

Aggressive orders always match against the best opposite price first. Within that price level, they consume the oldest resting order first. Trades execute at the resting order price, which is the standard exchange-style behavior implemented by the current book.

If an aggressive order is larger than the first resting order, the resting order is fully removed and matching continues against the next order at the same price or the next best price level. If the incoming order is fully filled, it never rests on the book.

### Passive Orders

A passive order does not cross the opposite side. A buy order is passive when the best ask is above its limit price or no asks exist. A sell order is passive when the best bid is below its limit price or no bids exist.

Passive orders are inserted into the price tree for their side and appended to the tail of the intrusive queue at their price level. The stored `Order*` is added to `orders_by_id_`, which gives older resting orders at the same price priority over newer orders while preserving direct cancel access.

`OrderPool` creates the resting order slot before queue insertion. The order book then links that pointer into the price-level queue and records the same pointer in `orders_by_id_`.

## Cancel Flow

Cancellation starts with an order ID lookup. Inside an `OrderBook`, the `orders_by_id_` index returns the exact live `Order*`, whose side, price, and embedded links identify where the order is resting.

The book then:

1. Finds the relevant bid or ask price level.
2. Uses the embedded `prev`/`next` pointers to unlink the order from the FIFO queue.
3. Removes the price level if the queue becomes empty.
4. Erases the order ID from the live order lookup.
5. Returns the order slot to `OrderPool`.
6. Returns a single `CanceledEvent` result for a successful cancellation.

Unknown IDs are rejected with a `RejectedEvent`. At the exchange layer, cancellation first probes the dense `order_to_book_` index and then calls `OrderBook::cancel` only on the owning symbol book.

The previous same-price cancellation path was `O(log P + Q)`: find the price level, then scan the queue. The current intrusive design keeps the ordered price-level lookup and makes queue removal `O(1)`, so `OrderBook` cancellation is `O(log P)` after the average `O(1)` order-ID hash lookup.

`OrderBook::cancel` returns:

```cpp
using CancelResult = std::variant<CanceledEvent, RejectedEvent>;
```

This avoids passing an event vector through the cancel hot path. Submissions
still use caller-owned vectors because one submitted order can produce multiple
events.

## Event System

The current event system is implemented as a `std::variant`:

```cpp
using Event = std::variant<
    TradeEvent,
    AcceptedEvent,
    CanceledEvent,
    RejectedEvent,
    BookSnapshotEvent>;
```

Current events include:

| Event | Purpose |
| --- | --- |
| `AcceptedEvent` | Reports that an order was accepted using a structured order id payload. |
| `RejectedEvent` | Reports invalid operations using a structured `RejectReason` plus order id. |
| `TradeEvent` | Reports an execution between an incoming order and a resting order. |
| `CanceledEvent` | Reports successful removal of a resting order. |
| `BookSnapshotEvent` | Carries snapshot display text for the non-hot-path print command. |

Event-driven design is useful because it keeps mutation and observation separate. The matching engine can update internal state and write a precise event stream without depending on terminal output, logging, networking, or persistence code.

Hot-path events carry structured data instead of preformatted strings. Display
strings such as `"accepted order 42"` and `"unknown order id 42"` are created by
`format_event()` at the presentation boundary. Caller-owned submit buffers avoid
repeated event-vector construction while preserving explicit event
materialization. Cancellation uses `CancelResult` directly because it emits only
one result.

## Complexity Analysis

| Operation            | Complexity                      | Notes                           |
| -------------------- | ------------------------------- | ------------------------------- |
| Insert resting order | O(log P) amortized              | Pool slot creation, tree insertion, intrusive tail append |
| Best bid/ask lookup  | O(1)                            | Front tree access via `begin()` |
| Match execution      | O(K)                            | K = fills generated             |
| Cancel by ID         | O(1) average + O(log P) + O(1)  | Hash lookup, price-level lookup, intrusive unlink, pool release |

Definitions:

- `P` = number of price levels on a side of the book.
- `K` = number of matched resting orders that generate fills.
- `Q` = number of resting orders at one price level.

The old deque-based cancel path had an additional O(Q) queue scan. The current intrusive index stores raw `Order*` values, so cancellation no longer depends on same-price queue depth inside the `OrderBook`. The exchange-level `order_to_book_` index also removes the previous cross-symbol scan before entering the book. Pool allocation is amortized by fixed-size blocks, cancel/match release paths do not call the general-purpose allocator, and cancel returns a single result instead of using an event vector.

## Determinism

The engine uses integer-only price and quantity fields:

```cpp
std::int64_t price;
std::uint64_t quantity;
```

This avoids floating-point rounding behavior in the matching core. Combined with ordered price trees, FIFO queues, synchronous processing, and explicit event emission, the same input action sequence produces the same state transitions and event stream.

Determinism matters in trading systems because order matching must be auditable, replayable, and explainable. A deterministic engine can reproduce historical behavior from an input log, support precise debugging, and provide stable correctness tests for edge cases such as partial fills, price-level exhaustion, and duplicate IDs.

## Future Improvements

Future work should remain separate from the current correctness-focused implementation. Potential improvements include:

- Further tuning of the order pool block size after broader Linux benchmark validation.
- Further exchange-level cancel metadata tuning after measuring multi-symbol workloads.
- Consider an event sink/callback API for submission if future profiling shows caller-owned event buffers are still material on multi-fill workloads.
- Lock-free or concurrent designs for higher-throughput deployments.
- Network ingress and session management.
- Binary protocols for lower parsing overhead.
- Persistence and replay logging.
- Release-mode latency benchmarking on Linux.
- Multi-symbol scaling with explicit ownership and routing.

## Design Philosophy

This engine prioritizes correctness before optimization. The implementation makes matching rules explicit, preserves deterministic behavior, and uses systems-level data structures that map directly to exchange concepts.

The architecture favors a clear exchange-style design over framework abstraction: parsed actions enter the exchange, order books mutate deterministic state, and events describe the result. That separation keeps the core matching logic small, inspectable, and ready for targeted optimization when the correctness baseline is stable.
