# Matching Engine

[![CI](https://github.com/eric-zhou-tz/low-latency-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/eric-zhou-tz/low-latency-matching-engine/actions/workflows/ci.yml)

A small modern C++20 matching engine scaffold built with CMake.

The project is intentionally small: it wires commands from `stdin` through a
parser and exchange into an order book, then prints events. Limit orders match
against resting liquidity using price-time priority.

## Structure

```text
include/   Public headers and domain interfaces
src/       Implementations and CLI entry point
tests/     GoogleTest unit tests
benchmark/ Google Benchmark microbenchmarks
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


## Benchmarks

Benchmark sources live under `benchmark/`:

- `insert_benchmark.cpp` measures passive BUY/SELL limit orders that do not
  cross and therefore rest on the book.
- `match_benchmark.cpp` preloads resting liquidity, then measures aggressive
  crossing limit orders that consume that liquidity.
- `cancel_benchmark.cpp` measures front, back, random, and unknown-order
  cancels, plus a mixed submit/cancel/match stream.

The benchmarks exercise the order-book hot path directly. They avoid parser,
stdin, file I/O, and logging overhead, and they do not include fabricated
performance numbers.

Configure a Release build to fetch Google Benchmark and build the benchmark
executables. This builds the benchmarks only; it does not run them:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native"
cmake --build build
```

Run benchmarks manually on the Linux EC2 benchmark host and save both readable
and structured output. Create the output directory first:

```sh
mkdir -p benchmarks

./build/insert_benchmark \
  --benchmark_out=benchmarks/insert_results.json \
  --benchmark_out_format=json \
  > benchmarks/insert_results.txt

./build/match_benchmark \
  --benchmark_out=benchmarks/match_results.json \
  --benchmark_out_format=json \
  > benchmarks/match_results.txt

./build/cancel_benchmark \
  --benchmark_out=benchmarks/cancel_results.json \
  --benchmark_out_format=json \
  > benchmarks/cancel_results.txt
```

The `.txt` files contain human-readable benchmark summaries. The `.json` files
preserve machine-readable results for future tooling, CI integration, and
regression tracking.

The cancel benchmarks matter because exchange workloads constantly remove live
orders before they trade. Front, back, and randomized cancels show how FIFO
queue position affects scalability, while unknown cancels isolate rejected
lookup behavior. The mixed workload keeps cancel measurements grounded in a
more realistic stream of resting inserts, live cancels, and crossing orders.

See [BENCHMARKS.md](BENCHMARKS.md) for the current Linux VM Google Benchmark
baseline summary.

Example EC2 execution command:

```sh
ssh -i ~/.ssh/matching-engine-key.pem ubuntu@<EC2_IP> \
'cd ~/matching-engine && \
mkdir -p benchmarks && \
./build/insert_benchmark \
--benchmark_out=benchmarks/insert_results.json \
--benchmark_out_format=json \
> benchmarks/insert_results.txt'
```

For the matching benchmark:

```sh
ssh -i ~/.ssh/matching-engine-key.pem ubuntu@<EC2_IP> \
'cd ~/matching-engine && \
mkdir -p benchmarks && \
./build/match_benchmark \
--benchmark_out=benchmarks/match_results.json \
--benchmark_out_format=json \
> benchmarks/match_results.txt'
```

Use Ubuntu/Linux and Release mode for final measurements. The recommended
release flags are `-O3 -march=native` when the target machine supports them.
Do not run benchmarks automatically from CMake or CI until that workflow is
added intentionally.

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
