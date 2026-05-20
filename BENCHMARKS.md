# Benchmarks

This document records baseline matching-engine throughput measurements from the
Linux EC2 benchmark host. Benchmarks are run manually and should not be executed
automatically by CMake or CI until that workflow is added intentionally.

## Environment

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `80e9dc4` plus local end-to-end mixed order-flow benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 137/137 passed before benchmark execution
- Run date: `2026-05-19T21:04:32Z`
- Command: pinned `end_to_end_benchmark --benchmark_repetitions=5`

## Workloads

- `insert_benchmark` measures passive BUY/SELL limit orders that do not cross,
  isolating resting order insertion throughput.
- `match_benchmark` preloads resting ask liquidity, then submits aggressive buy
  limit orders that cross and consume the book.
- `cancel_benchmark` preloads same-price FIFO liquidity, then measures front,
  back, random, and unknown cancels. It also includes a mixed submit/cancel/match
  stream with roughly 70% passive inserts, 20% cancels, and 10% crossing orders.
- `true_mixed_benchmark` measures direct `OrderBook` hot-path traffic with
  randomly interleaved GTC, cancel, modify, IOC, market, and FOK operations. It
  bypasses Parser, Exchange, filesystem I/O, and string/event formatting.
- `shallow_gtc_mixed_benchmark` measures direct `OrderBook` hot-path GTC churn
  with a low live resting depth, tight prices, random cancels/modifies, and
  crossing GTC orders that immediately match.
- `deep_sparse_gtc_mixed_benchmark` measures direct `OrderBook` hot-path GTC
  churn with many occupied price levels and only one to two orders per level.
  This "deep sparse" shape stresses price-level map/tree traversal and level
  cleanup more than long FIFO queues at one price.
- `latency_benchmark` runs a separate amortized batch latency suite over the
  same hot paths. It is not a Google Benchmark replacement and does not rename
  or replace the throughput benchmark binaries.
- `end_to_end_benchmark` measures public-boundary CLI-style overhead:
  in-memory input lines, parser, exchange routing, order-book mutation, and
  event formatting.
- `order_book_stress_tests` runs long deterministic correctness stress streams
  as GoogleTest/CTest cases. These are soak-style structural validation tests,
  not throughput benchmarks.

The hot-path throughput and latency workloads bypass parser, stdin, file I/O,
and logging. Setup/preload work is excluded from the measured loop where
appropriate.

## Stress/Soak Correctness Validation

The stress/soak suite validates long-run `OrderBook` health under million-op
streams. It is intentionally separate from Google Benchmark timing: the goal is
to catch structural degradation, stale indexes, missed level cleanup, and
aggregate-volume drift after sustained churn.

Each scenario runs 1,000,000 operations and checks book structure every 10,000
operations plus final state. Health checks verify:

- best bid and best ask are not crossed
- live order-id index entries match queued resting orders
- price-level total volume equals summed order quantities
- empty or zero-volume price levels are erased
- resting orders retain correct side, price, and positive quantity

Latest EC2 stress/soak validation metadata:

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `6e66ca0` plus local stress live-set fixes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Run date: `2026-05-19T21:45:45Z`
- Command: pinned `ctest -R OrderBookStressTest --output-on-failure`

Latest EC2 stress/soak correctness results:

| Test | Operations | Result | CTest Wall Time |
| --- | ---: | --- | ---: |
| `LongRunMixedOneMillionMaintainsBookHealth` | 1,000,000 | Passed | 0.54 s |
| `LongRunLevelChurnOneMillionMaintainsBookHealth` | 1,000,000 | Passed | 0.39 s |
| `LongRunModifyCancelChurnOneMillionMaintainsBookHealth` | 1,000,000 | Passed | 0.54 s |

Artifact files:

- `benchmarks/stress_results.txt`
- `benchmarks/stress_environment.txt`

## OrderBook True Mixed Hot Path

`BM_OrderBookTrueMixed` is an `OrderBook`-only Google Benchmark throughput case.
It pre-generates the full operation stream before timing, uses fixed RNG seeds,
reuses caller-owned `std::vector<Event>` buffers, and reports items/s through
Google Benchmark's item counter. Book construction, reserve setup, and preload
liquidity are outside the timed loop.

