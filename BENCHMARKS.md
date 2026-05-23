# Benchmarks

## Executive Summary

Latest full suite: pinned-core EC2/Linux run on `2026-05-20T22:43:35Z`.

| Item | Latest EC2 Run |
| --- | --- |
| Environment | AWS EC2 `t3.small`, Ubuntu Linux, `taskset -c 0` |
| CPU | Intel Xeon Platinum 8259CL @ 2.50GHz |
| Compiler | GCC/G++ 15.2.0 |
| Build flags | `-O3 -DNDEBUG -march=native` |
| Framework | Google Benchmark for throughput; custom fixed-batch latency runner |
| Validation | 127/127 CTest cases passed before benchmarks |
| Source | Local working tree based on `5cf8fd9`, transferred without `.git` metadata |

| Highlight | Metric |
| --- | ---: |
| OrderBook true mixed hot path, 100,000 operations | `15.84M ops/sec` |
| OrderBook true mixed p99 batch latency, 256-op batches | `0.122 us/op` |
| End-to-end true mixed flow, 100,000 commands | `1.34M commands/sec` |
| End-to-end true mixed p99 batch latency, 256-command batches | `1.020 us/op` |
| Best-level churn stress, 1,000,000 operations | `16.08M ops/sec` |

The suite separates direct matching-core performance from public-boundary cost. Hot-path results isolate `OrderBook`; end-to-end results include parser, exchange routing, matching, and event formatting.

## Benchmark Methodology

The numbers in this report come from a clean Release build on Linux/EC2 with CPU pinning enabled via `taskset -c 0`. The benchmark runner records environment metadata, commit hash when available, compiler version, build type, flags, selected benchmark targets, and system CPU details.

Rigor controls:

| Control | Practice |
| --- | --- |
| Build mode | Release build with `-O3 -DNDEBUG -march=native` |
| CPU noise | Fixed CPU affinity / pinned core; note any missing `taskset` support |
| Workload repeatability | Fixed RNG seeds and deterministic pre-generated workloads |
| Timing boundaries | Setup, allocation, preload, and RNG generation outside measured loops |
| Memory behavior | Reusable event buffers; reserve hints sized from modeled live depth where applicable |
| I/O | No filesystem I/O inside timed benchmark loops |
| Latency | Amortized fixed-batch latency, not true single-operation latency |
| Compiler barriers | `benchmark::DoNotOptimize(...)` and `benchmark::ClobberMemory()` where benchmarked state/results must stay visible |
| Validation | Correctness tests before EC2 benchmark execution |

Hot-path benchmarks measure typed matching operations directly against `OrderBook`. Realistic-flow benchmarks measure either the same mixed order stream on `OrderBook` or the public parser/exchange/formatter path. Stress benchmarks target adversarial book shapes. Replay benchmarks exercise deterministic fixture streams through the public path.

## Core Hot Path

Core hot-path benchmarks measure the matching engine without parser, CLI, filesystem, or formatting overhead. These are the best indicators of data-structure cost in the matching core.

### Throughput

| Benchmark | Description | Input Size | Throughput |
| --- | --- | ---: | ---: |
| Passive Insert | Non-crossing GTC limit orders resting on the book | 100,000 operations | `12.10M ops/sec` |
| One-Level Crossing Match | Aggressive orders consuming resting liquidity at one price level | 100,000 operations | `16.49M ops/sec` |
| Random Cancel | Cancel by live order id with shuffled lookup order | 100,000 operations | `8.02M ops/sec` |
| Unknown Cancel | Rejected cancel path through the order-id lookup miss path | 100,000 operations | `114.02M ops/sec` |
| Modify If Present | Same-price quantity reduction preserving FIFO priority | 100,000 operations | `30.01M ops/sec` |

### Latency

Latency rows report the median across five trials at 256 operations per timed batch. These are amortized batch measurements, not true single-operation latency. The runner does not currently report p999.

| Benchmark | p50 | p99 | p999 | Max |
| --- | ---: | ---: | ---: | ---: |
| Passive Insert | `0.129 us` | `0.179 us` | `N/A` | `0.500 us` |
| One-Level Crossing Match | `0.111 us` | `0.162 us` | `N/A` | `0.216 us` |
| Random Cancel | `0.330 us` | `0.393 us` | `N/A` | `0.457 us` |
| Unknown Cancel | `0.012 us` | `0.034 us` | `N/A` | `0.073 us` |
| Modify If Present | `0.070 us` | `0.120 us` | `N/A` | `0.528 us` |

Engineering notes:

| Observation | Why It Matters |
| --- | --- |
| Intrusive FIFO queues keep cancel and match removal O(1) once the order is found. | This preserves price-time priority while avoiding same-price queue scans. |
| The flat order-id map improves cancel lookup locality. | Random cancels remain cache-sensitive because ids touch the lookup table in shuffled order. |
| Balanced trees back price levels. | Best-price traversal is deterministic and ordered, but sparse deep books pay tree traversal and erase costs. |
| Reusable event buffers reduce transient allocation pressure. | Submit/match operations can emit multiple events without returning a fresh vector every time. |

