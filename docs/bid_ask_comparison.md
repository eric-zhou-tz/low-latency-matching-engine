# Bid/Ask Price-Level Comparison

This file tracks storage-comparison benchmark notes for bid/ask price-level
access.

## 2026-05-20 std::map vs absl::btree_map

Run metadata:

- Date: `2026-05-20T08:00:24Z`
- Commit: `9a032e3b57155eb6f16343384f6cd8d06fa74682` plus local
  `absl::btree_map` TreeLevels changes
- Baseline: `std::map` helper-baseline rows from the 2026-05-20
  Price-Level Helper Baseline section below
- Environment: AWS EC2 `t3.small`
- OS/kernel: Ubuntu on Linux `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL, 2 vCPUs, 2.50 GHz
- Build type: Release
- Flags: `-O3 -DNDEBUG`
- Compiler: `c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- Remote run directory: `/home/ubuntu/matching-engine-runs/absl-btree-20260520`
- Correctness: `141/141` tests passed before benchmark execution
- Commands: `BENCHMARK_TARGETS=deep_sparse_gtc_mixed,best_level_churn,level_create_delete_churn,true_mixed THROUGHPUT_REPETITIONS=5 PIN_CPU=0 ./benchmarks/run_ec2_benchmarks.sh`
- Transfer note: source archive excluded `.git`, local build dirs, `.DS_Store`,
  and macOS `._*` sidecar files; the remote preflight found no `._*` files.

Higher throughput is better. Values are median Google Benchmark rows from five
pinned repetitions.

| Workload | Input size | std::map throughput | absl::btree_map throughput | Throughput delta | absl::btree_map CPU |
|---|---:|---:|---:|---:|---:|
| True mixed | 1,000 | 18.589M/s | 17.085M/s | -8.09% | 58,530 ns |
| True mixed | 10,000 | 16.116M/s | 15.465M/s | -4.04% | 646,634 ns |
| True mixed | 100,000 | 14.563M/s | 14.168M/s | -2.71% | 7,058,203 ns |
| Deep sparse GTC mixed | 1,000 / 50,000 levels | 1.346M/s | 2.644M/s | +96.43% | 378,217 ns |
| Deep sparse GTC mixed | 10,000 / 50,000 levels | 1.675M/s | 2.591M/s | +54.66% | 3,860,222 ns |
| Deep sparse GTC mixed | 100,000 / 50,000 levels | 1.627M/s | 2.498M/s | +53.55% | 40,029,087 ns |
| Best-level churn | 10,000 | 18.010M/s | 17.597M/s | -2.29% | 568,277 ns |
| Best-level churn | 100,000 | 17.426M/s | 17.105M/s | -1.84% | 5,846,408 ns |
| Best-level churn | 1,000,000 | 14.458M/s | 14.025M/s | -2.99% | 71,300,966 ns |
| Level create/delete churn | 10,000 | 19.884M/s | 16.086M/s | -19.10% | 621,669 ns |
| Level create/delete churn | 100,000 | 19.515M/s | 15.719M/s | -19.45% | 6,361,817 ns |
| Level create/delete churn | 1,000,000 | 19.478M/s | 15.575M/s | -20.04% | 64,205,251 ns |

Interpretation:

- `absl::btree_map` is a clear win for the deep sparse workload, where compact
  ordered storage appears to help the 50,000-level book.
- True mixed and best-level churn regressed slightly, so the B-tree backend is
  not a universal improvement for shallow or best-touch-heavy books.
- Level create/delete churn regressed by about 19-20%. The 1,000,000-operation
  run was noisy (`cv=21.85%` for the B-tree median group), but every median in
  that workload still trailed the `std::map` helper baseline.
- Abseil `btree_map` generally follows STL sorted-container APIs, but it is a
  different implementation with different iterator/reference stability
  contracts than `std::map`. The current `OrderBook` helper layer does not keep
  TreeLevels iterators, references, or `OrderQueue*` values across TreeLevels
  insert/erase operations.

## 2026-05-20 Vector Price Ladder V1

Run metadata:

- Date: `2026-05-20T07:45:15Z`
- Commit: `9a032e3b57155eb6f16343384f6cd8d06fa74682` plus local ladder changes
- Environment: AWS EC2 `t3.small`
- OS/kernel: Ubuntu on Linux `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL, 2 vCPUs, 2.50 GHz
- Build type: Release
- Flags: `-O3 -DNDEBUG`
- Compiler: `c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- Remote run directory: `/home/ubuntu/matching-engine-runs/price-ladder-20260520-074457`
- Correctness: `141/141` tests passed before benchmark execution
- Commands: `BENCHMARK_TARGETS=shallow_gtc_mixed,best_level_churn,level_create_delete_churn,true_mixed THROUGHPUT_REPETITIONS=5 PIN_CPU=0 ./benchmarks/run_ec2_benchmarks.sh`
- Transfer note: source archive excluded `.git`, local build dirs, `.DS_Store`,
  and macOS `._*` sidecar files.