This workload is separate from the end-to-end benchmarks: it does not call
Parser, Exchange, filesystem I/O, or event formatting.

The timed operation stream uses this exact target mix:

| Operation type | Share |
| --- | ---: |
| GTC limit submit | 25% |
| Cancel | 25% |
| Modify | 20% |
| IOC limit submit | 15% |
| Market order | 10% |
| FOK limit submit | 5% |

Operations are randomly interleaved according to the mix rather than generated
in phases. The generator maintains a live set of resting GTC order ids while it
builds the stream. Cancels and modifies target only currently live resting GTC
liquidity. IOC, FOK, and market orders are transient taker flow: they may trade,
partially fill and expire/reject, or reject for insufficient liquidity, but they
are never inserted into the cancel/modify live set.

The workload keeps prices near a deterministic mid-price and uses the current
mixed-workload reserve heuristic:

`reserve_order_capacity = max(1024, operation_count / 10)`

The standalone latency runner also includes `OrderBookTrueMixed` amortized batch
latency for batch sizes 64, 256, and 1,024. These rows are labeled as amortized
batch latency, not true single-operation latency.

Latest EC2 True Mixed run metadata:

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `ffe3f1f` plus local True Mixed benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 121/121 passed before benchmark execution
- Run date: `2026-05-19T20:45:50Z`
- Command: pinned `true_mixed_benchmark` with 5 repetitions, followed by pinned
  `latency_benchmark --samples=1024 --warmup=128 --trials=5`

Latest EC2 True Mixed throughput results:

| Benchmark | Operations | CPU Time | Throughput | Reserve Capacity | Preload Orders | Max Live Orders |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_OrderBookTrueMixed/1000` | 1,000 | 46810 ns | 21.36M items/s | 1,024 | 256 | 256 |
| `BM_OrderBookTrueMixed/10000` | 10,000 | 566029 ns | 17.67M items/s | 1,024 | 256 | 256 |
| `BM_OrderBookTrueMixed/100000` | 100,000 | 6308711 ns | 15.85M items/s | 10,000 | 2,500 | 2,500 |

Artifact files:

- `benchmarks/true_mixed_results.txt`
- `benchmarks/true_mixed_results.json`

## OrderBook Shallow GTC Mixed Hot Path

`BM_ShallowGtcMixed` is an `OrderBook`-only Google Benchmark throughput case for
a shallow book: low live resting depth, high churn, and a cache-hot working set.
It targets about 512 resting GTC orders, preloads the book before timing, keeps
prices near a tight deterministic spread, and rebalances depth after cancels,
matches, and passive submits.

The primary operation stream uses fixed-seed random interleaving with this mix:

| Operation type | Share |
| --- | ---: |
| GTC limit submit | 50% |
| Cancel existing live order | 30% |
| Modify existing live order | 15% |
| Crossing GTC submit | 5% |

The benchmark bypasses Parser, Exchange, filesystem I/O, and event formatting.
It reuses caller-owned `std::vector<Event>` buffers and uses the same mixed
reserve heuristic:

`reserve_order_capacity = max(1024, operation_count / 10)`

Latest EC2 Shallow GTC Mixed run metadata:

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `d491452` plus local Shallow GTC Mixed benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 124/124 passed before benchmark execution
- Run date: `2026-05-20T04:01:43Z`
- Command: pinned `shallow_gtc_mixed_benchmark` with 5 repetitions

Latest EC2 Shallow GTC Mixed throughput results:

| Benchmark | Primary Operations | Timed Book Actions | CPU Time | Throughput | Reserve Capacity | Target Live Orders | Max Live Orders |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_ShallowGtcMixed/1000` | 1,000 | 1,850 | 90221 ns | 20.505M items/s | 1,024 | 512 | 513 |
| `BM_ShallowGtcMixed/10000` | 10,000 | 18,500 | 947193 ns | 19.531M items/s | 1,024 | 512 | 513 |
| `BM_ShallowGtcMixed/100000` | 100,000 | 185,000 | 9840556 ns | 18.800M items/s | 10,000 | 512 | 513 |

Artifact files:

