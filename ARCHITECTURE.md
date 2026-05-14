# Matching Engine Architecture

## Overview

This repository implements a low-latency C++ matching engine for exchange-style limit order matching. The engine follows deterministic price-time priority: better prices match first, and orders at the same price match in FIFO arrival order.

Orders are processed synchronously through an event-driven command flow. A parsed action is applied to the exchange, routed to an order book, and converted into domain events that describe the observable result of the operation.

The current implementation is intentionally focused on correctness, deterministic behavior, and clean systems design before advanced optimization. It uses simple standard-library data structures with explicit control flow so the matching rules, cancellation path, and emitted events are easy to inspect and test.

## Core Components

### Exchange

`Exchange` is the command-processing boundary. It receives typed `Action` values, owns order books by symbol, routes submissions to the correct book, and returns emitted `Event` values to the caller.

The current cancel path searches known symbol books until one recognizes the order ID. This keeps the interface simple while leaving room for a future exchange-level order ID index.

### OrderBook

`OrderBook` owns the resting liquidity for a single symbol. It stores bid and ask price levels, performs crossing checks, executes matching, rests unfilled quantity, cancels live orders, and emits the events produced by those state transitions.

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

Events describe the observable results of applying an action. The matching core returns events instead of printing directly, which separates domain behavior from presentation and makes the engine easier to test, replay, and integrate.

## Order Book Data Structures

The book is split into independent bid and ask trees:

```cpp
std::map<std::int64_t, std::deque<Order>, std::greater<>> bids_;
std::map<std::int64_t, std::deque<Order>> asks_;
```

The bid side uses descending price order, so `bids_.begin()` is the highest bid. The ask side uses ascending price order, so `asks_.begin()` is the lowest ask. This makes best-price access direct and keeps crossing logic simple.

Each price level stores a FIFO queue:

```cpp
std::deque<Order>
```

New resting orders are appended to the back of the deque. Matching consumes from the front. This preserves time priority for all orders resting at the same price level.

The book also maintains a live order lookup:

```cpp
std::unordered_map<std::uint64_t, OrderLocation> orders_by_id_;
```

`OrderLocation` records the side and price for a live order. This lets cancellation jump directly to the relevant price level instead of scanning every price level in the book.

Balanced trees are used because they provide deterministic ordered price levels, efficient insertion/removal, and direct access to the current best price. FIFO queues are used because the exchange priority rule within a price level is arrival order, not order ID or quantity.

Together, these structures implement price-time priority:

1. The tree selects the best available price.
2. The deque selects the oldest order at that price.
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

Passive orders are inserted into the price tree for their side and appended to the back of the deque at their price level. This gives older resting orders at the same price priority over newer orders.

## Cancel Flow

Cancellation starts with an order ID lookup. Inside an `OrderBook`, the `orders_by_id_` index identifies the side and price level where the order should be resting.

The book then:

1. Finds the relevant bid or ask price level.
2. Searches that level's FIFO queue for the target order ID.
3. Erases the order from the queue when found.
4. Removes the price level if the queue becomes empty.
5. Erases the order ID from the live order lookup.
6. Emits a cancel event for a successful cancellation.

Unknown IDs are rejected with a `RejectedEvent`. At the exchange layer, cancellation currently searches across symbol books because there is not yet a global order ID to symbol index.

## Event System

The current event system is implemented as a `std::variant`:

```cpp
using Event = std::variant<TradeEvent, AcceptedEvent, CanceledEvent, RejectedEvent>;
```

Current events include:

| Event | Purpose |
| --- | --- |
| `AcceptedEvent` | Reports that a command was accepted or returns a book snapshot message. |
| `RejectedEvent` | Reports invalid operations such as duplicate order IDs or unknown cancel IDs. |
| `TradeEvent` | Reports an execution between an incoming order and a resting order. |
| `CanceledEvent` | Reports successful removal of a resting order. |

Event-driven design is useful because it keeps mutation and observation separate. The matching engine can update internal state and return a precise event stream without depending on terminal output, logging, networking, or persistence code.

## Complexity Analysis

| Operation            | Complexity                   | Notes                           |
| -------------------- | ---------------------------- | ------------------------------- |
| Insert resting order | O(log P)                     | Tree insertion                  |
| Best bid/ask lookup  | O(1)                         | Front tree access via `begin()` |
| Match execution      | O(K)                         | K = fills generated             |
| Cancel by ID         | O(1) average + queue removal | Depends on queue erase strategy |

Definitions:

- `P` = number of price levels on a side of the book.
- `K` = number of matched resting orders that generate fills.

The current cancel path uses an average O(1) hash lookup to find the price level, then searches and erases within the deque at that level. A future iterator-based design can remove the queue scan.

At the exchange layer, cancel routing currently scans symbol books until the order is found. A global order ID index would remove that cross-book search.

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

- Intrusive linked-list storage for resting orders.
- Iterator-based O(1) cancellation within price levels.
- An exchange-level order ID index for direct symbol routing.
- Lock-free or concurrent designs for higher-throughput deployments.
- Network ingress and session management.
- Binary protocols for lower parsing overhead.
- Persistence and replay logging.
- Release-mode latency benchmarking on Linux.
- Multi-symbol scaling with explicit ownership and routing.

## Design Philosophy

This engine prioritizes correctness before optimization. The implementation makes matching rules explicit, preserves deterministic behavior, and uses systems-level data structures that map directly to exchange concepts.

The architecture favors a clear exchange-style design over framework abstraction: parsed actions enter the exchange, order books mutate deterministic state, and events describe the result. That separation keeps the core matching logic small, inspectable, and ready for targeted optimization when the correctness baseline is stable.
