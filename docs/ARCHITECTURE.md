# Matching Engine Architecture

## Overview

This repository implements a low-latency C++ matching engine for exchange-style limit order matching. The engine follows deterministic price-time priority: better prices match first, and orders at the same price match in FIFO arrival order.

Orders are processed synchronously through an event-driven command flow. A parsed action is applied to the exchange, routed to an order book, and converted into domain events that describe the observable result of the operation.

The current implementation is intentionally focused on correctness, deterministic behavior, and clean systems design with targeted hot-path optimizations where they make the matching rules clearer. It uses ordered standard-library maps for price levels, intrusive FIFO queues for per-price order priority, and pooled order storage with a free-list reuse path so cancellation and matching remain easy to inspect while avoiding avoidable node allocation.

## Complexity Analysis

| Operation | Complexity | Notes |
| --- | --- | --- |
| Exchange submit route | O(1) average + book work | Duplicate check and symbol lookup use flat hash maps. |
| Exchange cancel/modify route | O(1) average + book work | `order_to_book_` avoids scanning symbol books. |
| Best bid/ask lookup | O(1) | `begin()` on the ordered price tree. |
| Insert resting order | O(log P) + O(1) average | Price-level tree insertion/find, pool create, FIFO tail append, order-id index insert. |
| Match execution | O(K + E) amortized | `K` fills; `E` exhausted price levels erased by iterator. |
| `FOK` preflight | O(L) | Walks crossing price levels and uses aggregate `total_volume`, not per-order scans. |
| Cancel by ID | O(1) average + O(log P) + O(1) | Hash lookup, price-level lookup, intrusive unlink, pool release. |
| Same-price size-reduction modify | O(1) average + O(log P) | Route lookup, order lookup, price-level aggregate update. |
| Cancel-replace modify | O(1) average + cancel + match/rest | Removes old priority, then executes the replacement through normal matching. |
| Print snapshot | O(N) | Walks every resting order for presentation text. |

Definitions:

- `P` = number of price levels on a side of the book.
- `K` = number of matched resting orders that generate fills.
- `Q` = number of resting orders at one price level.
- `L` = number of crossing price levels inspected by an `FOK` preflight.
- `E` = number of price levels exhausted during matching.
- `N` = number of resting orders in a snapshot.

The old deque-based cancel path had an additional O(Q) queue scan. The current intrusive index stores raw `Order*` values, so cancellation no longer depends on same-price queue depth inside the `OrderBook`. The exchange-level `order_to_book_` index also removes the previous cross-symbol scan before entering the book. Pool allocation is amortized by fixed-size blocks, cancel/match release paths return slots to the free list instead of calling the general-purpose allocator, and cancel returns a single result instead of using an event vector.

Big O does not fully describe the hot path. The current design pays
`std::map`'s pointer-chasing cost to keep price ordering simple and
deterministic, then offsets some of that cost with cache-friendlier choices in
the order-id indexes and order storage. Flat hash maps keep lookup metadata
contiguous, intrusive queues avoid separate list-node allocations, and pooled
order blocks make live orders less scattered than per-order heap allocation.
Reserve sizing is therefore a locality decision as much as a growth decision:
too little reserve can trigger rehashing or block growth, while too much reserve
can inflate the working set and hurt cache/TLB behavior.

## Core Components

### Exchange

`Exchange` is the command-processing boundary. It receives typed `Action` values, owns order books by symbol, routes symbol-scoped actions to the correct book, and writes emitted `Event` values into a caller-owned buffer.

The exchange also maintains a live order index:

```cpp
ankerl::unordered_dense::map<OrderId, OrderBook*> order_to_book_;
```

The index maps each live order ID to the single-symbol `OrderBook` that owns the resting order, so cancel routing can go directly to the correct book instead of scanning all symbols. Symbol books are heap-owned by `std::unique_ptr<OrderBook>`, which keeps the stored `OrderBook*` values stable even if the symbol map rehashes.

### OrderBook

`OrderBook` owns the resting liquidity for a single symbol. It stores bid and
ask price levels, performs crossing checks, executes matching, rests unfilled
quantity, cancels live orders, and writes the events produced by those state
transitions. Submit, market, and modify paths can produce event streams because
one incoming order can accept and then trade against several resting orders.
Cancellation produces exactly one `CancelResult`.