- `benchmarks/shallow_gtc_mixed_results.txt`
- `benchmarks/shallow_gtc_mixed_results.json`

## OrderBook Deep Sparse GTC Mixed Hot Path

`BM_DeepSparseGtcMixed` is an `OrderBook`-only Google Benchmark throughput case
for deep sparse liquidity: many occupied price levels with only one to two
resting GTC orders per level. The goal is to stress the price-level
`std::map`/tree behavior, including traversal and level erasure, rather than
long same-price FIFO queues.

The workload preloads 50,000 occupied price levels before timing. It then
replays a fixed-seed deterministic operation stream with widely spaced passive
prices, a live order index for cancel/modify selection, caller-owned reusable
`std::vector<Event>` buffers, and `reserve_order_capacity` sized from the peak
modeled live order count.

The primary operation stream uses this exact target mix:

| Operation type | Share |
| --- | ---: |
| GTC limit submit at a new sparse level | 45% |
| Cancel existing live order | 30% |
| Modify existing order to a different sparse level | 15% |
| Crossing GTC submit walking sparse levels | 10% |

The benchmark bypasses Parser, Exchange, filesystem I/O, and event formatting.
Crossing GTC orders are generated to consume several best opposite price levels
without resting, so benchmark timing reflects direct `OrderBook` matching and
map updates.

Latest EC2 Deep Sparse GTC Mixed run metadata:

- Host: AWS EC2 benchmark host; IMDS instance-type query returned `N/A`
- OS: Ubuntu 26.04 LTS
- Kernel: `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `c1bea29` plus local Deep Sparse GTC Mixed benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: not rerun in this EC2 pass; only the new benchmark was run
- Run date: `2026-05-20T04:17:13Z`
- Command: pinned `deep_sparse_gtc_mixed_benchmark` with 5 repetitions and
  `--benchmark_filter="^BM_DeepSparseGtcMixed"`
- Transfer hygiene: source was synced with `.git`, build directories,
  `.DS_Store`, and macOS `._*` sidecar files excluded

Latest EC2 Deep Sparse GTC Mixed throughput results:

| Benchmark | Primary Operations | Preload Price Levels | Crossing Levels / Order | CPU Time | Throughput | Reserve Capacity | Max Live Orders | Max Price Levels |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_DeepSparseGtcMixed/1000` | 1,000 | 50,000 | 4 | 662811 ns | 1.509M items/s | 51,026 | 50,002 | 50,002 |
| `BM_DeepSparseGtcMixed/10000` | 10,000 | 50,000 | 4 | 5364079 ns | 1.864M items/s | 51,026 | 50,002 | 50,002 |
| `BM_DeepSparseGtcMixed/100000` | 100,000 | 50,000 | 4 | 54052361 ns | 1.850M items/s | 51,026 | 50,002 | 50,002 |

Artifact files:

- `benchmarks/deep_sparse_gtc_mixed_results.txt`
- `benchmarks/deep_sparse_gtc_mixed_results.json`
- `benchmarks/deep_sparse_gtc_mixed_environment.txt`

## End-to-end benchmarks

The end-to-end benchmarks measure full public-boundary system overhead:

`input lines -> Parser -> Exchange -> OrderBook -> Event formatting`

They pre-generate deterministic command scripts in memory before timing, avoid
filesystem I/O inside the timed loop, and include event formatting in the timed
work. These are parser/exchange/formatter overhead measurements and should not
be compared directly to OrderBook hot-path microbenchmarks.

`BM_EndToEnd_ParseProcessFormat` uses deterministic non-crossing multi-symbol
limit-order input to focus on parse, route, accept, and format cost.
`BM_EndToEnd_MixedOrderFlow_Throughput` uses the same deterministic mixed-flow
methodology as the OrderBook-only true mixed benchmark, but translates each
operation into public command strings so the measured work is:

`command script -> Parser -> Exchange -> OrderBook -> Event formatting`

This benchmark is intentionally separate from `BM_OrderBookTrueMixed`. It
measures Parser, Exchange, OrderBook, and event-formatting overhead together and
should not be compared directly against OrderBook-only mixed hot-path results.

