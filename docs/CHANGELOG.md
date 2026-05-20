# Changelog

All notable changes to this project will be documented in this file.

## v0.8.1 -> Queryable Benchmark History

- Added a recruiter-friendly SQLite benchmark history database at `benchmarks/benchmark_history.db` while keeping `BENCHMARKS.md` and `docs/benchmark_history.md` as the primary Markdown views.
- Added `benchmarks/schema.sql` and `benchmarks/benchmark_history.sql` so technical reviewers can inspect or recreate the benchmark database without relying on the binary artifact.
- Added `benchmarks/README.md` with simple `sqlite3` usage instructions and example queries for latest runs, throughput leaders, and latency rows.
- Populated the database from the existing benchmark history, current benchmark summary, changelog context, and checked-in benchmark result artifacts, preserving missing values as `NULL` instead of inventing precision.
- Linked the queryable benchmark artifacts from the benchmark summary and historical benchmark log.

## v0.8.0 -> Refreshed Full Benchmark Suite

- Refreshed the full EC2/Linux benchmark suite on the pinned `t3.small` host with a Release `-O3 -DNDEBUG -march=native` build and 127/127 CTest cases passing before benchmark execution.
- Reworked `BENCHMARKS.md` into a recruiter-facing current-results report covering core hot path, realistic flow, stress, and determinism/replay workloads.
- Added latest benchmark artifacts for core hot path, realistic flow, stress, replay, and batch latency runs under `benchmarks/results/`.
- Moved benchmark output defaults from `benchmarks/` to `benchmarks/results/` in the EC2 benchmark runner, perf-counter runner, and standalone batch-latency runner.
- Documented transfer hygiene for EC2 runs so `.git`, local build directories, `.DS_Store`, and macOS `._*` sidecar files stay out of remote benchmark source trees.

## v0.7.2 -> Added Path for Toy std Baseline

- Added the `toy/` path for the std baseline implementation.

## v0.7.1 -> Reversion to Stable Patch

- Reverted the working tree to stable commit `e6a5f77357e083a0449f829c73a99cbbc6fa5318` after the comparative price-level storage experiment showed direct hot-path regressions.
- Removed the experimental RB tree/B-tree/price-ladder comparison state from the active code path by returning to the pre-experiment stable matching implementation.
- Revalidated the reverted tree on the Ubuntu EC2 `t3.small` benchmark host with a clean Release build using `-O3 -DNDEBUG`.
- Synced sources to EC2 without `.git`, local build directories, `.DS_Store`, or macOS `._*` sidecar files; the remote run confirmed `0` sidecar files before benchmarking.
- Verified correctness with the full EC2 CTest suite: 124/124 tests passed.
- Ran the full scripted EC2 benchmark pass, including throughput and standalone latency validation.
- Confirmed benchmark medians returned close to the prior direct-hot-path baseline: True Mixed measured 15.71M items/s at 100,000 operations, Best-Level Churn measured 15.93M items/s at 1,000,000 operations, and Level Create/Delete Churn measured 23.55M items/s at 1,000,000 operations.
- Noted some EC2 run-to-run noise in large mixed/deep workloads, so medians should be compared rather than individual repetitions.
- Remote validation run: `/home/ubuntu/matching-engine-runs/revert-e6a5f77-20260520-081602`.

## v0.7.0 -> Comparative Benchmark suite (RB Tree vs B Tree vs Price Ladder)
- Comparative benchmark suite for RB Tree, B+ Tree, and Price Ladder matching engine architectures.
- Added throughput and latency benchmarking across shallow/deep book workloads
- Initial results inconclusive and under further investigation.

## v0.6.6 - Best Level Churn + Level create/delete churn

