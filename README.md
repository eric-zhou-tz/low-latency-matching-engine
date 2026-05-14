# Matching Engine

A small modern C++20 matching engine scaffold built with CMake.

The project is intentionally minimal: it wires commands from `stdin` through a
parser and exchange into an order book, then prints events. Full matching logic
is left as TODO work inside the order book.

## Structure

```text
include/   Public headers and domain interfaces
src/       Implementations and CLI entry point
tests/     Dependency-free smoke tests
examples/  Example order command streams
```

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Run

```sh
./build/matching_engine < examples/demo.orders
```

Supported commands:

```text
SUBMIT <id> <symbol> <BUY|SELL> <price> <quantity>
CANCEL <id>
PRINT
```

## Architectural Intent

- `Parser` owns text-to-action conversion.
- `Exchange` owns command routing and exchange-level state.
- `OrderBook` owns order storage and, later, matching behavior.
- Events are returned as values so the core stays testable and independent of IO.

## Next Steps

- Add side-specific price levels.
- Implement price-time priority matching.
- Introduce a global order-id index for faster cancellation.
- Replace smoke tests with a richer test framework when behavior grows.
