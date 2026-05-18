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
- Commit: `6f581de` plus local amortized-batch-latency benchmark changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 26/26 passed before benchmark execution
- Run date: `2026-05-18T06:54:52Z`

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

These workloads bypass parser, stdin, file I/O, and logging. Setup/preload work
is excluded from the measured loop where appropriate.

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
| `RestingLimitOrderInsert` | 73,728 | 64 | 1,024 | 5 | 85.17 | 100.31 | 125.91 | 522.05 |
| `RestingLimitOrderInsert` | 294,912 | 256 | 1,024 | 5 | 111.64 | 125.99 | 158.30 | 896.73 |
| `RestingLimitOrderInsert` | 1,179,648 | 1,024 | 1,024 | 5 | 179.83 | 191.75 | 206.41 | 302.72 |
| `CrossingLimitOrderMatch` | 73,728 | 64 | 1,024 | 5 | 49.45 | 64.41 | 80.91 | 199.30 |
| `CrossingLimitOrderMatch` | 294,912 | 256 | 1,024 | 5 | 63.81 | 101.38 | 111.52 | 150.08 |
| `CrossingLimitOrderMatch` | 1,179,648 | 1,024 | 1,024 | 5 | 151.34 | 251.11 | 268.84 | 327.51 |
| `CancelFront` | 73,728 | 64 | 1,024 | 5 | 28.89 | 38.22 | 42.44 | 201.73 |
| `CancelFront` | 294,912 | 256 | 1,024 | 5 | 38.44 | 54.10 | 74.75 | 120.66 |
| `CancelFront` | 1,179,648 | 1,024 | 1,024 | 5 | 145.92 | 230.46 | 244.99 | 273.51 |
| `CancelBack` | 73,728 | 64 | 1,024 | 5 | 27.86 | 35.84 | 41.36 | 185.19 |
| `CancelBack` | 294,912 | 256 | 1,024 | 5 | 36.92 | 51.15 | 72.89 | 129.76 |
| `CancelBack` | 1,179,648 | 1,024 | 1,024 | 5 | 139.67 | 150.20 | 155.13 | 170.88 |
| `CancelRandom` | 73,728 | 64 | 1,024 | 5 | 80.08 | 119.88 | 139.12 | 280.34 |
| `CancelRandom` | 294,912 | 256 | 1,024 | 5 | 313.30 | 363.28 | 386.71 | 484.33 |
| `CancelRandom` | 1,179,648 | 1,024 | 1,024 | 5 | 492.00 | 516.70 | 523.77 | 565.47 |
| `CancelUnknown` | 73,728 | 64 | 1,024 | 5 | 10.25 | 14.45 | 16.94 | 159.06 |
| `CancelUnknown` | 294,912 | 256 | 1,024 | 5 | 12.45 | 26.24 | 30.27 | 64.70 |
| `CancelUnknown` | 1,179,648 | 1,024 | 1,024 | 5 | 38.48 | 45.17 | 51.39 | 69.08 |
| `MixedSubmitCancel` | 73,728 | 64 | 1,024 | 5 | 61.36 | 77.52 | 84.30 | 245.22 |
| `MixedSubmitCancel` | 294,912 | 256 | 1,024 | 5 | 112.14 | 133.16 | 154.61 | 204.82 |
| `MixedSubmitCancel` | 1,179,648 | 1,024 | 1,024 | 5 | 165.69 | 180.98 | 191.54 | 301.92 |

## Results

Human-readable output is saved in `benchmarks/*.txt`. Machine-readable Google
Benchmark JSON output and latency JSON output are saved in `benchmarks/*.json`
for future tooling, regression tracking, and CI integration. Longitudinal
benchmark rows are tracked in `docs/benchmark_history.md`.

### Resting Limit Order Inserts

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_RestingLimitOrderInsert/1000` | 31362 ns | 31.8854M items/s |
| `BM_RestingLimitOrderInsert/10000` | 597540 ns | 17.6971M items/s |
| `BM_RestingLimitOrderInsert/100000` | 6216070 ns | 16.0882M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 31368 ns | 31.8803M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 308796 ns | 32.3838M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 4864998 ns | 20.5566M items/s |

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
- Refactor host: AWS EC2 `t3.small`
- Correctness tests: 26/26 passed before benchmark execution

Latest full cancel artifact run:

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 16559 ns | 60.3903M items/s |
| `BM_CancelFront/10000` | 158706 ns | 63.2502M items/s |
| `BM_CancelFront/100000` | 2935703 ns | 34.069M items/s |
| `BM_CancelBack/1000` | 16102 ns | 62.1035M items/s |
| `BM_CancelBack/10000` | 148024 ns | 67.5564M items/s |
| `BM_CancelBack/100000` | 2781006 ns | 35.959M items/s |
| `BM_CancelRandom/1000` | 18925 ns | 53.0964M items/s |
| `BM_CancelRandom/10000` | 228504 ns | 43.7639M items/s |
| `BM_CancelRandom/100000` | 9608913 ns | 10.4114M items/s |
| `BM_CancelUnknown/1000` | 8089 ns | 123.625M items/s |
| `BM_CancelUnknown/10000` | 68107 ns | 146.827M items/s |
| `BM_CancelUnknown/100000` | 968597 ns | 103.564M items/s |
| `BM_MixedSubmitCancel/1000` | 27070 ns | 36.9416M items/s |
| `BM_MixedSubmitCancel/10000` | 260516 ns | 38.3875M items/s |
| `BM_MixedSubmitCancel/100000` | 5279053 ns | 18.9449M items/s |

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
| `BM_CancelFront/100000` | 12.0807M items/s | 10.6954M items/s | 14.5193M items/s | 34.069M items/s |
| `BM_CancelBack/100000` | 7.02396k items/s | 10.8996M items/s | 14.7098M items/s | 35.959M items/s |
| `BM_CancelRandom/100000` | 6.73904k items/s | 1.68836M items/s | 2.53093M items/s | 10.4114M items/s |

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

The latest run keeps the existing Google Benchmark throughput suite intact and
adds amortized batch latency artifacts for the same hot paths. Versus the prior
reusable-submit-buffer run, the deepest crossing-match workload moved from
19.9854M to 20.5566M items/s, while deepest mixed submit/cancel/match moved
from 14.6258M to 18.9449M items/s. The 10,000-order insert and a few cancel
rows were noisier than the surrounding workloads, so avoid over-reading small
deltas from this single full-suite pass.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