- Added `best_level_churn_benchmark` for direct `OrderBook` top-of-book churn: best-level cancels, one-tick inside-improving submits, marketable taker flow, and occasional modifies.
- Added `level_create_delete_churn_benchmark` for repeated creation and deletion of whole price levels across both bid and ask sides.
- Kept both workloads on the `OrderBook` hot path only with fixed RNG seeds, caller-owned reusable `std::vector<Event>` buffers, no parser/exchange/filesystem/string formatting in the timed loop, and `reserve_order_capacity` based on modeled live depth.
- Updated the EC2 runner with focused `BENCHMARK_TARGETS` support, sidecar-file fail-fast checks for macOS `._*` files, and result artifacts for `benchmarks/*churn_results.{txt,json}`.
- Refreshed EC2 Release medians: best-level churn measured 15.100M items/s at 1,000,000 operations; level create/delete churn measured 23.628M items/s at 1,000,000 operations.
- Synced EC2 source without `.git`, build directories, `.DS_Store`, or macOS `._*` sidecar files; 124/124 correctness tests passed before each focused benchmark run.
- Documented methodology and EC2 medians in `BENCHMARKS.md` and `docs/benchmark_history.md`.

## v0.6.5 - Shallow Dense + Deep Sparse GTC Mixed Benchmark

- Added `shallow_gtc_mixed_benchmark` for shallow dense GTC churn: tight prices, low live depth, random cancels/modifies, and crossing GTC submits.
- Added `deep_sparse_gtc_mixed_benchmark` for 50,000 occupied price levels with only one to two resting GTC orders per level, stressing `std::map` price-level behavior.
- Kept both benchmarks OrderBook-only with fixed seeds, reusable `std::vector<Event>` buffers, no parser/exchange/filesystem/string formatting in the timed loop, and `reserve_order_capacity` based on modeled live depth.
- Wired the benchmark targets into CMake and the EC2 runner, with result artifacts under `benchmarks/*gtc_mixed_results.*`.
- Refreshed EC2 Release medians: shallow dense measured 18.800M items/s at 100,000 operations; deep sparse measured 1.850M items/s at 100,000 operations.
- Synced EC2 source with `.git`, build directories, `.DS_Store`, and macOS `._*` sidecar files excluded to avoid bogus replay fixtures.
- Documented methodology and EC2 medians in `BENCHMARKS.md` and `docs/benchmark_history.md`.

## v0.6.4 - Stress/Soak Correctness Tests

- Added `order_book_stress_tests` with three million-operation correctness stress scenarios: mixed order flow, price-level churn, and modify/cancel churn.
- Added periodic structural health checks during stress runs to validate uncrossed books, order-id index consistency, price-level aggregate volumes, empty-level cleanup, and resting order metadata.
- Made long-run stress streams deterministic while allowing legitimate full book drains to replenish passive liquidity instead of failing the harness.
- Wired stress tests into CMake/CTest as a dedicated GoogleTest executable.
- Validated the stress suite on the Ubuntu EC2 `t3.small` host in Release mode with `-O3 -DNDEBUG`; all 3/3 stress tests passed across 3,000,000 total operations.
- Added stress/soak validation artifacts and documentation in `BENCHMARKS.md`, `docs/benchmark_history.md`, `benchmarks/stress_results.txt`, and `benchmarks/stress_environment.txt`.

## v0.6.3 - End to End OrderBook True Mixed Benchmark

- Added an end-to-end True Mixed Google Benchmark covering the full pipeline: parser → exchange → order book → event formatting.
- Interleaved GTC submits, cancels, modifies, IOC orders, market orders, and FOK orders in a deterministic mixed stream.
- Kept filesystem I/O outside the timed loop while measuring full public-boundary execution overhead.
- Added amortized batch latency coverage for 64, 256, and 1,024-operation batches.
- Used fixed RNG seeds, reusable std::vector<Event> buffers, and the current 10% reserve_order_capacity heuristic.

## v0.6.2 - OrderBook True Mixed Benchmark

- Added an OrderBook-only True Mixed Google Benchmark throughput case with randomly interleaved GTC submits, cancels, modifies, IOC limit orders, market orders, and FOK limit orders.
- Kept the True Mixed workload on the hot path only: no parser, exchange, filesystem I/O, or string/event formatting inside the measured loop.
- Added deterministic workload generation with fixed RNG seeds, caller-owned reusable `std::vector<Event>` buffers, and `reserve_order_capacity` sized with the current 10% live-order heuristic.
- Added matching amortized batch latency coverage for the same True Mixed stream at 64, 256, and 1,024-operation batches, explicitly reported as amortized batch latency rather than true single-operation latency.
- Wired `true_mixed_benchmark` into CMake and the EC2 benchmark runner, producing `benchmarks/true_mixed_results.txt` and `benchmarks/true_mixed_results.json`.
- Refreshed EC2 Release benchmark artifacts on `t3.small`; 121/121 correctness tests passed before benchmark execution, and the 100,000-operation True Mixed throughput case measured 15.85M items/s.