## Realistic Flow

Realistic-flow benchmarks use a deterministic mixed stream: GTC submits, cancels, modifies, IOC orders, market orders, and FOK orders. The direct `OrderBook` variant isolates matching behavior; the end-to-end variant includes parser, exchange routing, and event formatting.

### Throughput

| Benchmark | Description | Input Size | Throughput |
| --- | --- | ---: | ---: |
| OrderBook True Mixed | Direct matching-core mixed exchange flow | 100,000 operations | `15.84M ops/sec` |
| End-to-End Passive Insert | Public command path for non-crossing inserts | 100,000 commands | `1.04M commands/sec` |
| End-to-End True Mixed | Parser -> Exchange -> OrderBook -> event formatting | 100,000 commands | `1.34M commands/sec` |

### Latency

Latency rows report 256-operation amortized batches. The `OrderBook` row is the median across five standalone latency trials; the end-to-end row is the Google Benchmark median counter row. The suite does not currently report p999.

| Benchmark | p50 | p99 | p999 | Max |
| --- | ---: | ---: | ---: | ---: |
| OrderBook True Mixed | `0.081 us` | `0.122 us` | `N/A` | `0.147 us` |
| End-to-End True Mixed | `0.879 us` | `1.020 us` | `N/A` | `1.560 us` |

Engineering notes:

| Observation | Why It Matters |
| --- | --- |
| The mixed stream is randomly interleaved with fixed seeds rather than phase-based. | It better resembles exchange traffic where passive, cancel, modify, and taker flow interact continuously. |
| IOC, FOK, and market orders are transient taker flow. | They exercise rejection, expiration, and multi-level matching without polluting cancel/modify live sets. |
| End-to-end throughput is intentionally lower than hot-path throughput. | It includes command parsing, symbol routing, variant/event handling, and string formatting overhead. |
| Batch latency is amortized. | It is useful for comparing workload shape and regressions, but it is not a true one-order tail latency claim. |

## Stress

Stress benchmarks are adversarial `OrderBook` shapes. They are useful for finding data-structure cliffs and noisy cases, not for advertising a single headline number.

### Throughput

| Benchmark | Description | Input Size | Throughput |
| --- | --- | ---: | ---: |
| Best-Level Churn | Repeated top-of-book cancel, improve, match, and modify flow | 1,000,000 operations | `16.08M ops/sec` |
| Level Create/Delete Churn | Repeated creation and cleanup of short-lived price levels | 1,000,000 operations | `23.08M ops/sec` |
| Shallow GTC Mixed | Dense, low-depth GTC churn with a cache-hot working set | 100,000 primary ops / 185,000 book actions | `18.45M ops/sec` |
| Deep Sparse GTC Mixed | 50,000 occupied price levels with sparse liquidity | 100,000 primary ops / 50,000 preloaded levels | `1.74M ops/sec` |

### Latency

Stress latency is not measured by the current suite.

| Benchmark | p50 | p99 | p999 | Max |
| --- | ---: | ---: | ---: | ---: |
| Best-Level Churn | `N/A` | `N/A` | `N/A` | `N/A` |
| Level Create/Delete Churn | `N/A` | `N/A` | `N/A` | `N/A` |
| Shallow GTC Mixed | `N/A` | `N/A` | `N/A` | `N/A` |
| Deep Sparse GTC Mixed | `N/A` | `N/A` | `N/A` | `N/A` |

Engineering notes:

| Observation | Why It Matters |
| --- | --- |
| Best-level churn repeatedly mutates the inside market. | It stresses best-price maintenance and tree updates near the prices that matter most. |
| Level create/delete churn forces price-level lifetime turnover. | It exposes allocator, tree insert/erase, empty-level cleanup, and cache locality costs. |
| Shallow dense books tend to stay cache-hot. | High throughput here does not imply the same behavior for sparse, deep books. |
| Deep sparse books stress balanced-tree traversal. | Throughput drops because each operation touches a much larger ordered price-level set. |
| The 1M level create/delete run had noisy repetitions. | The reported value is the Google Benchmark median; mean was less representative for this row. |

Experimental or legacy rows, such as reserve-capacity sweeps and the older 70/20/10 mixed submit/cancel path, should stay out of the main report unless they explain a tuning decision.

## Determinism / Replay

Replay benchmarks use golden fixtures and deterministic command streams. They are primarily credibility checks: the same public input should produce the same public output while still giving a rough throughput signal for replay-style workloads.

### Throughput

| Benchmark | Description | Input Size | Throughput |
| --- | --- | ---: | ---: |
| Golden Fixture Replay | Parser/exchange/formatter path over in-memory replay fixtures | 16 fixtures / 856 input bytes | `268.15k fixture-runs/sec` |

