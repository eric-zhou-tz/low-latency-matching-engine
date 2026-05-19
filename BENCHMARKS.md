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
- Commit: `e1eb134` plus local reserve-order-capacity rule changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 121/121 passed before benchmark execution
- Run date: `2026-05-19T18:55:58Z`
- Command: `THROUGHPUT_REPETITIONS=5 LATENCY_TRIALS=5 benchmarks/run_ec2_benchmarks.sh`

## Workloads

- `insert_benchmark` measures passive BUY/SELL limit orders that do not cross,
  isolating resting order insertion throughput.
- `match_benchmark` preloads resting ask liquidity, then submits aggressive buy
  limit orders that cross and consume the book.
- `cancel_benchmark` preloads same-price FIFO liquidity, then measures front,
  back, random, and unknown cancels. It also includes a mixed submit/cancel/match
  stream with roughly 70% passive inserts, 20% cancels, and 10% crossing orders.
- `latency_benchmark` runs a separate amortized batch latency suite over the
  same hot paths. It is not a Google Benchmark replacement and does not rename
  or replace the throughput benchmark binaries.
- `end_to_end_benchmark` measures public-boundary CLI-style overhead:
  in-memory input lines, parser, exchange routing, order-book mutation, and
  event formatting.

The hot-path throughput and latency workloads bypass parser, stdin, file I/O,
and logging. Setup/preload work is excluded from the measured loop where
appropriate.

## End-to-end benchmarks

The end-to-end benchmarks measure full public-boundary system overhead:

`input lines -> Parser -> Exchange -> OrderBook -> Event formatting`

They pre-generate deterministic command scripts in memory before timing, avoid
filesystem I/O inside the timed loop, and include event formatting in the timed
work. These are parser/exchange/formatter overhead measurements and should not
be compared directly to OrderBook hot-path microbenchmarks.

`BM_EndToEnd_ParseProcessFormat` uses deterministic non-crossing multi-symbol
limit-order input to focus on parse, route, accept, and format cost.
`BM_EndToEnd_ReplayScenario` uses replay-style multi-symbol streams containing
inserts, crosses, cancels, modifies, market orders, IOC, and FOK commands.

Latest EC2 median results:

| Benchmark | Commands | CPU Time | Throughput |
| --- | ---: | ---: | ---: |
| `BM_EndToEnd_ParseProcessFormat/1000` | 1,000 | 658470 ns | 1.5187M commands/s |
| `BM_EndToEnd_ParseProcessFormat/10000` | 10,000 | 7114647 ns | 1.4056M commands/s |
| `BM_EndToEnd_ParseProcessFormat/100000` | 100,000 | 94438475 ns | 1.0589M commands/s |
| `BM_EndToEnd_ReplayScenario/1000` | 1,000 | 849665 ns | 1.1769M commands/s |
| `BM_EndToEnd_ReplayScenario/10000` | 10,000 | 8532050 ns | 1.1721M commands/s |
| `BM_EndToEnd_ReplayScenario/100000` | 100,000 | 97765103 ns | 1.0229M commands/s |

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
| `RestingLimitOrderInsert` | 73,728 | 64 | 1,024 | 5 | 92.27 | 132.97 | 180.62 | 563.64 |
| `RestingLimitOrderInsert` | 294,912 | 256 | 1,024 | 5 | 150.12 | 173.35 | 197.67 | 233.14 |
| `RestingLimitOrderInsert` | 1,179,648 | 1,024 | 1,024 | 5 | 171.95 | 188.18 | 226.67 | 418.26 |
| `CrossingLimitOrderMatch` | 73,728 | 64 | 1,024 | 5 | 58.84 | 76.27 | 103.72 | 245.70 |
| `CrossingLimitOrderMatch` | 294,912 | 256 | 1,024 | 5 | 110.45 | 141.38 | 169.14 | 226.87 |
| `CrossingLimitOrderMatch` | 1,179,648 | 1,024 | 1,024 | 5 | 162.80 | 259.52 | 284.31 | 335.36 |
| `CancelFront` | 73,728 | 64 | 1,024 | 5 | 33.23 | 43.58 | 48.91 | 226.52 |
| `CancelFront` | 294,912 | 256 | 1,024 | 5 | 41.36 | 74.93 | 91.09 | 147.35 |
| `CancelFront` | 1,179,648 | 1,024 | 1,024 | 5 | 148.07 | 216.25 | 231.86 | 246.89 |
| `CancelBack` | 73,728 | 64 | 1,024 | 5 | 33.05 | 40.47 | 46.88 | 193.39 |
| `CancelBack` | 294,912 | 256 | 1,024 | 5 | 45.98 | 58.81 | 81.52 | 150.88 |
| `CancelBack` | 1,179,648 | 1,024 | 1,024 | 5 | 139.94 | 152.27 | 158.94 | 235.31 |
| `CancelRandom` | 73,728 | 64 | 1,024 | 5 | 89.84 | 130.50 | 159.16 | 412.25 |
| `CancelRandom` | 294,912 | 256 | 1,024 | 5 | 338.02 | 380.86 | 402.86 | 497.50 |
| `CancelRandom` | 1,179,648 | 1,024 | 1,024 | 5 | 497.07 | 523.03 | 582.65 | 845.81 |
| `CancelUnknown` | 73,728 | 64 | 1,024 | 5 | 10.56 | 15.59 | 18.27 | 144.44 |
| `CancelUnknown` | 294,912 | 256 | 1,024 | 5 | 12.41 | 28.29 | 32.39 | 72.28 |
| `CancelUnknown` | 1,179,648 | 1,024 | 1,024 | 5 | 36.36 | 40.29 | 47.53 | 66.69 |
| `MixedSubmitCancel` | 73,728 | 64 | 1,024 | 5 | 45.81 | 80.83 | 128.08 | 2220.33 |
| `MixedSubmitCancel` | 294,912 | 256 | 1,024 | 5 | 97.36 | 127.50 | 179.64 | 7987.60 |
| `MixedSubmitCancel` | 1,179,648 | 1,024 | 1,024 | 5 | 144.72 | 172.43 | 193.47 | 10604.05 |

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
