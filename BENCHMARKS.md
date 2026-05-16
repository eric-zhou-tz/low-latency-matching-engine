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
- Commit: `e66c0d9` plus local intrusive `OrderQueue`/`OrderPool` changes
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
| `BM_RestingLimitOrderInsert/1000` | 173879 ns | 5.75114M items/s |
| `BM_RestingLimitOrderInsert/10000` | 1706820 ns | 5.85885M items/s |
| `BM_RestingLimitOrderInsert/100000` | 18432261 ns | 5.42527M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 165630 ns | 6.03756M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 1613590 ns | 6.19736M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 16377990 ns | 6.10576M items/s |

Artifact files:

- `benchmarks/match_results.txt`
- `benchmarks/match_results.json`

### Cancel Path

The results in this section are the latest recorded Linux EC2 cancel benchmarks.
They describe the intrusive FIFO queue refactor, where orders embed their own
queue links and the order book stores direct `Order*` cancel locations.

Architecture update:

- Previous implementation: price levels used `std::deque<Order>`, while the
  order-id index stored only side and price. Cancel complexity was
  `O(log P + Q)` because the book found the price level and then scanned the
  same-price queue for the target id.
- Current implementation: price levels use intrusive `OrderQueue` values, while
  the order-id index stores direct `Order*` values. Cancel complexity is
  `O(log P)` for the price-level lookup with `O(1)` unlink at the queue level.
- `P` is the number of price levels. `Q` is the queue length at one price level.

The deque baseline, iterator-based refactor, and intrusive refactor results were
captured on the Linux EC2 benchmark host.

Run metadata:

- Baseline commit: `2a886e5` plus local cancel benchmark changes
- Refactor commit base: `bcaa292` plus local iterator-based cancel changes
- Intrusive refactor commit base: `e66c0d9` plus local intrusive `OrderQueue`/`OrderPool` changes
- Refactor host: AWS EC2 `t3.small`
- Correctness tests: 20/20 passed before benchmark execution

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 66562 ns | 15.0237M items/s |
| `BM_CancelFront/10000` | 649222 ns | 15.4031M items/s |
| `BM_CancelFront/100000` | 6887367 ns | 14.5193M items/s |
| `BM_CancelBack/1000` | 67762 ns | 14.7576M items/s |
| `BM_CancelBack/10000` | 664354 ns | 15.0522M items/s |
| `BM_CancelBack/100000` | 6798211 ns | 14.7098M items/s |
| `BM_CancelRandom/1000` | 77359 ns | 12.9267M items/s |
| `BM_CancelRandom/10000` | 975962 ns | 10.2463M items/s |
| `BM_CancelRandom/100000` | 39511151 ns | 2.53093M items/s |
| `BM_CancelUnknown/1000` | 105877 ns | 9.44488M items/s |
| `BM_CancelUnknown/10000` | 836130 ns | 11.9599M items/s |
| `BM_CancelUnknown/100000` | 7854059 ns | 12.7323M items/s |
| `BM_MixedSubmitCancel/1000` | 137654 ns | 7.26457M items/s |
| `BM_MixedSubmitCancel/10000` | 1339162 ns | 7.46735M items/s |
| `BM_MixedSubmitCancel/100000` | 14213347 ns | 7.03564M items/s |

Deque baseline, iterator refactor, and intrusive refactor for the deepest cancel
cases:

| Benchmark | Deque Throughput | Iterator Throughput | Intrusive Throughput |
| --- | ---: | ---: | ---: |
| `BM_CancelFront/100000` | 12.0807M items/s | 10.6954M items/s | 14.5193M items/s |
| `BM_CancelBack/100000` | 7.02396k items/s | 10.8996M items/s | 14.7098M items/s |
| `BM_CancelRandom/100000` | 6.73904k items/s | 1.68836M items/s | 2.53093M items/s |

Artifact files:

- `benchmarks/cancel_results.txt`
- `benchmarks/cancel_results.json`

## Initial Read

The insert and match baselines improved after moving resting order storage into
intrusive queues backed by pooled order slots. These workloads still use a small
number of price levels, so balanced-tree traversal is not yet under significant
pressure.

The cancel results show that intrusive queues reduced allocator and node
overhead versus the iterator-based `std::list` refactor. Front and back cancels
are now around 14.5M-14.7M items/s at 100,000 orders. Random cancel improved
from 1.68836M items/s to 2.53093M items/s at 100,000 orders, but it still trails
front/back cancellation because the shuffled cancel sequence remains dominated
by hash-table lookup and cache locality across a large live-order set.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