### Latency

Replay latency is not measured by the current suite.

| Benchmark | p50 | p99 | p999 | Max |
| --- | ---: | ---: | ---: | ---: |
| Golden Fixture Replay | `N/A` | `N/A` | `N/A` | `N/A` |

Engineering notes:

| Observation | Why It Matters |
| --- | --- |
| Fixture bytes are loaded before timing. | Replay timing avoids filesystem I/O in the measured loop. |
| Replay uses the same parser/exchange/formatter path as golden tests. | Performance data stays tied to deterministic public behavior rather than only private APIs. |
| Exact-output replay tests guard semantic drift. | Matching changes can be performance-tested without losing deterministic auditability. |

## Historical Improvements

| Version | Major Change | Impact |
| --- | --- | --- |
| v0.2.0 | Added limit-order matching with price-time priority and FIFO price levels. | Established the core deterministic matching model. |
| v0.3.0 | Added cancel support with a live order-id index. | Made cancellation deterministic and enabled cancel-path benchmarking. |
| v0.3.1 | Replaced node-based FIFO queues with intrusive `OrderQueue`. | Removed same-price queue scans and made unlink by order id O(1). |
| v0.3.2 | Added pooled stable order storage. | Reduced hot-path allocation/deallocation pressure for resting orders. |
| v0.3.4 | Moved order-id lookup to `ankerl::unordered_dense::map`. | Improved cache locality for cancel-heavy workloads. |
| v0.3.6 | Replaced hot-path event strings with structured event payloads. | Reduced formatting/allocation work inside matching paths. |
| v0.3.7 | Added caller-owned reusable submit event buffers. | Reduced vector churn for submit and match operations. |
| v0.3.8 | Added amortized batch latency runner and EC2 pinning workflow. | Separated throughput from latency-style regression signals. |
| v0.4.0-v0.4.2 | Added market, IOC/FOK, and modify flows. | Expanded the benchmark surface toward realistic exchange behavior. |
| v0.5.1 | Added golden replay fixtures. | Made deterministic public-boundary behavior testable over command tapes. |
| v0.6.0-v0.6.3 | Added end-to-end and true-mixed benchmark coverage. | Paired matching-core results with parser/exchange/formatter overhead. |
| v0.6.4-v0.6.6 | Added stress/soak, shallow/deep, best-level, and level churn workloads. | Exposed behavior under sustained churn and adversarial book shapes. |
| v0.7.1 | Reverted an experimental storage comparison after regressions. | Preserved the stable implementation when benchmark evidence did not support the change. |

Queryable history for technical review is available in `benchmarks/benchmark_history.db`, with the regenerating SQL dump in `benchmarks/benchmark_history.sql`.

## Reproducibility

Build and test:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"

cmake --build build-release
ctest --test-dir build-release --output-on-failure -C Release
```

Run the scripted EC2 benchmark sweep:

```bash
PIN_CPU=0 \
CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native" \
benchmarks/run_ec2_benchmarks.sh
```

Run focused categories:

```bash
BENCHMARK_TARGETS=core_hot_path benchmarks/run_ec2_benchmarks.sh
BENCHMARK_TARGETS=realistic_flow benchmarks/run_ec2_benchmarks.sh
BENCHMARK_TARGETS=stress benchmarks/run_ec2_benchmarks.sh
BENCHMARK_TARGETS=replay benchmarks/run_ec2_benchmarks.sh
BENCHMARK_TARGETS=batch_latency benchmarks/run_ec2_benchmarks.sh
```

Artifact expectations:

| Artifact | Purpose |
| --- | --- |
| `benchmarks/results/benchmark_environment.txt` | Environment, commit, compiler, flags, and CPU metadata |
| `benchmarks/results/core_hot_path_results.{txt,json}` | Core hot-path throughput |
| `benchmarks/results/realistic_flow_results.{txt,json}` | Realistic direct and end-to-end throughput |
| `benchmarks/results/stress_benchmark_results.{txt,json}` | Stress workload throughput |
| `benchmarks/results/determinism_replay_results.{txt,json}` | Replay throughput |
| `benchmarks/results/batch_latency_results.{txt,json}` | Amortized fixed-batch latency |
| `benchmarks/benchmark_history.{db,sql}` | Queryable benchmark history and a plain SQL recreation path |

EC2 transfer hygiene:

| Rule | Reason |
| --- | --- |
| Exclude `.git`, local build directories, `.DS_Store`, and macOS `._*` AppleDouble files from remote source transfers. | Sidecar files can be discovered as bogus replay fixtures and invalidate CTest setup. |
| Run benchmarks only on native Linux/EC2 for final numbers. | Local macOS and Docker checks are useful for development, but release benchmark claims should come from the Linux host. |
| Keep raw Google Benchmark output out of this document. | Curated reporting should show comparable metrics, with raw artifacts retained under `benchmarks/results/`. |