## v0.6.1 - Preallocation Based on Max Live Orders

- Renamed benchmark and order-book preallocation identifiers from `expected_order_capacity` to `reserve_order_capacity` to make the capacity value explicit as a tuning hint.
- Changed the mixed submit/cancel benchmark reserve policy to `max(1024, operation_count / 10)` after EC2 reserve sweeps showed reserving the full operation count hurt throughput through cache/locality footprint effects.
- Documented peak live orders versus total operations in the architecture notes and clarified why reserve sizing is decoupled from operation count.
- Refreshed the full EC2 Release benchmark suite on `t3.small`; 121/121 correctness tests passed before benchmark execution and the 100,000-operation mixed submit/cancel benchmark measured 18.241M items/s by median.

## v0.6.0 - End-to-End Benchmarks

- Added Google Benchmark coverage for full public-boundary CLI-style overhead: in-memory input lines, parser, exchange routing, order-book mutation, and event formatting.
- Added `BM_EndToEnd_ParseProcessFormat` for deterministic multi-symbol parse/process/format throughput without filesystem I/O inside the timed loop.
- Added `BM_EndToEnd_ReplayScenario` for replay-style multi-symbol command streams covering inserts, crosses, cancels, modifies, market orders, IOC, and FOK.
- Wired the new `end_to_end_benchmark` target into CMake and the EC2 benchmark runner, producing `benchmarks/end_to_end_results.txt` and `benchmarks/end_to_end_results.json`.
- Refreshed EC2 Release benchmark artifacts on `t3.small` with GCC 15.2.0 and `-O3 -DNDEBUG`; 121/121 correctness tests passed before benchmark execution.
- Documented the new results in `BENCHMARKS.md` and `docs/benchmark_history.md`, with an explicit note that end-to-end parser/exchange/formatter overhead should not be compared directly to OrderBook hot-path microbenchmarks.

## v0.5.2 - Invariants + Regression

- Added deterministic order-book invariant tests that validate randomized operation streams preserve uncrossed books, order-id index consistency, FIFO uniqueness, price-level volume totals, empty-level cleanup, best-price ordering, and live quantity accounting.
- Added focused regression tests under `tests/regression/` for FOK rejection atomicity, IOC and market non-resting remainders, partial-fill cancel cleanup, empty price-level removal, FIFO matching at one price, best-price traversal, and multi-symbol exchange isolation.
- Wired `order_book_invariant_tests` and `order_book_regression_tests` into CMake and CTest as dedicated GoogleTest executables.
- Verified the full local CTest suite passes with 121 GoogleTest cases, including 8 focused regression cases.

## v0.5.1 - Golden Replay Tests

- Added a golden replay test suite that runs fixture command tapes through the public CLI pipeline: parser, exchange, order book, and event formatter.
- Added `tests/replay/` fixtures with paired `.txt` input tapes and `.expected` output tapes for submit/print, crossing trades, partial fills, cancels, duplicate ids, modify, market, IOC, FOK, multi-symbol isolation, deterministic replay, and malformed input behavior.
- Added a replay README documenting the exact-output public-boundary regression test contract.
- Wired `golden_replay_tests` into CMake and CTest with exact output comparison after normalizing only line endings.
- Verified the full local CTest suite passes with 111 GoogleTest cases, including 16 replay fixtures.

## v0.5.0 - Unit Tests

- Audited the parser, order-book, and exchange test suites for semantic coverage gaps across current engine behavior.
- Expanded order-book GoogleTest coverage for duplicate-order rejection details, partial-fill cancelability, exact FOK fills, exact market fills without remainder rejection, and fully filled replacement-modify outcomes.
- Expanded exchange integration coverage for duplicate order ids across symbols and market orders, cross-symbol matching isolation, partial versus full fill index cleanup, IOC exact fills, replacement modify event ordering, and `PRINT` snapshot correctness across empty and multi-symbol books.
- Expanded parser coverage for `PRINT`, malformed market/cancel/modify/print commands, extra trailing tokens, invalid market sides, and zero-quantity modify rejection.
- Tightened parser validation so `MARKET`, `CANCEL`, and `PRINT` now reject trailing tokens consistently with `SUBMIT` and `MODIFY`.
- Removed the legacy assert-based `tests/order_book_tests.cpp` smoke test file because its coverage overlapped heavily with the maintained GoogleTest suite and it was not wired into CMake.
- Verified the full local CTest suite passes with 79 GoogleTest cases.