Higher throughput is better. CPU deltas compare median CPU time; positive means
the ladder variant was slower.

| Workload | Input size | Tree throughput | Ladder throughput | Throughput delta | Tree CPU | Ladder CPU | CPU delta |
|---|---:|---:|---:|---:|---:|---:|---:|
| True mixed | 1,000 | 16.823M/s | 8.749M/s | -47.99% | 59,442 ns | 114,302 ns | +92.29% |
| True mixed | 10,000 | 15.138M/s | 9.127M/s | -39.71% | 660,596 ns | 1,095,602 ns | +65.85% |
| True mixed | 100,000 | 13.797M/s | 8.735M/s | -36.69% | 7,247,980 ns | 11,448,337 ns | +57.95% |
| Shallow GTC mixed | 1,000 / 1,850 actions | 18.168M/s | 12.993M/s | -28.48% | 101,829 ns | 142,379 ns | +39.82% |
| Shallow GTC mixed | 10,000 / 18,500 actions | 17.204M/s | 12.554M/s | -27.03% | 1,075,334 ns | 1,473,630 ns | +37.04% |
| Shallow GTC mixed | 100,000 / 185,000 actions | 16.638M/s | 12.484M/s | -24.97% | 11,119,320 ns | 14,819,380 ns | +33.28% |
| Best-level churn | 10,000 | 17.403M/s | 3.396M/s | -80.49% | 574,616 ns | 2,944,652 ns | +412.43% |
| Best-level churn | 100,000 | 17.069M/s | 8.725M/s | -48.88% | 5,858,495 ns | 11,460,871 ns | +95.63% |
| Best-level churn | 1,000,000 | 14.493M/s | 10.234M/s | -29.39% | 68,997,163 ns | 97,717,928 ns | +41.63% |
| Level create/delete churn | 10,000 | 19.121M/s | 595.881k/s | -96.88% | 522,977 ns | 16,781,886 ns | +3109.87% |
| Level create/delete churn | 100,000 | 18.442M/s | 595.510k/s | -96.77% | 5,422,472 ns | 167,923,222 ns | +2996.80% |
| Level create/delete churn | 1,000,000 | 18.406M/s | 596.528k/s | -96.76% | 54,331,139 ns | 1,676,367,527 ns | +2985.47% |

Interpretation:

- The first real ladder implementation is correct and deterministic, but it is
  not yet a performance win on these workloads.
- The shallow and true-mixed books still favor `std::map` because v1 ladder best
  lookup and emptiness checks scan vector slots instead of maintaining best
  indexes or active-level state.
- Level create/delete churn is the clearest warning: sparse workloads are very
  expensive with scan-based `best_level` and `levels_empty`, even when the ladder
  window is deliberately bounded to the generated price envelope.
- Out-of-range limit prices are intentionally rejected in this v1 design. There
  is no hybrid overflow tree, dynamic recentering, or historical base-price
  inference in this patch.
- Hybrid ladder plus overflow, active-index tracking, or a different sparse
  structure remain future work before ladder storage should be considered a
  production default.

## 2026-05-20 Price-Level Helper Baseline

Run metadata:

