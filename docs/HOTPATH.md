# Hot Path Analysis

This note records the first perf-based hot path pass for the intrusive
`OrderQueue` / `OrderPool` cancel implementation.

## Environment

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- Compiler: GCC/G++ 15.2.0
- Build: Release with debug symbols and frame pointers
- Workload: `BM_OrderBook_CancelRandom_Throughput/100000`

Perf build:

```bash
cmake -B build-perf-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -fno-omit-frame-pointer -g"

cmake --build build-perf-debug
```

## Commands

`perf_event_paranoid` was lowered for the session:

```bash
sudo sysctl kernel.perf_event_paranoid=1
```

Hardware PMU counters were still unavailable on this EC2/kernel combination:

```text
<not supported> cpu-cycles
```

The useful profile was collected with the software `cpu-clock` event:

```bash
sudo perf record -e cpu-clock -g \
  -o perf-cancel-random-debug.data \
  ./build-perf-debug/core_hot_path_benchmark \
  --benchmark_filter='^BM_OrderBook_CancelRandom_Throughput/100000$' \
  --benchmark_min_time=3s

sudo perf report \
  -i perf-cancel-random-debug.data \
  --stdio \
  --no-children \
  --sort=symbol

sudo perf annotate \
  -i perf-cancel-random-debug.data \
  --stdio \
  --symbol='matching_engine::OrderBook::cancel(unsigned long)'
```

FlameGraph:

```bash
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph

sudo perf script -i perf-cancel-random-debug.data > perf-cancel-random.script
~/FlameGraph/stackcollapse-perf.pl perf-cancel-random.script > perf-cancel-random.folded
~/FlameGraph/flamegraph.pl \
  --title "BM_OrderBook_CancelRandom_Throughput/100000 cpu-clock" \
  --countname samples \
  perf-cancel-random.folded > perf-cancel-random.svg
```

Local artifact:

- `../perf-cancel-random.svg`

## Benchmark Comparison

The first pass compared cancel variants at 100,000 resting orders. Front and
back cancels are now diagnostic-only benchmark registrations; random and
unknown cancels remain in the default suite.

| Workload | CPU Time | Throughput | Read |
| --- | ---: | ---: | --- |
| `BM_CancelFront/100000` | ~6.88-6.91 ms | ~14.47M-14.53M/s | Fast sequential successful cancel |
| `BM_CancelBack/100000` | ~6.88-6.92 ms | ~14.45M-14.53M/s | Fast reverse-order successful cancel |
| `BM_OrderBook_CancelUnknown_Throughput/100000` | ~7.96-7.97 ms | ~12.54M-12.57M/s | Hash miss and rejection event path |
| `BM_OrderBook_CancelRandom_Throughput/100000` | ~33.5-37.4 ms in later perf runs | ~2.67M-2.98M/s | Successful cancel with randomized order-id access |

Random cancel is roughly 5-6x slower than front/back cancel at the same book
depth. That gap remains after removing the old queue scan and replacing
`std::list` nodes with intrusive queue links.

## FlameGraph Reading

In the FlameGraph:

- Box width is sampled time.
- Height is call depth.
- X-position is layout, not timeline order.
- Colors are visual grouping, not severity.

The lower frames are Google Benchmark and process startup:

```text
core_hot_path_benchmark
_start
main
RunSpecifiedBenchmarks
RunBenchmarks
DoOneRepetition
BM_OrderBook_CancelRandom_Throughput
run_cancel_workload
```

Ignore those for engine hot-path decisions.

The useful regions are:

1. `submit` / `_M_insert_unique_node` / `malloc`
   - This is benchmark preload/setup.
   - Google Benchmark pauses timing around preload, but `perf record` still
     samples it.
   - This region is useful context, but it is not the measured cancel loop.

2. `matching_engine::OrderBook::cancel`
   - This is the measured cancel path and dominates random cancel.
   - The widest frames under it are unordered-map lookup and erase work.

3. `std::vector<std::variant<...>>`
   - Older runs returned a one-event `std::vector<Event>` for each cancel.
   - The current cancel path returns one direct `CancelResult` instead.

## Annotated Cancel Findings

`perf report` for random cancel:

```text
50.52%  matching_engine::OrderBook::cancel(unsigned long)
```

`perf annotate` inside `OrderBook::cancel` showed the heaviest local samples in:

| Area | Approx local sample share | Meaning |
| --- | ---: | --- |
| unordered-map bucket/node load | ~17.5% | `orders_by_id_.find(order_id)` bucket traversal |
| unordered-map key load | ~21.9% | Load key from hash node |
| unordered-map key compare | ~12.6% | Compare found key with cancel id |
| random `Order*` dereference | ~31.5% | Load order metadata, especially side/price, from a randomized order pointer |

The random `Order*` dereference is the clearest locality signal. The hash lookup
finds a pointer, then cancellation jumps to a randomized order slot to read the
side and price needed for price-level lookup and unlink.

## Current Conclusion

The intrusive FIFO queue is doing its job.

`OrderQueue::remove` does not appear as a meaningful hot symbol. Front and back
cancels exercise successful cancel, intrusive unlink, and pool release, yet stay
around 14.5M cancels/s at 100,000 orders.

The remaining random-cancel cost is dominated by:

1. `orders_by_id_` unordered-map lookup.
2. Random pointer chasing from the hash table to `Order`.
3. Hash erase on successful cancel.
4. Materializing the single cancel result.

Not currently dominant:

1. `OrderQueue::remove`.
2. `OrderPool::release`.
3. The price-level `std::map` lookup.

## Optimization Targets

Likely next targets, in priority order:

1. Store richer cancel metadata in the book-local order-id index.
   - Today `orders_by_id_` maps `order_id -> Order*`.
   - Exchange-level routing now uses `order_to_book_` to avoid scanning symbol books before book-local cancel.
   - Cancel then dereferences a random `Order*` to read `side` and `price`.
   - Storing `{Order*, Side, Price}` or `{Order*, OrderQueue*}` in the hash entry
     may avoid one expensive random order dereference before queue lookup.

2. Reserve and tune the order-id index.
   - Pre-reserving `orders_by_id_` capacity for expected book depth may reduce
     hash-table growth and bucket locality costs.
   - A lower max load factor may reduce bucket-chain work at the cost of memory.

3. Reduce event-vector overhead.
   - Cancel returns a single direct result and no longer needs an event vector.
   - Submit uses a caller-provided event buffer because it can produce multiple
     events.

4. Consider a flatter order-id table.
   - `std::unordered_map` is node-based.
   - A flat hash table or custom open-addressed table may improve locality for
     random cancel workloads.

## Profiling Caveats

- Hardware counters such as cycles, instructions, cache misses, and TLB misses
  were unavailable on this EC2/kernel setup.
- `perf record` samples the whole process, including Google Benchmark setup code
  that is excluded from reported benchmark timing by `PauseTiming()`.
- Random cancel is slow enough that `OrderBook::cancel` dominates the profile
  anyway; front/back profiles contain more setup/preload noise.
- Use the FlameGraph for broad attribution, and `perf annotate` for the actual
  instruction-level read inside `OrderBook::cancel`.