## v0.4.2 - Modify Order Action

- Added native `ModifyOrderAction` support with `MODIFY <order_id> <new_price> <new_quantity>` parsing and exchange routing by the live order-id index.
- Added `ModifiedEvent` for same-price quantity reductions that preserve FIFO priority, and `ReplacedEvent` for cancel-replace modifications that lose priority.
- Implemented order-book modify semantics for resting GTC orders: reduced quantity mutates in place, while price changes and quantity increases remove the old resting order and execute the replacement through the shared matching/resting path.
- Refactored limit-order execution into reusable internal helpers so submit and modify share one matching and resting implementation without routing modify through public submit/cancel APIs.
- Kept IOC, FOK, market, fully filled, and unknown orders out of modify/cancel routing by maintaining the exchange-level `order_to_book_` index after replacement fills and non-resting outcomes.
- Expanded parser, order-book, and exchange GoogleTest coverage for FIFO preservation/loss, unknown and non-resting modify rejection, replacement matching, exchange index consistency, fully executed replacements, invalid modify input, and atomic modify event streams without cancel/accept pairs.

## v0.4.1 - FOK + IOC Orders

- Added fill-or-kill limit order support through the existing `SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity> [GTC|IOC|FOK]` command.
- Added `TimeInForce::FillOrKill` and parser support for the `FOK` flag while preserving existing GTC default and IOC behavior.
- Moved limit-order acceptance emission out of `prepare_incoming_order()` so FOK orders can reject before acceptance when full execution is unavailable.
- Added an order-book FOK preflight that scans crossing opposite-side price levels using aggregate level volume before mutating book state.
- Ensured rejected FOK orders do not partially fill, do not rest, and do not enter the exchange cancel index.
- Expanded parser, order-book, and exchange unit coverage for FOK parsing, full multi-level fills, insufficient-liquidity rejection, and cancel-index behavior.

## v0.4.0 - Market Orders

- Added first-class market order support through the new `MARKET <id> <symbol> <BUY|SELL> <quantity>` command.
- Added `MarketOrderAction` and routed market submissions through `Exchange` into a market-specific order-book path.
- Implemented market buy and sell matching that sweeps available opposite-side liquidity in price-time priority order.
- Ensured market orders never rest on the book; any unfilled remainder expires after available liquidity is consumed.
- Added rejection behavior for market orders that cannot fully fill because of insufficient opposite-side liquidity.
- Expanded parser and order-book unit coverage for market command parsing, full fills, multi-level sweeps, partial fills, and empty-book expiration.

## v0.3.9 - Perf Counter Instrumentation + EC2 Validation

- Added lightweight EC2 `perf stat` instrumentation for the amortized batch latency benchmark workflow.
- Added `benchmarks/run_perf_counters.sh` to pin `latency_benchmark` with `taskset -c 0`, probe requested PMU counters, and write both text and CSV perf artifacts.
- Requested aggregate counters for cycles, instructions, branches, branch misses, cache references/misses, L1 data-cache loads/misses, and LLC loads/misses.
- Refreshed EC2 Release benchmark artifacts on `t3.small`; 26/26 correctness tests passed before benchmark execution.
- Confirmed the latest throughput results are broadly stable versus the previous EC2 run, with small deltas treated as normal run-to-run variation rather than a code regression.
- Captured that this EC2 KVM host did not expose the requested hardware PMU events to `perf`, so `perf_results.txt` and `perf_results.csv` record the unsupported counter set instead of reporting misleading values.
- Updated `BENCHMARKS.md` and `docs/benchmark_history.md` with the new latency results, perf-counter methodology, and unsupported-counter probe outcome.

