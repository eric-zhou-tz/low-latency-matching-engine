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
- FIFO queues at each price level
- Deterministic integer-based pricing and quantities
- Cancel order support
- Exchange-level symbol routing
- GoogleTest unit testing
- Google Benchmark performance benchmarking
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

Additional documentation:

- [Architecture](docs/ARCHITECTURE.md)
- [Benchmarks](BENCHMARKS.md)
- [Benchmark History](docs/benchmark_history.md)
- [Hot Path Analysis](docs/HOTPATH.md)
- [Changelog](docs/CHANGELOG.md)

## Repository Structure

```text
include/   Public headers and domain interfaces
src/       Core engine implementation
tests/     GoogleTest unit tests
benchmark/ Google Benchmark microbenchmarks
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

## Supported Commands

```text
SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity>
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
- FIFO cancel-order performance
- Mixed submit/cancel/match workloads

Example Release build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native"

cmake --build build
```

Detailed benchmark results and methodology are available in
[BENCHMARKS.md](BENCHMARKS.md).

## Linux Development

The repository includes Docker workflows for Linux validation and reproducible
development environments.

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

- Investigate why random cancels are significantly slower than other cancel paths.
- Implement market order support.
- Formalize the print book action and output contract.
- Implement trade reports.
- Perform hot path analysis across matching, canceling, and symbol routing.
- Add latency benchmarks.
