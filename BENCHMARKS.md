# Benchmarks

This document records baseline matching-engine throughput measurements from the
Linux EC2 benchmark host. Benchmarks are run manually and should not be executed
automatically by CMake or CI until that workflow is added intentionally.

## Environment

- Host: AWS EC2 `t3.small`
- OS: Ubuntu 26.04 LTS
- CPU: Intel Xeon Platinum 8259CL @ 2.50GHz
- Compiler: GCC/G++ 15.2.0
- Commit: `780a416`
- Build type: `Release`
- Release flags: `-O3 -DNDEBUG -march=native`
- Correctness tests: 12/12 passed before benchmark execution

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
regression tracking, and CI integration.

### Resting Limit Order Inserts

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_RestingLimitOrderInsert/1000` | 197760 ns | 5.05665M items/s |
| `BM_RestingLimitOrderInsert/10000` | 1866554 ns | 5.35747M items/s |
| `BM_RestingLimitOrderInsert/100000` | 20993583 ns | 4.76336M items/s |

Artifact files:

- `benchmarks/insert_results.txt`
- `benchmarks/insert_results.json`

### Crossing Limit Order Matching

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CrossingLimitOrderMatch/1000` | 172129 ns | 5.80959M items/s |
| `BM_CrossingLimitOrderMatch/10000` | 1894272 ns | 5.27907M items/s |
| `BM_CrossingLimitOrderMatch/100000` | 18652535 ns | 5.36120M items/s |

Artifact files:

- `benchmarks/match_results.txt`
- `benchmarks/match_results.json`

### Cancel Path

Run metadata:

- Commit: `2a886e5` plus local cancel benchmark changes
- Correctness tests: 19/19 passed before benchmark execution

| Benchmark | CPU Time | Throughput |
| --- | ---: | ---: |
| `BM_CancelFront/1000` | 80996 ns | 12.3462M items/s |
| `BM_CancelFront/10000` | 788499 ns | 12.6823M items/s |
| `BM_CancelFront/100000` | 8277653 ns | 12.0807M items/s |
| `BM_CancelBack/1000` | 603849 ns | 1.65604M items/s |
| `BM_CancelBack/10000` | 56368146 ns | 177.405k items/s |
| `BM_CancelBack/100000` | 1.4237e+10 ns | 7.02396k items/s |
| `BM_CancelRandom/1000` | 1172999 ns | 852.516k items/s |
| `BM_CancelRandom/10000` | 107371953 ns | 93.1342k items/s |
| `BM_CancelRandom/100000` | 1.4839e+10 ns | 6.73904k items/s |
| `BM_CancelUnknown/1000` | 84651 ns | 11.8133M items/s |
| `BM_CancelUnknown/10000` | 843077 ns | 11.8613M items/s |
| `BM_CancelUnknown/100000` | 7945855 ns | 12.5852M items/s |
| `BM_MixedSubmitCancel/1000` | 136836 ns | 7.308M items/s |
| `BM_MixedSubmitCancel/10000` | 1295960 ns | 7.71629M items/s |
| `BM_MixedSubmitCancel/100000` | 13352105 ns | 7.48946M items/s |

Artifact files:

- `benchmarks/cancel_results.txt`
- `benchmarks/cancel_results.json`

## Initial Read

The baseline appears mostly CPU/allocation-bound rather than price-tree-bound.
These workloads use a small number of price levels, so balanced-tree traversal
is not yet under significant pressure. The current hot path still allocates and
mutates standard containers for submitted orders, event vectors, strings,
deques, maps, and the order-id index.

The cancel results show the expected split between index-only work and FIFO
queue scans. Front cancels and unknown cancels stay near constant throughput,
while back and randomized cancels degrade sharply at deep same-price levels
because the current cancel path finds the order by scanning the deque.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