## v0.3.8 - Latency Benchmarks
- Added a standalone amortized batch latency benchmarking suite alongside the existing Google Benchmark throughput benchmarks.
- Added fixed-size batch latency measurements (64, 256, and 1,024 operations) with p50, p95, p99, and max percentile reporting across five EC2 trials.
- Added dedicated latency workloads for resting inserts, crossing matches, front/back/random cancels, unknown cancels, and mixed submit/cancel/match streams.
-  Implemented low-overhead latency methodology using timed operation batches instead of per-operation timers to avoid misleading single-op measurements.
- Added EC2 benchmark automation with taskset-based CPU pinning, environment metadata capture, and JSON/text latency artifacts.
- Refreshed Linux EC2 Release benchmarks; deepest mixed submit/cancel/match throughput improved from 14.6258M to 18.9449M items/s, while deepest crossing match throughput improved from 19.9854M to 20.5566M items/s.
- Latest latency results showed ~28 ns p50 front/back cancels, ~10 ns p50 unknown cancels, and ~61 ns p50 mixed submit/cancel latency at 64-operation batches on the EC2 benchmark host.

## v0.3.7 - Reusable Submit Event Buffers

- Changed submit processing to write into caller-owned reusable `std::vector<Event>` buffers instead of returning event vectors by value.
- Preserved `OrderBook::cancel` as a direct `CancelResult` path because cancellation produces exactly one result and does not need an event buffer.
- Updated CLI, exchange routing, tests, and benchmarks for the split API: submit/match reuse event buffers, cancel returns one result.
- Refreshed EC2 Release benchmarks; 100,000-order insert improved from 13.6655M to 16.3102M items/s, crossing match improved from 9.71048M to 19.9854M items/s, and mixed submit/cancel/match improved from 11.0886M to 14.6258M items/s.
- Confirmed cancel performance stayed in the direct-result range, with 100,000 random cancel at 11.1673M items/s and unknown cancel at 107.774M items/s.

## v0.3.6 - Structured Events + Cancel Result Optimization

- Replaced hot-path accepted and rejected event strings with structured payloads: `AcceptedEvent` now stores the order id, and `RejectedEvent` stores `RejectReason` plus order id.
- Added `BookSnapshotEvent` so snapshot output can remain presentation-oriented without forcing `AcceptedEvent` to carry arbitrary strings.
- Changed `OrderBook::cancel` to return a single `CancelResult` variant instead of allocating and returning a one-event `std::vector<Event>`.
- Kept `Exchange::process` returning event vectors for the public command boundary while wrapping cancel results only at that outer layer.
- Refreshed EC2 Release benchmarks after the event optimization; 100,000-order insert improved from 8.37928M to 13.6655M items/s, crossing match improved from 5.68622M to 9.71048M items/s, random cancel improved from 5.71876M to 11.2259M items/s, and unknown cancel improved from 11.0453M to 104.52M items/s.

## v0.3.5 - Symbol-Free Resting Orders + Struct Padding Optimizations

- Removed `symbol` from the hot-path `Order` stored in `OrderBook`; symbol routing now lives in `SubmitOrderAction` and `Exchange`.
- Reduced Linux `Order` size from 80 bytes to 48 bytes while keeping one book per symbol.
- Updated parser, exchange routing, snapshots, tests, and benchmarks for the split between transient submit commands and resting book orders.
- Refreshed EC2 Release benchmarks after the layout change; 100,000-order insert improved from 6.69874M to 8.37928M items/s, random cancel improved from 5.21629M to 5.71876M items/s, and mixed submit/cancel/match improved to 8.03819M items/s.
- Noted the benchmark tradeoff: unknown cancel regressed from 12.8694M to 11.0453M items/s, so the result is a meaningful locality improvement but not an across-the-board win.

## v0.3.4 - Dense Order Lookup

- Replaced the live order-id index with `ankerl::unordered_dense::map` to reduce node chasing on cancel-heavy workloads.
- Reserved expected order lookup capacity during benchmark setup so known-depth runs avoid rehashing during preload or timed submit loops.
- Improved EC2 random cancel throughput in the paired 100,000-order comparison from 2.71735M items/s to 5.23192M items/s.
- Documented the flat hash-map tradeoff: better cache locality and lookup throughput in exchange for weaker iterator/reference stability than node-based maps.

## v0.3.3 - Hot Path Analysis and Docs Reorganization