- Environment: AWS EC2 `t3.small`
- OS/kernel: Ubuntu on Linux `7.0.0-1004-aws`
- CPU: Intel Xeon Platinum 8259CL, 2 vCPUs, 2.50 GHz
- Build type: Release
- Flags: `-O3 -DNDEBUG`
- Compiler: `c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- Remote run directory: `/home/ubuntu/matching-engine-runs/price-level-helper-20260520-065524`
- Correctness: `134/134` tests passed before benchmark execution
- Transfer note: source archive excluded `._*`, `.DS_Store`, local build dirs,
  and previous benchmark result artifacts.

Interpretation:

- The helper abstraction preserved behavior but added measurable direct
  `OrderBook` hot-path overhead.
- The slowdown is acceptable for the storage experiment phase because it gives a
  stable semantic surface for comparing RB tree, B-tree, and price ladder
  implementations.
- After selecting the storage design, the winning path can be flattened or
  specialized again to recover direct-access performance.
- End-to-end mixed throughput changed only slightly, so the regression is mostly
  visible in direct book microbenchmarks and amortized hot-path latency.

## Median Throughput Comparison

Higher throughput is better. `CPU delta` is the change in median CPU time, where
positive means slower.

| Benchmark | Previous | Helper abstraction | Throughput delta | CPU delta |
|---|---:|---:|---:|---:|
| `BM_RestingLimitOrderInsert/1000` | 28.840M/s | 23.732M/s | -17.71% | +21.53% |
| `BM_RestingLimitOrderInsert/10000` | 29.502M/s | 24.213M/s | -17.93% | +21.84% |
| `BM_RestingLimitOrderInsert/100000` | 11.495M/s | 10.006M/s | -12.95% | +14.88% |
| `BM_CrossingLimitOrderMatch/1000` | 28.642M/s | 25.627M/s | -10.53% | +11.77% |
| `BM_CrossingLimitOrderMatch/10000` | 28.909M/s | 25.675M/s | -11.19% | +12.60% |
| `BM_CrossingLimitOrderMatch/100000` | 17.139M/s | 9.935M/s | -42.03% | +72.51% |
| `BM_OrderBookTrueMixed/1000` | 21.363M/s | 18.589M/s | -12.98% | +14.92% |
| `BM_OrderBookTrueMixed/10000` | 17.667M/s | 16.116M/s | -8.78% | +9.63% |
| `BM_OrderBookTrueMixed/100000` | 15.851M/s | 14.563M/s | -8.13% | +8.84% |
| `BM_ShallowGtcMixed/1000` | 20.505M/s | 18.190M/s | -11.29% | +12.72% |
| `BM_ShallowGtcMixed/10000` | 19.531M/s | 17.472M/s | -10.54% | +11.79% |
| `BM_ShallowGtcMixed/100000` | 18.800M/s | 16.771M/s | -10.79% | +12.09% |
| `BM_DeepSparseGtcMixed/1000` | 1.509M/s | 1.346M/s | -10.82% | +12.13% |
| `BM_DeepSparseGtcMixed/10000` | 1.864M/s | 1.675M/s | -10.15% | +11.30% |
| `BM_DeepSparseGtcMixed/100000` | 1.850M/s | 1.627M/s | -12.04% | +13.69% |
| `BM_BestLevelChurn/10000` | 20.593M/s | 18.010M/s | -12.54% | +14.34% |
| `BM_BestLevelChurn/100000` | 19.629M/s | 17.426M/s | -11.22% | +12.64% |
| `BM_BestLevelChurn/1000000` | 15.100M/s | 14.458M/s | -4.25% | +4.44% |
| `BM_LevelCreateDeleteChurn/10000` | 24.423M/s | 19.884M/s | -18.58% | +22.83% |
| `BM_LevelCreateDeleteChurn/100000` | 23.720M/s | 19.515M/s | -17.73% | +21.54% |
| `BM_LevelCreateDeleteChurn/1000000` | 23.628M/s | 19.478M/s | -17.56% | +21.30% |

## End-to-End Throughput

End-to-end mixed throughput changed much less than direct `OrderBook`
microbenchmarks.

| Benchmark | Previous | Helper abstraction | Throughput delta |
|---|---:|---:|---:|
| `BM_EndToEnd_MixedOrderFlow_Throughput/1000` | 1.374M/s | 1.349M/s | -1.81% |
| `BM_EndToEnd_MixedOrderFlow_Throughput/10000` | 1.376M/s | 1.372M/s | -0.29% |
| `BM_EndToEnd_MixedOrderFlow_Throughput/100000` | 1.343M/s | 1.312M/s | -2.33% |

## Latency Notes

Median latency across trials also shows direct hot-path overhead:

- `OrderBookTrueMixed` p50 worsened by about `+10%`, `+16%`, and `+32%` for
  batch sizes `64`, `256`, and `1024`.
- `MixedSubmitCancel` p50 worsened by about `+12%`, `+22%`, and `+24%` for batch
  sizes `64`, `256`, and `1024`.
- `CrossingLimitOrderMatch` p50 worsened by about `+33%`, `+15%`, and `+16%` for
  batch sizes `64`, `256`, and `1024`.
- Some cancel latency results were mixed by batch size, so treat those as noisy
  until the storage variants are compared under the same runner.

## Next Comparison Targets

Use the same semantic price-level API to compare:

1. Current RB-tree-backed `std::map` helper baseline.
2. B-tree-backed price levels.
3. Vector or ladder-backed price levels.

After the storage comparison is complete, consider flattening or specializing
the winning path so matching code can recover the direct hot-path shape without
losing the benchmark evidence gathered during the experiment.
