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
- Commit: `c3e1468` plus local end-to-end benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 121/121 passed before benchmark execution
- Run date: `2026-05-19T07:28:53Z`
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
| `BM_EndToEnd_ParseProcessFormat/1000` | 1,000 | 641101 ns | 1.55982M commands/s |
| `BM_EndToEnd_ParseProcessFormat/10000` | 10,000 | 6840502 ns | 1.46188M commands/s |
| `BM_EndToEnd_ParseProcessFormat/100000` | 100,000 | 92662402 ns | 1.07919M commands/s |
| `BM_EndToEnd_ReplayScenario/1000` | 1,000 | 848701 ns | 1.17827M commands/s |
| `BM_EndToEnd_ReplayScenario/10000` | 10,000 | 8429845 ns | 1.18626M commands/s |
| `BM_EndToEnd_ReplayScenario/100000` | 100,000 | 91189243 ns | 1.09662M commands/s |

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
| `RestingLimitOrderInsert` | 73,728 | 64 | 1,024 | 5 | 82.34 | 97.17 | 128.77 | 563.98 |
| `RestingLimitOrderInsert` | 294,912 | 256 | 1,024 | 5 | 120.09 | 144.84 | 170.54 | 495.33 |
| `RestingLimitOrderInsert` | 1,179,648 | 1,024 | 1,024 | 5 | 172.94 | 186.19 | 215.46 | 308.96 |
| `CrossingLimitOrderMatch` | 73,728 | 64 | 1,024 | 5 | 57.83 | 73.36 | 90.30 | 237.39 |
| `CrossingLimitOrderMatch` | 294,912 | 256 | 1,024 | 5 | 105.89 | 133.25 | 152.50 | 232.49 |
| `CrossingLimitOrderMatch` | 1,179,648 | 1,024 | 1,024 | 5 | 158.30 | 258.01 | 287.69 | 398.37 |
| `CancelFront` | 73,728 | 64 | 1,024 | 5 | 32.45 | 43.28 | 49.28 | 293.56 |
| `CancelFront` | 294,912 | 256 | 1,024 | 5 | 40.60 | 62.47 | 92.24 | 144.64 |
| `CancelFront` | 1,179,648 | 1,024 | 1,024 | 5 | 147.17 | 214.60 | 226.87 | 248.27 |
| `CancelBack` | 73,728 | 64 | 1,024 | 5 | 32.03 | 39.78 | 45.69 | 196.05 |
| `CancelBack` | 294,912 | 256 | 1,024 | 5 | 46.14 | 53.89 | 85.55 | 134.26 |
| `CancelBack` | 1,179,648 | 1,024 | 1,024 | 5 | 140.07 | 152.10 | 158.23 | 219.41 |
| `CancelRandom` | 73,728 | 64 | 1,024 | 5 | 94.36 | 132.17 | 165.12 | 296.20 |
| `CancelRandom` | 294,912 | 256 | 1,024 | 5 | 327.02 | 367.54 | 394.09 | 456.16 |
| `CancelRandom` | 1,179,648 | 1,024 | 1,024 | 5 | 503.23 | 529.29 | 546.41 | 705.53 |
| `CancelUnknown` | 73,728 | 64 | 1,024 | 5 | 10.06 | 14.77 | 17.41 | 154.77 |
| `CancelUnknown` | 294,912 | 256 | 1,024 | 5 | 14.15 | 31.48 | 36.27 | 88.09 |
| `CancelUnknown` | 1,179,648 | 1,024 | 1,024 | 5 | 36.09 | 41.39 | 48.89 | 64.81 |
| `MixedSubmitCancel` | 73,728 | 64 | 1,024 | 5 | 82.52 | 98.55 | 112.86 | 270.34 |
| `MixedSubmitCancel` | 294,912 | 256 | 1,024 | 5 | 136.82 | 160.03 | 194.10 | 269.84 |
| `MixedSubmitCancel` | 1,179,648 | 1,024 | 1,024 | 5 | 182.27 | 197.60 | 215.71 | 306.39 |

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
| `BM_RestingLimitOrderInsert/1000` | 35726 ns | 27.9908M items/s |
| `BM_RestingLimitOrderInsert/10000` | 342625 ns | 29.1864M items/s |
| `BM_RestingLimitOrderInsert/100000` | 8956498 ns | 11.1651M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 34845 ns | 28.6985M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 342823 ns | 29.1696M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 5877256 ns | 17.0147M items/s |

Artifact files:

- `benchmarks/match_results.txt`
- `benchmarks/match_results.json`

### Cancel Path

The results in this section are the latest recorded Linux EC2 cancel benchmarks.
They describe the flat hash-map lookup refactor, where `orders_by_id_` uses
`ankerl::unordered_dense::map<std::uint64_t, Order*>` and benchmark setup
reserves expected live-order capacity before preload.

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
- End-to-end benchmark base: `c3e1468` plus local end-to-end benchmark changes
- Refactor host: AWS EC2 `t3.small`
- Correctness tests: 121/121 passed before benchmark execution