`BM_EndToEnd_MixedOrderFlow_Throughput` pre-generates deterministic interleaved
command streams before timing, uses fixed RNG seeds, avoids filesystem I/O in
the timed loop, and includes event formatting in the measured work. Preload
liquidity is applied through the public parser/exchange path before timing so
the active book state matches the hot-path mixed workload methodology.

The timed mixed order-flow command stream uses this exact operation mix:

| Operation type | Share |
| --- | ---: |
| GTC limit submit | 25% |
| Cancel | 25% |
| Modify | 20% |
| IOC limit submit | 15% |
| Market order | 10% |
| FOK limit submit | 5% |

Operations are randomly interleaved according to the mix rather than generated
in phases. The generator maintains a live set of resting GTC order ids while it
builds the stream. Cancels and modifies target only currently live resting GTC
liquidity. IOC, FOK, and market commands are transient taker flow: they may
trade, partially fill and expire/reject, or reject for insufficient liquidity,
but they are never inserted into the cancel/modify live set.

The end-to-end mixed flow uses the current mixed-workload reserve heuristic
unless a benchmark configuration overrides it:

`reserve_order_capacity = max(1024, operation_count / 10)`

`BM_EndToEnd_MixedOrderFlow_Latency` reports amortized end-to-end batch latency
for the same mixed command stream at batch sizes 64, 256, and 1,024. It collects
1,024 timed batch samples after 128 warmup batches and reports p50, p95, p99,
and max ns/op as Google Benchmark counters. These values are amortized over
fixed-size batches and are not true single-operation latency.

Latest EC2 median throughput results:

| Benchmark | Commands | CPU Time | Throughput |
| --- | ---: | ---: | ---: |
| `BM_EndToEnd_ParseProcessFormat/1000` | 1,000 | 651418 ns | 1.535M commands/s |
| `BM_EndToEnd_ParseProcessFormat/10000` | 10,000 | 6991258 ns | 1.430M commands/s |
| `BM_EndToEnd_ParseProcessFormat/100000` | 100,000 | 95509290 ns | 1.047M commands/s |
| `BM_EndToEnd_MixedOrderFlow_Throughput/1000` | 1,000 | 727879 ns | 1.374M commands/s |
| `BM_EndToEnd_MixedOrderFlow_Throughput/10000` | 10,000 | 7268960 ns | 1.376M commands/s |
| `BM_EndToEnd_MixedOrderFlow_Throughput/100000` | 100,000 | 74442924 ns | 1.343M commands/s |

Latest EC2 mixed order-flow amortized batch latency results:

