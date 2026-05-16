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
- Commit: `a857109` plus local order-id load factor tuning changes
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG`
- Correctness tests: 20/20 passed before benchmark execution

## Workloads

- `insert_benchmark` measures passive BUY/SELL limit orders that do not cross,
  isolating resting order insertion throughput.
- `match_benchmark` preloads resting ask liquidity, then submits aggressive buy
  limit orders that cross and consume the book.
- `cancel_benchmark` preloads same-price FIFO liquidity, then measures front,
  back, random, and unknown cancels. It also includes a mixed submit/cancel/match
  stream with roughly 70% passive inserts, 20% cancels, and 10% crossing orders.

These workloads bypass parser, stdin, file I/O, and logging. Setup/preload work
is excluded from the measured loop where appropriate.

## Results

Human-readable output is saved in `benchmarks/*.txt`. Machine-readable Google
Benchmark JSON output is saved in `benchmarks/*.json` for future tooling,
regression tracking, and CI integration. Longitudinal benchmark rows are tracked
in `docs/benchmark_history.md`.

### Resting Limit Order Inserts

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_RestingLimitOrderInsert/1000` | 111114 ns | 8.99976M items/s |
| `BM_RestingLimitOrderInsert/10000` | 1073777 ns | 9.31292M items/s |
| `BM_RestingLimitOrderInsert/100000` | 14928181 ns | 6.69874M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 119688 ns | 8.35508M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 1180758 ns | 8.46914M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 17861517 ns | 5.59863M items/s |

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
- Refactor host: AWS EC2 `t3.small`
- Correctness tests: 20/20 passed before benchmark execution

Latest full cancel artifact run:

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 33270 ns | 30.0573M items/s |
| `BM_CancelFront/10000` | 319987 ns | 31.2513M items/s |
| `BM_CancelFront/100000` | 6858158 ns | 14.5812M items/s |
| `BM_CancelBack/1000` | 32164 ns | 31.0909M items/s |
| `BM_CancelBack/10000` | 402979 ns | 24.8152M items/s |
| `BM_CancelBack/100000` | 6169345 ns | 16.2092M items/s |
| `BM_CancelRandom/1000` | 34145 ns | 29.2865M items/s |
| `BM_CancelRandom/10000` | 452879 ns | 22.0809M items/s |
| `BM_CancelRandom/100000` | 19170720 ns | 5.21629M items/s |
| `BM_CancelUnknown/1000` | 73857 ns | 13.5398M items/s |
| `BM_CancelUnknown/10000` | 722294 ns | 13.8448M items/s |
| `BM_CancelUnknown/100000` | 7770355 ns | 12.8694M items/s |
| `BM_MixedSubmitCancel/1000` | 89751 ns | 11.1419M items/s |
| `BM_MixedSubmitCancel/10000` | 896010 ns | 11.1606M items/s |
| `BM_MixedSubmitCancel/100000` | 15904599 ns | 6.28749M items/s |

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

| Benchmark | Deque Throughput | Iterator Throughput | Intrusive Throughput | Dense Lookup Throughput |
| --- | ---: | ---: | ---: | ---: |
| `BM_CancelFront/100000` | 12.0807M items/s | 10.6954M items/s | 14.5193M items/s | 14.5812M items/s |
| `BM_CancelBack/100000` | 7.02396k items/s | 10.8996M items/s | 14.7098M items/s | 16.2092M items/s |
| `BM_CancelRandom/100000` | 6.73904k items/s | 1.68836M items/s | 2.53093M items/s | 5.21629M items/s |

Artifact files:

- `benchmarks/cancel_results.txt`
- `benchmarks/cancel_results.json`

## Initial Read

The latest insert results improved versus the prior intrusive-only artifact,
helped by reserving order-id lookup capacity before the timed submit loop. The
match results improved at 1,000 and 10,000 orders, while the 100,000-order match
run was slower than the prior artifact, so the deep match result should be
treated as a follow-up measurement point rather than a clean win.

The cancel results show that intrusive queues reduced allocator and node
overhead versus the iterator-based `std::list` refactor, and that the dense
order-id lookup improves the cache-sensitive random cancel path. At 100,000
orders, random cancel improved to 5.21629M items/s in the full latest run and to
5.23192M items/s in the paired 3-second comparison. The mixed workload also
improved in the longer paired run, though it remains more sensitive to insert,
erase, and matching churn than pure cancel lookup.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
