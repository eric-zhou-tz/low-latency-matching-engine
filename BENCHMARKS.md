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

Both workloads bypass parser, stdin, file I/O, and logging. Setup/preload work
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

## Initial Read

The baseline appears mostly CPU/allocation-bound rather than price-tree-bound.
These workloads use a small number of price levels, so balanced-tree traversal
is not yet under significant pressure. The current hot path still allocates and
mutates standard containers for submitted orders, event vectors, strings,
deques, maps, and the order-id index.

Future benchmark iterations should add workloads with wider price distributions
and deeper books before drawing stronger conclusions about tree-structure costs.
