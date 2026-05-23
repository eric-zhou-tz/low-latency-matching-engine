# Contributing

Thanks for helping improve the matching engine. The project favors small,
reviewable changes that preserve deterministic matching behavior and keep
benchmark claims reproducible.

## Development Setup

Use Docker when you want the closest path to CI-style validation:

```bash
docker build --target validation -t matching-engine-test .
./scripts/docker_validate.sh
```

For a native build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

CMake fetches GoogleTest, Google Benchmark, and `unordered_dense` during
configure.

## CI Expectations

Pull requests should pass:

- GitHub Actions `CI`, which builds and runs tests on Ubuntu.
- GitHub Actions `Sanitizers`, which runs AddressSanitizer and
  UndefinedBehaviorSanitizer with Clang.
- `./scripts/docker_validate.sh` for a Release container smoke pass when a
  change touches CLI flows, replay fixtures, benchmarks, or Docker setup.

Run targeted test binaries locally while iterating, then run the full CTest
suite before handing off a change.

## Formatting And Comments

- Match the surrounding C++ style and keep changes narrowly scoped.
- Prefer clear names over clever abbreviations.
- Use Doxygen comments for headers and short practical comments inside function
  bodies when they explain a non-obvious step, data-structure update, or matching
  invariant.
- Avoid comments that only repeat a single obvious line of code.

## Sanitizers

The sanitizer workflow uses:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer failures should be treated as correctness bugs, even when normal tests
appear to pass.

## Replay Tests

Golden replay fixtures live in `tests/replay/`.

When adding or changing replay behavior:

- Add a `.txt` command tape and matching `.expected` output tape.
- Keep fixture names descriptive and stable.
- Cover both the success path and the relevant rejection or edge case.
- Run `golden_replay_tests` or the full CTest suite before submitting.

The editable CLI replay script is `tests/replay_cli.txt`; use it for public demo
flows and CLI replay mode checks.

## Adding Benchmarks

Benchmark sources are grouped by intent:

- `benchmarks/core_hot_path/` for direct `OrderBook` microbenchmarks.
- `benchmarks/realistic_flow/` for mixed order-flow and end-to-end workloads.
- `benchmarks/stress/` for churn, sparse-book, and pressure workloads.
- `benchmarks/determinism_replay/` for replay-throughput checks.

When registering a new Google Benchmark:

- Add the source to the matching executable in `CMakeLists.txt`.
- Use `benchmark::DoNotOptimize(...)` for benchmarked values.
- Use `benchmark::ClobberMemory()` when writes must remain visible.
- Keep setup outside the timed loop with `PauseTiming()` and `ResumeTiming()`
  when setup is not the measured work.
- Report item counts and counters consistently with nearby benchmarks.
- Add or update tests when the workload depends on new matching behavior.

Release benchmark numbers should come from Ubuntu Linux/EC2 with CPU pinning.
Local and Docker runs are useful for sanity checks, not headline results.

## Benchmark History

If `BENCHMARKS.md` changes meaningfully, update
`benchmarks/benchmark_history.db` and regenerate
`benchmarks/benchmark_history.sql` in the same change. Preserve unknown values as
`NULL`, include commit and environment provenance, and keep old rows unless they
are clearly incorrect or duplicated.
