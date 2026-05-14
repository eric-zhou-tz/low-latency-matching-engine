# Matching Engine

A small modern C++20 matching engine scaffold built with CMake.

The project is intentionally small: it wires commands from `stdin` through a
parser and exchange into an order book, then prints events. Limit orders match
against resting liquidity using price-time priority.

## Structure

```text
include/   Public headers and domain interfaces
src/       Implementations and CLI entry point
tests/     GoogleTest unit tests
examples/  Example order command streams
```

## Build

GoogleTest is fetched by CMake with `FetchContent`; it does not need to be
installed globally.

```sh
cmake -S . -B build
cmake --build build
```

## Test

```sh
ctest --test-dir build
```

For more detail on failures:

```sh
ctest --test-dir build --output-on-failure
```

## Linux Testing With Docker

Build and test inside an Ubuntu Linux container:

```sh
docker build --target build -t matching-engine-test .
```

Open an interactive Linux development shell:

```sh
docker build --target dev -t matching-engine-dev .
docker run --rm -it -v "$PWD":/workspace -w /workspace matching-engine-dev
```

From inside the container:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

Build a small runtime image and run the demo:

```sh
docker build --target runtime -t matching-engine .
docker run --rm -i matching-engine < examples/demo.orders
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
- `OrderBook` owns order storage and matching behavior.
- Events are returned as values so the core stays testable and independent of IO.

## Next Steps

- Introduce a global order-id index for faster cancellation.
- Add market orders and time-in-force handling.
- Broaden unit coverage around parser and exchange behavior.