### Parser

`Parser` converts textual commands into typed actions. It keeps input handling separate from exchange logic, which allows matching behavior to be tested independently from command parsing.

Supported commands currently include:

```text
SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity> [GTC|IOC|FOK]
MARKET <id> <symbol> <BUY|SELL> <quantity>
CANCEL <id>
MODIFY <id> <new_price> <new_quantity>
PRINT
```

`SUBMIT` defaults to `GTC` when no time-in-force token is provided. `MARKET`
orders omit price because they use available opposite-side liquidity
immediately and never rest. `MODIFY` is routed by order ID because the exchange
live-order index already knows which symbol book owns the resting order.

### Actions

Actions represent validated command intent before it mutates exchange state. The current action set is implemented as a `std::variant`:

```cpp
using Action =
    std::variant<SubmitOrderAction,
                 MarketOrderAction,
                 CancelOrderAction,
                 ModifyOrderAction,
                 PrintBookAction>;
```

This keeps command dispatch explicit and makes replay-style processing straightforward.

### Command Routing Paths

Every parsed action enters `Exchange::process`, which clears the caller-owned
event buffer and uses `std::visit` to dispatch the typed command.

| Command | Routing path | State mutation |
| --- | --- | --- |
| `SUBMIT` | Exchange duplicate check -> symbol book lookup/create -> `OrderBook::submit` | May trade, rest `GTC` remainder, update book and exchange live indexes. |
| `MARKET` | Exchange duplicate check -> symbol book lookup/create -> `OrderBook::submit_market` | May trade immediately, never rests incoming remainder. |
| `CANCEL` | Exchange order-id route lookup -> owning `OrderBook::cancel` | Removes one resting order from queue, book index, pool, and exchange index. |
| `MODIFY` | Exchange order-id route lookup -> owning `OrderBook::modify` | Either updates quantity in place or cancel-replaces through normal matching. |
| `PRINT` | Exchange iterates known symbol books -> `OrderBook::snapshot` | Does not mutate matching state. |

After submit, market, and modify operations, the exchange scans emitted
`TradeEvent` values and removes fully filled resting orders from
`order_to_book_`. That keeps the exchange-level route index aligned with each
book's internal live-order index.

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

Modification can emit one or more events because cancel-replace behavior may
turn the replacement into an aggressive order that immediately trades:

```cpp
void modify(OrderId order_id, Price new_price, Quantity new_quantity, std::vector<Event>& out);
```

The design decision is to keep `Action` as a closed set of typed commands rather
than route string tokens through the exchange. That makes unsupported commands a
parser concern, keeps exchange dispatch exhaustive at compile time, and gives
tests a direct way to exercise matching behavior without text parsing.

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

### Design Decisions and Tradeoffs

The book intentionally uses `std::map` for price levels instead of a flat sorted
vector, heap, or fixed tick array. A tree costs pointer chasing and has weaker
cache locality than contiguous storage, but it gives deterministic ordering,
stable logarithmic insert/erase, and direct best-price access without assuming a
bounded price domain. That is a good fit for an inspectable matching engine
where correctness and replayability are more important than squeezing every
cycle from a specialized price ladder.

The same design uses intrusive FIFO queues instead of `std::deque` or
`std::list` per price level. The tradeoff is that `Order` carries `prev` and
`next` fields and queue ownership must be disciplined, but the benefit is direct
unlink on cancel, no extra list node allocation, and a queue shape that mirrors
price-time priority: append at the tail, match from the head.

Order objects live in `OrderPool` blocks instead of individually allocated
nodes. This custom pool allocator is one of the highest-impact performance
choices in the engine: it keeps recently created orders close together in
memory, preserves stable `Order*` values for intrusive queues and hash indexes,
and lets filled or canceled slots be reused from a local free list.

The free list matters because the hottest destructive paths are cancel and full
fill. When either path removes an order, the book unlinks the intrusive node,
erases the id index entry, and returns the slot to `OrderPool::release()` rather
than calling the general-purpose allocator. A later insert can take that slot
directly from `OrderPool::create()`. This turns repeated order churn into local
pointer rewiring and slot reuse instead of repeated heap allocate/free traffic.

