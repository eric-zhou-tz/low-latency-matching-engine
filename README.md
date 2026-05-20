# Matching Engine

[![CI](https://github.com/eric-zhou-tz/low-latency-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/eric-zhou-tz/low-latency-matching-engine/actions/workflows/ci.yml)

A low-latency exchange-style matching engine written in modern C++20.

This project focuses on deterministic order matching, price-time priority, and
performance-oriented systems design. The engine routes parsed exchange commands
through symbol-level order books and executes trades against resting liquidity
using FIFO matching semantics.

Designed as a systems engineering project for learning exchange architecture,
order-book design, and low-latency infrastructure patterns used in modern
electronic trading systems.

## Features

- Price-time priority matching
- Immediate-or-cancel (IOC) and fill-or-kill (FOK) limit orders
- Market order support
- FIFO queues at each price level
- Deterministic integer-based pricing and quantities
- Cancel order support
- Exchange-level symbol routing
- Structured hot-path events with presentation formatting at the boundary
- GoogleTest unit testing
- Google Benchmark performance benchmarking
- End-to-end CLI-style benchmark coverage
- Linux benchmark validation on EC2
- Dockerized Linux development workflow
- Modern CMake-based build system

## Architecture

The engine follows a simplified exchange-style event flow:

```text
stdin/orders
    |
Parser
    |
Exchange
    |
Symbol OrderBook
    |
Matching Engine
    |
Trade / Accept / Reject Events
```

Text parsing is used for CLI/testing convenience and is intentionally separated
from the matching core. Most hot-path benchmarks measure the typed matching core
directly; the dedicated end-to-end benchmarks intentionally include parser,
exchange, order-book, and event-formatting overhead.

The CLI defaults to the optimized engine. For comparison and regression checks,
it can also route the same parsed commands through a deliberately simple STL
baseline:

```bash
./build/matching_engine --model=fast < examples/demo.orders
./build/matching_engine --model=toy-std < examples/demo.orders
```

`toy-std` lives under `toy/` and exists only as a reference baseline. It uses
plain `std::map`, `std::deque`, and `std::unordered_map` internals and should
not be treated as a performance implementation.

`Exchange` handles multi-symbol simulation and routing, while `OrderBook` is the
latency-sensitive matching core. Production systems often route using integer
symbol IDs, symbol partitioning, or both.

Internally, the matching core emits structured domain events rather than
preformatted strings. Submissions write into caller-owned `std::vector<Event>`
buffers because one order can produce multiple fills, while cancellation returns
a single `CancelResult` and does not need an event buffer.

Additional documentation:

- [Architecture](docs/ARCHITECTURE.md)
- [Benchmarks](Benchmarks.md)
- [Benchmark History](docs/benchmark_history.md)
- [Hot Path Analysis](docs/HOTPATH.md)
- [Changelog](docs/CHANGELOG.md)

## Repository Structure

```text
include/   Public headers
  core/    Action, event, and order value types
  book/    Order book, order pool, and price-level queue types
  io/      Text command parser interface
src/       Engine implementation
  book/    Order book matching and cancel logic
  io/      Parser implementation
tests/     GoogleTest unit tests
toy/       Simple std-container baseline engine for comparison/regression
benchmarks/ Benchmark sources, EC2 runners, and historical result artifacts
examples/  Example order streams
docs/      Architecture, changelog, benchmark history, and hot-path notes
```

## Quick Start

Clone the repository:

```bash
git clone https://github.com/eric-zhou-tz/low-latency-matching-engine.git
cd low-latency-matching-engine
```

Build the project:

```bash
cmake -S . -B build
cmake --build build
```

Run the demo order stream:

```bash
./build/matching_engine < examples/demo.orders
```

Run the demo through each engine model:

```bash
# Fast optimized engine; this is also the default when --model is omitted.
./build/matching_engine --model=fast < examples/demo.orders

# Toy std baseline for comparison/regression/reference only.
./build/matching_engine --model=toy-std < examples/demo.orders
```

## Supported Commands

```text
SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity> [GTC|IOC|FOK]
MARKET <id> <symbol> <BUY|SELL> <quantity>
CANCEL <id>
PRINT
```

## Testing

Run unit tests:

```bash
ctest --test-dir build --output-on-failure
```

## Benchmarking

Benchmarks are implemented using Google Benchmark and executed on a dedicated
Ubuntu EC2 benchmark host using Release-mode builds.

Current benchmark coverage includes:

- Resting limit-order insertion
- Aggressive crossing-order matching
- Random and rejected cancel-order performance
- True mixed OrderBook workloads with GTC, cancel, modify, IOC, market, and FOK flow
- Amortized batch latency for matching-engine hot paths
- End-to-end CLI-style parse/process/format throughput

Latest EC2 Release hot-path highlights:

- 100,000-order insert: `11.49M items/s`
- 100,000-order crossing match: `17.14M items/s`
- 100,000 random cancel: `8.483M items/s`
- 100,000 unknown cancel: `115.7M items/s`
- 100,000 mixed submit/cancel/match: `18.24M items/s`

Latest EC2 Release end-to-end CLI-style highlights:

- 100,000 command parse/process/format: `1.047M commands/s`
- 100,000 command mixed order flow: `1.343M commands/s`

End-to-end results include parser, exchange, OrderBook, and event-formatting
overhead. They should not be compared directly to OrderBook hot-path
microbenchmarks.

Example Release build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"

cmake --build build
```

Detailed benchmark results and methodology are available in
[Benchmarks.md](Benchmarks.md).

## Linux Development

The repository includes Docker workflows for Linux validation and reproducible
development environments. Docker is used for reproducible Linux builds and
validation, not as the source of published latency measurements; benchmark
numbers come from native Release builds on Linux/EC2.

Build Linux test container:

```bash
docker build --target build -t matching-engine-test .
```

Run interactive Linux development environment:

```bash
docker build --target dev -t matching-engine-dev .
docker run --rm -it -v "$PWD":/workspace -w /workspace matching-engine-dev
```

## Engineering Goals

This project emphasizes:

- Deterministic systems behavior
- Exchange-style order-book architecture
- Low-latency engineering principles
- Testability and reproducibility
- Clean modern C++ design
- Linux-based benchmarking workflows

## Future Steps

- Evaluate an event sink/callback API for submissions if event-vector allocation remains visible in future profiles.
- Formalize the print book action and output contract for non-demo integrations.
- Add richer trade-report output options at the public boundary.
- Expand benchmark analysis across parser, symbol routing, formatting, and matching hot paths.
