# Hot Path Analysis

## Current EC2 c7i-flex.large Snapshot

The current documented hot-path snapshot uses the full-suite EC2 refresh from
`2026-05-31T21:41:35Z`. Flamegraph and `perf stat` notes remain from the May 23
profiling pass because this refresh did not rerun `perf`.

### Environment

- Host: AWS EC2 `c7i-flex.large`
- OS/kernel: Ubuntu 26.04 LTS, Linux `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8488C
- Compiler: GCC/G++ 15.2.0
- Build: Release, `-O3 -DNDEBUG -march=native`
- Source: local tree at `75d9181e` plus uncommitted single-order latency
  benchmark and documentation updates; synced to EC2 without `.git` metadata or
  macOS sidecar files
- Validation: `130/130` CTest cases passed before benchmark execution

Historical benchmark development before the v1 refresh was performed on EC2
`t3.small`. Current benchmark rows use EC2 `c7i-flex.large` for more stable
sustained CPU performance; profiling artifacts remain from the earlier c7i
profiling pass.

### Artifacts

| Artifact | Purpose |
| --- | --- |
| [`hotpath-throughput.svg`](hotpath-throughput.svg) | Throughput comparison across core, realistic, boundary, and stress paths |
| [`hotpath-latency.svg`](hotpath-latency.svg) | Core hot-path p50/p99/max batch latency at 256-op batches |
| [`hotpath-critical-path.svg`](hotpath-critical-path.svg) | Source-level submit/match and cancel path diagram |
| [`perf-random-cancel.svg`](perf-random-cancel.svg) | `cpu-clock` flamegraph for random cancel |
| [`perf-true-mixed.svg`](perf-true-mixed.svg) | `cpu-clock` flamegraph for direct true mixed flow |
| [`perf-end-to-end-true-mixed.svg`](perf-end-to-end-true-mixed.svg) | `cpu-clock` flamegraph for parser/exchange/formatter flow |
| [`perf-deep-sparse.svg`](perf-deep-sparse.svg) | `cpu-clock` flamegraph for deep sparse GTC stress |

Raw reports and perf data live under `benchmarks/results/`:

- `core_hot_path_results.{txt,json}`
- `realistic_flow_results.{txt,json}`
- `stress_benchmark_results.{txt,json}`
- `batch_latency_results.{txt,json}`
- `single_order_latency_results.{txt,json}`
- `perf-*-report.txt`
- `perf-*.data`
- `perf-*.folded`

### Throughput Snapshot

All rows are Google Benchmark median rows from the pinned
`2026-05-31T21:41:35Z` EC2 full-suite refresh.

| Workload | CPU Time | Throughput |
| --- | ---: | ---: |
| Passive insert, 100k ops | `3.04 ms` | `32.85M ops/sec` |
| One-level crossing match, 100k ops | `2.79 ms` | `35.86M ops/sec` |
| Random cancel, 100k ops | `3.83 ms` | `26.10M ops/sec` |
| Unknown cancel, 100k ops | `0.32 ms` | `314.08M ops/sec` |
| Modify if present, 100k ops | `1.67 ms` | `59.96M ops/sec` |
| OrderBook true mixed, 100k ops | `4.45 ms` | `22.48M ops/sec` |
| End-to-end true mixed, 100k commands | `47.62 ms` | `2.10M commands/sec` |
| Best-level churn, 1M ops | `35.81 ms` | `27.92M ops/sec` |
| Level create/delete churn, 1M ops | `23.62 ms` | `42.34M ops/sec` |
| Shallow GTC mixed, 100k primary ops | `6.94 ms` | `26.67M ops/sec` |
| Deep sparse GTC mixed, 100k primary ops | `30.69 ms` | `3.26M ops/sec` |

### Latency Snapshot

Core latency rows are median values across five trials at a 256-operation batch
size from the `2026-05-31T21:41:35Z` full-suite refresh. These are amortized
batch latencies, not true single-order tail latency.

| Workload | p50 | p99 | Max |
| --- | ---: | ---: | ---: |
| Passive insert | `44.00 ns/op` | `81.41 ns/op` | `112.11 ns/op` |
| One-level crossing match | `36.57 ns/op` | `79.43 ns/op` | `98.46 ns/op` |
| Random cancel | `86.86 ns/op` | `143.89 ns/op` | `218.98 ns/op` |
| Unknown cancel | `8.12 ns/op` | `10.67 ns/op` | `52.82 ns/op` |
| Modify if present | `22.25 ns/op` | `33.11 ns/op` | `93.05 ns/op` |
| OrderBook true mixed | `52.11 ns/op` | `93.70 ns/op` | `114.35 ns/op` |

### Single-Order Latency Snapshot

Single-order rows are median percentile values across five 1,000,000-sample
trials. Each sample measures one precomputed public `Exchange::process(action)`
call; setup and action generation are outside the measured window. The timer
p50 was 23 ns on the median trial.

| Workload | p50 | p99 | p999 |
| --- | ---: | ---: | ---: |
| Passive insert | `313 ns` | `557 ns` | `1,890 ns` |
| Aggressive match | `459 ns` | `739 ns` | `990 ns` |
| Known cancel | `327 ns` | `603 ns` | `794 ns` |
| Modify if present | `320 ns` | `540 ns` | `718 ns` |
| Market order | `476 ns` | `779 ns` | `1,077 ns` |
| Unknown cancel reject | `34 ns` | `39 ns` | `96 ns` |

### Perf Findings

Hardware PMU counters were unavailable on this EC2/kernel combination during
the May 23 profiling pass. That run therefore used software `cpu-clock`
sampling with frame pointers. Kernel symbols were restricted, but user-space
matching-engine symbols resolved correctly. Separate `perf stat` attempts for
core hot path, realistic flow, and stress failed with `No supported events
found. The cycles event is not supported.`

| Profile | Main signal |
| --- | --- |
| Random cancel | `remove_resting_order` and `cancel` dominate the measured path; preload still appears because `perf` samples setup excluded by Google Benchmark timing. |
| Direct true mixed | Engine symbols are led by `remove_resting_order`, `modify`, `add_resting_order`, `prepare_incoming_order`, `submit`, and `cancel`; workload generation also appears and should not be read as matching-core cost. |
| End-to-end true mixed | Parser and formatting costs dominate: stream extraction, `Parser::parse_line`, string output, and locale/iostream setup are wider than `OrderBook` symbols. |
| Deep sparse GTC mixed | `remove_resting_order` is the widest engine symbol, with `std::map` lower-bound/find work visible under deep sparse price-level lookups. |

### Current Classification

| Path | Classification | Reason |
| --- | --- | --- |
| End-to-end parser/format boundary | Hot | Direct `OrderBook` true mixed is roughly 10.7x faster than end-to-end true mixed in throughput. |
| Random cancel | Hot | Random cancel has the highest p50/p99 among core batch-latency rows and remains much slower than hash-miss cancel. |
| Deep sparse price-level access | Hot under adversarial books | Sparse 50k-level workload falls to ~3.26M ops/sec and shows tree lookup work in perf. |
| Intrusive queue unlink itself | Warm | It is part of hot cancel/remove frames, but the queue operation is no longer isolated as the dominant cost. |
| Unknown cancel | Cold | The rejection path is extremely fast at ~314M ops/sec, ~11 ns p99 batch latency, and 39 ns p99 single-order latency. |

### Caveats

- `perf record` samples benchmark setup and workload generation, even when
  Google Benchmark excludes that work from timing with `PauseTiming()`.
- Direct true-mixed and end-to-end true-mixed flamegraphs therefore include
  workload-construction frames; use the throughput and latency artifacts for the
  measured benchmark numbers.
- Hardware counters such as cycles, instructions, cache misses, and LLC misses
  were unsupported on this host. See `benchmarks/results/perf_results.csv` and
  `benchmarks/results/*_perf_stat*.txt`.
- `perf stat` runs are diagnostic only and introduce measurement overhead; the
  official throughput and latency numbers above come from normal native
  benchmark execution.

## Earlier Cancel-Only Pass

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