The tradeoff is manual lifetime management inside the pool and block-level
capacity tuning. The benefit is large: the EC2 optimized-vs-std-toy comparison
shows the optimized book ahead by `122.13x` on passive insert, `320.71x` on
random cancel, and `1,642.78x` on unknown cancel at 10,000 operations. Those
gains come from the combination of dense id lookup, intrusive FIFO links, and
pooled free-list storage removing scans and allocator pressure from the hot
paths.

The exchange keeps a second live-order index from order ID to owning book. This
duplicates a small amount of metadata already present inside each book, but it
turns cross-symbol cancel and modify routing into an average `O(1)` lookup
instead of a scan over symbol books. Heap-owning the books with
`std::unique_ptr<OrderBook>` keeps those stored book pointers stable when the
symbol map grows.

Live orders are owned by `OrderPool`:

```cpp
OrderPool order_pool_;
```

`OrderPool` allocates contiguous blocks of order slots and reuses canceled or
filled slots through an internal free list:

```text
new resting order -> OrderPool::create()
cancel/full fill  -> OrderPool::release()
next insert       -> reuse free-list slot before growing blocks
```

`OrderBook` owns the pool, while `OrderQueue` and `orders_by_id_` only hold raw
non-owning pointers into that pool. That ownership model keeps pointer lifetime
simple at the book boundary while letting hot queue/index operations work with
plain `Order*` values.

### Reserve Capacity Tuning

Reserve capacity is a preallocation hint for `orders_by_id_` and `OrderPool`, not
the total number of operations a benchmark or replay will process. The important
runtime footprint is closer to peak live resting orders:

```text
live_orders = accepted_resting_orders - filled_resting_orders - canceled_orders
```

A stream with 100,000 commands can have far fewer than 100,000 live orders if it
contains cancels and fills. The mixed submit/cancel benchmark, for example,
peaks around 40,003 live resting orders even though it processes 100,000
operations.

EC2 reserve sweeps showed that reserving the full operation count significantly
hurt mixed-workload throughput. The extra preallocated hash-table and pool
footprint appears to damage cache/TLB locality more than it helps by avoiding
later growth. For that workload, reserve sizing behaved like a cache/locality
tuning parameter, with small reserves around 8,192 to 16,384, and roughly 10% of
the operation count, outperforming both no reserve and large reserves near or
above peak live depth.

Current benchmark reserve rules are workload-specific:

- Submit-only and cancel-only: reserve exactly `order_count`.
- Match-only: reserve `resting_order_count`, or `order_count` when the benchmark
  only exposes that single count.
- Mixed submit/cancel/match: reserve `max(1024, order_count / 10)`.
- End-to-end parse/process/format: reserve `max(1024, command_count / 10)`.

Balanced trees are used because they provide deterministic ordered price levels, efficient insertion/removal, and direct access to the current best price. Intrusive FIFO queues are used because the exchange priority rule within a price level is arrival order, not order ID or quantity, while embedded links keep cancel removal O(1) with lower allocator overhead than node-based standard containers.

Together, these structures implement price-time priority:

1. The tree selects the best available price.
2. The price-level head pointer selects the oldest order at that price.
3. Matching removes or reduces resting orders in that exact priority order.

## Matching Flow

An incoming order follows a synchronous lifecycle:

1. Validate order identity against the live order lookup.
2. Reject duplicate live order IDs before emitting acceptance.
3. For `FOK`, preflight aggregate opposite-side liquidity before mutating state.
4. Emit `AcceptedEvent` when the order is allowed to execute.
5. Match against the opposite side while price and quantity allow execution.
6. Generate `TradeEvent` values for each fill.
7. Reduce incoming and resting quantities for partial fills.
8. Rest any remaining incoming `GTC` quantity on its own side of the book.

### Aggressive Orders

An aggressive order crosses the spread and executes immediately against resting liquidity. A buy order is aggressive when its limit price is greater than or equal to the best ask. A sell order is aggressive when its limit price is less than or equal to the best bid.