- Added perf-based hot path analysis for `BM_CancelRandom/100000`.
- Captured FlameGraph notes showing random cancel is now dominated by hash lookup, event vector work, and cache locality rather than FIFO queue removal.
- Added `HOTPATH.md` to document perf commands, FlameGraph interpretation, and follow-up optimization ideas.
- Moved architecture, changelog, benchmark history, and hot-path notes into `docs/`.
- Updated `README.md` to link the documentation set while keeping `README.md` and `BENCHMARKS.md` at the repository root.

## v0.3.2 - OrderPool Storage Arena

- Added `OrderPool` to own stable resting-order storage in contiguous blocks.
- Reused canceled and filled order slots through an internal free list to avoid hot-path deallocation.
- Moved order ownership out of price levels so `OrderQueue` and `orders_by_id_` store raw non-owning `Order*` pointers.
- Refreshed EC2 Release benchmarks after the intrusive queue and order pool refactor.

## v0.3.1 - Intrusive OrderQueue Cancel Path

- Replaced node-based FIFO queues with an intrusive `OrderQueue` that stores `head`, `tail`, and `total_volume`.
- Embedded `prev` and `next` links directly in `Order` for O(1) cancel unlink by order ID.
- Updated `orders_by_id_` to store direct `Order*` cancel locations.
- Preserved strict FIFO price-time priority for matching and cancellation.

## v0.3.0 - Cancel Order Support

- Added a first-class `CanceledEvent` for successful cancel operations.
- Maintained the order-id index as a live resting-order index keyed by order id with side and price location data.
- Implemented deterministic cancel behavior that removes the order from its FIFO price-level queue, clears empty price levels, and rejects unknown, filled, or already-canceled orders.
- Expanded unit coverage for buy and sell cancels, unknown cancels, FIFO preservation after cancel, price-level cleanup, partial-fill cancel behavior, and fully filled orders.

## v0.2.4 - Linux VM Google Benchmark Baseline

- Added baseline Google Benchmark throughput artifacts from the Ubuntu EC2 benchmarking host.
- Recorded resting limit order insert and crossing limit order match throughput.
- Added `BENCHMARKS.md` to summarize environment details, benchmark workloads, results, artifacts, and initial performance read.
- Documented saving both human-readable benchmark logs and machine-readable JSON outputs for future regression tracking.

## v0.2.3 - GitHub Actions CI

- Added GitHub Actions CI for Ubuntu CMake/Ninja builds with GCC.
- Added sanitizer CI coverage with Clang, AddressSanitizer, and UndefinedBehaviorSanitizer.
- Registered GoogleTest-based tests with CTest discovery.
- Added a README badge for the primary CI workflow.

## v0.2.2 - Docker Linux Test Environment

- Added a multi-stage Dockerfile for Ubuntu-based build, development, and runtime images.
- Added a Docker build target that configures, builds, and runs the CTest suite on Linux.
- Added an interactive Docker development shell for testing the project in a Linux environment.
- Added a lightweight runtime image for running the matching engine CLI with sample order input.
- Documented Docker build, test, development shell, and runtime commands in the README.

## v0.2.1 - GoogleTest Test Infrastructure

- Added GoogleTest via CMake `FetchContent` so tests do not require a global install.
- Built production code as an `engine` library target for reuse by test binaries.
- Added `tests/order_book_test.cpp` with unit coverage for order resting, duplicate IDs, aggressive matching, FIFO price-time priority, partial fills, and cancellation behavior.
- Registered GoogleTest cases with CTest using `gtest_discover_tests`.
- Updated README build and test instructions.

## v0.2.0 - Limit Order Matching

- Added limit order matching with price-time priority.
- Added bid and ask price levels backed by FIFO order queues.
- Emitted `TradeEvent` records for executed matches.
- Rested partially filled incoming limit orders with remaining quantity.
- Removed fully filled resting orders from the book and cancel index.
- Expanded order book smoke tests for matching, partial fills, and FIFO behavior.

## v0.1.0 - Basic Order Model + CLI Parser

- Added the initial C++20/CMake project scaffold.
- Defined basic action, order, event, parser, order book, and exchange types.
- Added a stdin-driven CLI loop: `stdin -> parser -> exchange -> events`.
- Added basic command parsing for `SUBMIT`, `CANCEL`, and `PRINT`.
- Added minimal smoke tests and example order input.
