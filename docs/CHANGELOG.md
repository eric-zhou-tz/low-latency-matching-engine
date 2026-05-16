# Changelog

All notable changes to this project will be documented in this file.

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