Aggressive orders always match against the best opposite price first. Within that price level, they consume the oldest resting order first. Trades execute at the resting order price, which is the standard exchange-style behavior implemented by the current book.

If an aggressive order is larger than the first resting order, the resting order is fully removed and matching continues against the next order at the same price or the next best price level. If the incoming order is fully filled, it never rests on the book.

### Passive Orders

A passive order does not cross the opposite side. A buy order is passive when the best ask is above its limit price or no asks exist. A sell order is passive when the best bid is below its limit price or no bids exist.

Passive orders are inserted into the price tree for their side and appended to the tail of the intrusive queue at their price level. The stored `Order*` is added to `orders_by_id_`, which gives older resting orders at the same price priority over newer orders while preserving direct cancel access.

`OrderPool` creates the resting order slot before queue insertion. The order book then links that pointer into the price-level queue and records the same pointer in `orders_by_id_`.

### Time-in-Force and Market Orders

`GTC` limit orders rest any unfilled remainder. `IOC` limit orders can trade
immediately, but any unfilled remainder expires without entering the book. `FOK`
limit orders first walk crossing price levels and use each level's
`total_volume` aggregate to prove the full quantity can execute; if there is not
enough eligible liquidity, the book emits `RejectedEvent{InsufficientLiquidity}`
and leaves state unchanged.

Market orders reuse the same best-price matching loops with sentinel prices:
market buys behave like buys priced at the maximum `Price`, and market sells
behave like sells priced at the minimum `Price`. This avoids maintaining a
second execution algorithm. The tradeoff is that market-order intent is visible
only at the action/exchange boundary; inside the book it becomes an unbounded
crossing order that never rests. If visible liquidity is insufficient, the book
emits a rejection for the unfilled remainder after any immediate trades.

### Modify Flow

Modification is routed through the exchange-level `order_to_book_` index, then
applied inside the owning `OrderBook`.

The book uses two modify paths:

1. Same price with lower quantity: update the order in place, reduce the price
   level's aggregate `total_volume`, preserve FIFO priority, and emit
   `ModifiedEvent`.
2. Price change or size increase: remove the old resting order, emit
   `ReplacedEvent`, and process the replacement through normal matching/resting
   logic.

The cancel-replace path intentionally loses FIFO priority because the modified
order is economically equivalent to a new order at that price/size. This costs
an extra remove plus insert/match path, but it keeps priority rules explicit and
prevents size increases from jumping ahead of older resting liquidity.

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
    ModifiedEvent,
    ReplacedEvent,
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
| `ModifiedEvent` | Reports a same-price quantity reduction that preserved FIFO priority. |
| `ReplacedEvent` | Reports a modify that used cancel-replace semantics and lost FIFO priority. |
| `BookSnapshotEvent` | Carries snapshot display text for the non-hot-path print command. |

Event-driven design is useful because it keeps mutation and observation separate. The matching engine can update internal state and write a precise event stream without depending on terminal output, logging, networking, or persistence code.

Hot-path events carry structured data instead of preformatted strings. Display
strings such as `"accepted order 42"` and `"unknown order id 42"` are created by
`format_event()` at the presentation boundary. Caller-owned submit buffers avoid
repeated event-vector construction while preserving explicit event
materialization. Cancellation uses `CancelResult` directly because it emits only
one result.

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
- Evaluate alternative price-level containers for bounded tick domains if benchmarks show `std::map` locality is the dominant cost.
- Add explicit market-data snapshot/delta events instead of using presentation-oriented snapshot strings.
- Add richer reject metadata if integrations need to distinguish parse, validation, and matching failures more precisely.
- Lock-free or concurrent designs for higher-throughput deployments.
- Network ingress and session management.
- Binary protocols for lower parsing overhead.
- Persistence and replay logging.
- Multi-symbol sharding with explicit ownership and routing.

## Design Philosophy

This engine prioritizes correctness before optimization. The implementation makes matching rules explicit, preserves deterministic behavior, and uses systems-level data structures that map directly to exchange concepts.

The architecture favors a clear exchange-style design over framework abstraction: parsed actions enter the exchange, order books mutate deterministic state, and events describe the result. That separation keeps the core matching logic small, inspectable, and ready for targeted optimization when the correctness baseline is stable.