Latest full cancel artifact run:

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 18121 ns | 55.1853M items/s |
| `BM_CancelFront/10000` | 168776 ns | 59.25M items/s |
| `BM_CancelFront/100000` | 4120772 ns | 24.2673M items/s |
| `BM_CancelBack/1000` | 17371 ns | 57.5679M items/s |
| `BM_CancelBack/10000` | 164979 ns | 60.6139M items/s |
| `BM_CancelBack/100000` | 3948901 ns | 25.3235M items/s |
| `BM_CancelRandom/1000` | 19771 ns | 50.5804M items/s |
| `BM_CancelRandom/10000` | 273750 ns | 36.5296M items/s |
| `BM_CancelRandom/100000` | 12587167 ns | 7.9446M items/s |
| `BM_CancelUnknown/1000` | 7077 ns | 141.306M items/s |
| `BM_CancelUnknown/10000` | 57867 ns | 172.809M items/s |
| `BM_CancelUnknown/100000` | 892094 ns | 112.096M items/s |
| `BM_MixedSubmitCancel/1000` | 30503 ns | 32.7834M items/s |
| `BM_MixedSubmitCancel/10000` | 301858 ns | 33.1281M items/s |
| `BM_MixedSubmitCancel/100000` | 7325240 ns | 13.6514M items/s |

Order pool reserve sweep:

This focused EC2 run keeps the mixed submit/cancel/match operation stream fixed
at 100,000 operations and varies only the explicit setup-time reserve capacity
used for the order-id map and `OrderPool`. A reserve capacity of `0` constructs
the book without calling `reserve_order_capacity`. Setup allocation remains
outside the timed benchmark loop, but the resulting memory layout and table
sizes affect the measured hot path. The benchmark observed a peak live depth of
about 40,003 resting orders.

Command:

```bash
taskset -c 0 ./build-release/cancel_reserve_sweep_benchmark \
  --benchmark_filter=BM_MixedSubmitCancelReserveSweep \
  --benchmark_repetitions=5 \
  --benchmark_out=benchmarks/order_pool_reserve_sweep_results.json \
  --benchmark_out_format=json
```

| Reserve Capacity | Max Live Orders | Median CPU Time | Median Throughput | Best Throughput | Worst Throughput | Jitter |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 40,003 | 5468915 ns | 18.2852M items/s | 18.8156M items/s | 17.798M items/s | 2.27% CPU CV |
| 1,000 | 40,003 | 5137813 ns | 19.4635M items/s | 19.7882M items/s | 18.2701M items/s | 3.28% CPU CV |
| 10,000 | 40,003 | 5001246 ns | 19.995M items/s | 20.3507M items/s | 18.187M items/s | 4.74% CPU CV |
| 100,000 | 40,003 | 6900086 ns | 14.4926M items/s | 15.1359M items/s | 13.2397M items/s | 5.37% CPU CV |
| 1,000,000 | 40,003 | 16091381 ns | 6.21451M items/s | 6.29124M items/s | 6.06895M items/s | 1.57% CPU CV |
| 10,000,000 | 40,003 | 19414420 ns | 5.15081M items/s | 5.27851M items/s | 4.09862M items/s | 11.44% CPU CV |

Conclusion: the mixed submit/cancel regression looks like over-reservation and
cache/TLB footprint damage, not underallocation. No reserve, 1,000 reserve, and
10,000 reserve all beat the current 100,000-operation reserve policy despite
allocating some blocks during the timed stream. The 100,000 reserve point is
both slower and much noisier, while 1,000,000 and 10,000,000 collapse
throughput. Do not treat this as a production policy change yet; a separate
change should evaluate a better default reserve heuristic against the other
OrderBook microbenchmarks.

Artifact files:

- `benchmarks/order_pool_reserve_sweep_results.txt`
- `benchmarks/order_pool_reserve_sweep_results.json`

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
| `BM_CancelFront/100000` | 12.0807M items/s | 10.6954M items/s | 14.5193M items/s | 33.9001M items/s |
| `BM_CancelBack/100000` | 7.02396k items/s | 10.8996M items/s | 14.7098M items/s | 35.7337M items/s |
| `BM_CancelRandom/100000` | 6.73904k items/s | 1.68836M items/s | 2.53093M items/s | 10.3807M items/s |

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
refreshes amortized batch latency artifacts for the same hot paths, and adds a
`perf stat` counter probe. The deepest crossing-match workload measured
20.41M items/s, while deepest mixed submit/cancel/match measured 18.798M
items/s. The perf probe did not expose the requested hardware PMU events on
this `t3.small` KVM host, so the counter artifacts document the unsupported
event set instead of reporting cache or branch statistics.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