| Benchmark | Batch Size | Samples | p50 ns/op | p95 ns/op | p99 ns/op | Max ns/op |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_EndToEnd_MixedOrderFlow_Latency/64` | 64 | 1,024 | 726.28 | 803.27 | 904.25 | 1397.53 |
| `BM_EndToEnd_MixedOrderFlow_Latency/256` | 256 | 1,024 | 857.27 | 910.51 | 965.37 | 1124.97 |
| `BM_EndToEnd_MixedOrderFlow_Latency/1024` | 1,024 | 1,024 | 1096.41 | 1137.83 | 1173.40 | 1349.23 |

Artifact files:

- `benchmarks/end_to_end_results.txt`
- `benchmarks/end_to_end_results.json`

## Amortized Batch Latency

Throughput benchmarks remain Google Benchmark based. Latency benchmarks are
reported by the standalone `latency_benchmark` runner and time fixed-size
operation batches of 64, 256, and 1,024 operations with
`std::chrono::steady_clock`.

Latency results are reported as amortized nanoseconds per operation over
fixed-size timed batches. They are useful for comparing relative behavior across
workloads and commits, but should not be interpreted as true per-order
production latency.

The latency runner pre-generates order streams, cancel ids, and shuffled random
sequences before timing. Book construction, preload/setup, random generation,
and vector allocation are excluded from the measured batch loop. Each recorded
sample measures exactly one batch, computes `elapsed_ns / batch_size`, and the
artifact reports p50, p95, p99, and max over those amortized samples. This avoids
most per-operation timer overhead without pretending to measure true
single-operation latency.

The EC2 workflow is scripted in `benchmarks/run_ec2_benchmarks.sh`. It configures
a clean Release build, runs correctness tests first, runs the existing Google
Benchmark throughput binaries with repetitions, then runs the latency suite with
multiple trials. The script uses `taskset -c 0` when available and records
environment metadata next to the benchmark artifacts.

Latency artifact files:

- `benchmarks/latency_results.txt`
- `benchmarks/latency_results.json`

Latest latency result table:

The table below reports the median value across five latency trials for each
workload and batch size. Each trial recorded 1,024 timed batches after 128
warmup batches.

| Benchmark | Workload Size | Batch Size | Samples / Trial | Trials | p50 ns/op | p95 ns/op | p99 ns/op | Max ns/op |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `RestingLimitOrderInsert` | 73,728 | 64 | 1,024 | 5 | 88.05 | 108.19 | 140.39 | 628.48 |
| `RestingLimitOrderInsert` | 294,912 | 256 | 1,024 | 5 | 142.91 | 164.61 | 188.04 | 270.22 |
| `RestingLimitOrderInsert` | 1,179,648 | 1,024 | 1,024 | 5 | 171.44 | 184.82 | 201.17 | 290.42 |
| `CrossingLimitOrderMatch` | 73,728 | 64 | 1,024 | 5 | 62.39 | 80.83 | 97.66 | 230.30 |
| `CrossingLimitOrderMatch` | 294,912 | 256 | 1,024 | 5 | 120.08 | 149.38 | 178.64 | 240.03 |
| `CrossingLimitOrderMatch` | 1,179,648 | 1,024 | 1,024 | 5 | 165.90 | 260.26 | 271.33 | 313.90 |
| `CancelFront` | 73,728 | 64 | 1,024 | 5 | 32.70 | 41.73 | 47.27 | 193.73 |
| `CancelFront` | 294,912 | 256 | 1,024 | 5 | 39.37 | 58.94 | 80.68 | 107.21 |
| `CancelFront` | 1,179,648 | 1,024 | 1,024 | 5 | 148.04 | 217.88 | 230.55 | 246.32 |
| `CancelBack` | 73,728 | 64 | 1,024 | 5 | 32.20 | 39.83 | 44.88 | 179.69 |
| `CancelBack` | 294,912 | 256 | 1,024 | 5 | 46.74 | 55.16 | 82.86 | 110.97 |
| `CancelBack` | 1,179,648 | 1,024 | 1,024 | 5 | 140.62 | 152.44 | 157.92 | 171.79 |
| `CancelRandom` | 73,728 | 64 | 1,024 | 5 | 93.22 | 132.09 | 155.25 | 339.25 |
| `CancelRandom` | 294,912 | 256 | 1,024 | 5 | 337.83 | 384.77 | 411.42 | 713.30 |
| `CancelRandom` | 1,179,648 | 1,024 | 1,024 | 5 | 499.49 | 521.80 | 539.78 | 797.38 |
| `CancelUnknown` | 73,728 | 64 | 1,024 | 5 | 9.84 | 14.34 | 17.22 | 157.12 |
| `CancelUnknown` | 294,912 | 256 | 1,024 | 5 | 11.75 | 27.48 | 31.54 | 69.39 |
| `CancelUnknown` | 1,179,648 | 1,024 | 1,024 | 5 | 36.02 | 40.67 | 47.25 | 65.33 |
| `MixedSubmitCancel` | 73,728 | 64 | 1,024 | 5 | 48.00 | 72.77 | 114.25 | 2494.44 |
| `MixedSubmitCancel` | 294,912 | 256 | 1,024 | 5 | 91.61 | 110.44 | 143.21 | 4793.89 |
| `MixedSubmitCancel` | 1,179,648 | 1,024 | 1,024 | 5 | 133.54 | 162.34 | 173.34 | 7694.61 |
| `OrderBookTrueMixed` | 73,728 | 64 | 1,024 | 5 | 63.88 | 71.52 | 80.77 | 257.84 |
| `OrderBookTrueMixed` | 294,912 | 256 | 1,024 | 5 | 80.56 | 88.70 | 118.61 | 151.13 |
| `OrderBookTrueMixed` | 1,179,648 | 1,024 | 1,024 | 5 | 137.29 | 164.88 | 177.25 | 187.60 |

The mixed latency run had large max outliers in this EC2 pass, so p50/p95/p99
are more representative than the max column for that workload.

## Hardware Performance Counters

The EC2 latency workflow also has an additive `perf stat` counter pass in
`benchmarks/run_perf_counters.sh`. Run it after building the Release benchmark
binary, typically after `benchmarks/run_ec2_benchmarks.sh`, when latency changes
need a hardware-level explanation. The existing throughput and latency pipeline
is unchanged.

The script pins the latency runner with `taskset -c 0`, executes
`latency_benchmark` under `perf stat`, and records aggregate counters for:

- `cycles`
- `instructions`
- `branches`
- `branch-misses`
- `cache-references`
- `cache-misses`
- `L1-dcache-loads`
- `L1-dcache-load-misses`
- `LLC-loads`
- `LLC-load-misses`

Perf artifact files:

- `benchmarks/perf_results.txt`
- `benchmarks/perf_results.csv`

Some virtualized EC2 PMUs may not expose every requested cache event. The script
probes each counter before the measured run, skips unsupported events, and lists
them in `perf_results.txt` so the artifact remains reproducible instead of
silently changing the event set. If `perf` or `taskset` is missing on an Ubuntu
host, the script attempts to install the matching `linux-tools` or `util-linux`
package before failing with an explicit error.

Latest counter probe:

- Run date: `2026-05-18T17:32:14Z`
- Host: AWS EC2 `t3.small` on KVM
- Result: none of the requested hardware PMU events were exposed to `perf`
- Unsupported events: `cycles`, `instructions`, `branches`, `branch-misses`,
  `cache-references`, `cache-misses`, `L1-dcache-loads`,
  `L1-dcache-load-misses`, `LLC-loads`, and `LLC-load-misses`

Interpretation notes:

- Deep random cancel workloads are expected to be dominated by cache misses as
  lookup depth and shuffled order IDs put more pressure on the memory hierarchy.
- Front and back cancels should stay comparatively cache-local because the
  intrusive queue keeps unlinking `O(1)` once the order is found.
- Rising `LLC-load-misses` as workload depth grows indicates memory hierarchy
  pressure, especially on random cancel and mixed submit/cancel workloads.
- High branch-miss rates can indicate unpredictable random lookup paths rather
  than arithmetic or matching-rule cost.
- Cycles-per-instruction trends help explain whether a workload is becoming
  memory-bound or remains mostly compute-bound.

These counters complement latency percentiles by explaining why latency changes
occur. They are intentionally aggregate `perf stat` counters only; this workflow
does not add flamegraphs, `perf record`, `perf report`, or sampling profilers.

## Results

Human-readable output is saved in `benchmarks/*.txt`. Machine-readable Google
Benchmark JSON output and latency JSON output are saved in `benchmarks/*.json`
for future tooling, regression tracking, and CI integration. Longitudinal
benchmark rows are tracked in `docs/benchmark_history.md`.

### Resting Limit Order Inserts

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_RestingLimitOrderInsert/1000` | 34674 ns | 28.84M items/s |
| `BM_RestingLimitOrderInsert/10000` | 338965 ns | 29.50M items/s |
| `BM_RestingLimitOrderInsert/100000` | 8699497 ns | 11.49M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 34914 ns | 28.64M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 345913 ns | 28.91M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 5834668 ns | 17.14M items/s |

Artifact files:

- `benchmarks/match_results.txt`
- `benchmarks/match_results.json`

### Cancel Path

The results in this section are the latest recorded Linux EC2 cancel benchmarks.
They describe the flat hash-map lookup refactor, where `orders_by_id_` uses
`ankerl::unordered_dense::map<std::uint64_t, Order*>` and benchmark setup uses
workload-specific reserve-capacity hints before preload.

Architecture update:

- Previous implementation: price levels used `std::deque<Order>`, while the
  order-id index stored only side and price. Cancel complexity was
  `O(log P + Q)` because the book found the price level and then scanned the
  same-price queue for the target id.
- Current implementation: price levels use intrusive `OrderQueue` values, while
  the order-id index stores direct `Order*` values in a flat open-addressing
  hash map. Cancel complexity is `O(log P)` for the price-level lookup with
  `O(1)` unlink at the queue level.
- Locality effect: node-based `std::unordered_map` lookups can chase separately
  allocated nodes after probing the bucket array. `ankerl::unordered_dense`
  stores metadata and key/value entries in contiguous arrays, which reduces
  pointer chasing on random cancel and hash-miss paths.
- Tradeoff: flat maps generally provide weaker iterator/reference stability
  around insert, erase, and rehash than node-based maps. The book only keeps raw
  `Order*` values in the id index, while order lifetime remains owned by
  `OrderPool`.
- `P` is the number of price levels. `Q` is the queue length at one price level.

The deque baseline, iterator-based refactor, intrusive refactor, and dense
lookup refactor results were captured on the Linux EC2 benchmark host.

Run metadata:

- Baseline commit: `2a886e5` plus local cancel benchmark changes
- Refactor commit base: `bcaa292` plus local iterator-based cancel changes
- Intrusive refactor commit base: `e66c0d9` plus local intrusive `OrderQueue`/`OrderPool` changes
- Dense lookup refactor base: `2cba3e5` plus local `ankerl::unordered_dense` changes
- Symbol-free order base: `37e63d0` plus local symbol-free `Order` changes
- Structured event/cancel-result base: `00ca2b1` plus local structured-event/cancel-result changes
- Reusable submit event-buffer base: `523506d` plus local reusable-submit-buffer changes
- Amortized batch latency base: `6f581de` plus local latency benchmark changes
- Perf-counter instrumentation base: `dca7f50` plus local perf-counter instrumentation changes
- End-to-end benchmark base: `c3e1468` plus local end-to-end/reserve-capacity benchmark changes
- Reserve-capacity rule base: `e1eb134` plus local reserve-order-capacity rule changes
- Refactor host: AWS EC2 `t3.small`
- Correctness tests: 121/121 passed before benchmark execution

Latest full cancel artifact run:

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 18375 ns | 54.42M items/s |
| `BM_CancelFront/10000` | 169100 ns | 59.14M items/s |
| `BM_CancelFront/100000` | 3285726 ns | 30.43M items/s |
| `BM_CancelBack/1000` | 17343 ns | 57.66M items/s |
| `BM_CancelBack/10000` | 165130 ns | 60.56M items/s |
| `BM_CancelBack/100000` | 3192502 ns | 31.32M items/s |
| `BM_CancelRandom/1000` | 19915 ns | 50.21M items/s |
| `BM_CancelRandom/10000` | 268060 ns | 37.31M items/s |
| `BM_CancelRandom/100000` | 11788407 ns | 8.483M items/s |
| `BM_CancelUnknown/1000` | 7110 ns | 140.7M items/s |
| `BM_CancelUnknown/10000` | 57751 ns | 173.2M items/s |
| `BM_CancelUnknown/100000` | 864176 ns | 115.7M items/s |
| `BM_MixedSubmitCancel/1000` | 30646 ns | 32.63M items/s |
| `BM_MixedSubmitCancel/10000` | 331720 ns | 30.15M items/s |
| `BM_MixedSubmitCancel/100000` | 5482014 ns | 18.24M items/s |

Separate paired 3-second comparison run:

This table uses a focused before/after run of only the requested deepest
workloads. It is used for deltas because each workload received a longer
measurement window than the full artifact run above.

| Benchmark | Baseline Throughput | Dense Throughput | Throughput Delta | CPU Time Delta |
| --- | ---: | ---: | ---: | ---: |
| `BM_CancelRandom/100000` | 2.71735M items/s | 5.23192M items/s | +92.5% | -48.1% |
| `BM_CancelUnknown/100000` | 12.8103M items/s | 12.9185M items/s | +0.8% | -0.8% |
| `BM_MixedSubmitCancel/100000` | 6.1451M items/s | 7.01731M items/s | +14.2% | -12.4% |

Order-id load factor tuning run:

This focused run used only the 100,000-operation random cancel, unknown cancel,
and mixed submit/cancel benchmarks. Each load factor used the same Release
binary, `--benchmark_min_time=3s`, and 5 repetitions. Values below are Google
Benchmark mean aggregate rows, with deltas relative to the `0.80` baseline.

| Load Factor | Benchmark | CPU Time | Throughput | Throughput Delta vs 0.80 | CPU Time Delta vs 0.80 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0.50 | `BM_CancelRandom/100000` | 21599970 ns | 4.63827M items/s | -12.8% | +14.6% |
| 0.60 | `BM_CancelRandom/100000` | 21116797 ns | 4.75425M items/s | -10.7% | +12.0% |
| 0.70 | `BM_CancelRandom/100000` | 21477911 ns | 4.68257M items/s | -12.0% | +13.9% |
| 0.80 | `BM_CancelRandom/100000` | 18851725 ns | 5.32119M items/s | 0.0% | 0.0% |
| 0.50 | `BM_CancelUnknown/100000` | 10327630 ns | 9.78933M items/s | -24.3% | +33.3% |
| 0.60 | `BM_CancelUnknown/100000` | 9917648 ns | 10.1306M items/s | -21.6% | +28.0% |
| 0.70 | `BM_CancelUnknown/100000` | 9779039 ns | 10.2806M items/s | -20.5% | +26.2% |
| 0.80 | `BM_CancelUnknown/100000` | 7747137 ns | 12.925M items/s | 0.0% | 0.0% |
| 0.50 | `BM_MixedSubmitCancel/100000` | 16114586 ns | 6.22999M items/s | -16.0% | +19.4% |
| 0.60 | `BM_MixedSubmitCancel/100000` | 15833793 ns | 6.31734M items/s | -14.8% | +17.4% |
| 0.70 | `BM_MixedSubmitCancel/100000` | 16157134 ns | 6.19291M items/s | -16.5% | +19.7% |
| 0.80 | `BM_MixedSubmitCancel/100000` | 13492604 ns | 7.41336M items/s | 0.0% | 0.0% |

Recommendation: keep `orders_by_id_` at `0.80` for production. Lower load
factors shorten expected probe chains, but they spread the flat table over more
memory. On these deep EC2 workloads, the extra memory footprint appears to hurt
cache locality more than the shorter probes help. The `0.60` setting was the
best lower-density candidate, but it still trailed `0.80` by 10.7% on random
cancel throughput, 21.6% on unknown cancel throughput, and 14.8% on the mixed
workload.

Deepest cancel cases across the deque baseline, iterator refactor, intrusive
refactor, and dense lookup refactor:

| Benchmark | Deque Throughput | Iterator Throughput | Intrusive Throughput | Latest Throughput |
| --- | ---: | ---: | ---: | ---: |
| `BM_CancelFront/100000` | 12.0807M items/s | 10.6954M items/s | 14.5193M items/s | 30.43M items/s |
| `BM_CancelBack/100000` | 7.02396k items/s | 10.8996M items/s | 14.7098M items/s | 31.32M items/s |
| `BM_CancelRandom/100000` | 6.73904k items/s | 1.68836M items/s | 2.53093M items/s | 8.483M items/s |

Artifact files:

- `benchmarks/cancel_results.txt`
- `benchmarks/cancel_results.json`
- `benchmarks/benchmark_environment.txt`

## Initial Read

The current event API keeps cancellation on the direct single-result
`CancelResult` path, while submissions write into caller-owned
`std::vector<Event>` buffers. This keeps realistic event materialization in the
submit and match benchmarks without constructing and destroying a vector for
every submitted order.

The latest run keeps the existing Google Benchmark throughput suite intact,
uses `reserve_order_capacity = max(1024, operation_count / 10)` for the mixed
submit/cancel workload, and refreshes amortized batch latency artifacts for the
same hot paths. The deepest crossing-match workload measured 17.14M items/s,
while deepest mixed submit/cancel/match measured 18.24M items/s by median. The
mixed 100,000-operation throughput repetitions were noisy, so the median is more
representative than the mean for that workload. The most recent `perf` probe
still did not expose the requested hardware PMU events on this `t3.small` KVM
host, so the counter artifacts document the unsupported event set instead of
reporting cache or branch statistics.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
