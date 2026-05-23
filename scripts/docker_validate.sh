#!/usr/bin/env bash

set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-matching-engine-test}"
DOCKER_TARGET="${DOCKER_TARGET:-validation}"

log() {
    # Prefix each phase so CI logs make the failing validation step obvious.
    printf '\n==> %s\n' "$*"
}

run_container() {
    # Run commands from the image workdir so build paths and replay fixtures resolve normally.
    docker run --rm "$IMAGE_NAME" "$@"
}

run_cli_flow() {
    local label="$1"
    local input_text="$2"
    local expected_text="$3"
    local output

    log "Validating CLI flow: $label"
    # Use a pseudo-terminal so scripted input exercises the interactive menu, not replay stdin.
    output="$(
        printf '%s' "$input_text" \
            | docker run --rm -i "$IMAGE_NAME" script -q -e -c './build/matching_engine' /dev/null
    )"
    if [[ "$output" != *"$expected_text"* ]]; then
        printf 'Expected CLI output to contain:\n%s\n\nActual output:\n%s\n' \
            "$expected_text" "$output" >&2
        return 1
    fi
}

main() {
    # Build once, then reuse the same image for every validation command.
    log "Building Docker validation image: $IMAGE_NAME"
    docker build --target "$DOCKER_TARGET" -t "$IMAGE_NAME" .

    log "Running full CTest suite"
    run_container ctest --test-dir build --output-on-failure -C Release

    log "Running focused parser, replay, and CLI test binaries"
    run_container ./build/parser_tests
    run_container ./build/golden_replay_tests
    run_container ./build/cli_presentation_tests

    log "Validating direct demo replay through the optimized engine"
    run_container /bin/bash -lc \
        './build/matching_engine --model=fast < examples/demo.orders | grep -F "CANCELED order_id=1" >/dev/null'

    log "Validating direct demo replay through the std baseline"
    run_container /bin/bash -lc \
        './build/matching_engine --model=toy-std < examples/demo.orders | grep -F "CANCELED order_id=1" >/dev/null'

    run_cli_flow "help mode" $'6\n\n7\n' "Supported commands:"
    run_cli_flow "manual command mode" \
        $'2\nSUBMIT 1 AAPL BUY 100 10 GTC\nPRINT\nEXIT\n7\n' \
        "ACCEPTED order_id=1"
    run_cli_flow "interactive guided demo mode" $'1\n\nQ\n7\n' "Interactive guided demo"
    run_cli_flow "benchmark comparison mode" $'3\n7\n7\n' \
        "Benchmark comparison: optimized engine vs std baseline"
    run_cli_flow "replay file mode" $'5\n\n\n7\n' "Replay complete. Executed 5 commands."

    log "Running lightweight benchmark executable sanity checks"
    run_container ./build/core_hot_path_benchmark \
        --benchmark_filter='^BM_OrderBook_PassiveInsert_Throughput/1000$' \
        --benchmark_min_time=0.001s \
        --benchmark_repetitions=1
    run_container ./build/realistic_flow_benchmark \
        --benchmark_filter='^BM_OrderBook_TrueMixed_Throughput/1000$' \
        --benchmark_min_time=0.001s \
        --benchmark_repetitions=1
    run_container ./build/stress_benchmark \
        --benchmark_filter='^BM_Stress_ShallowGtcMixed_Throughput/1000$' \
        --benchmark_min_time=0.001s \
        --benchmark_repetitions=1
    run_container ./build/determinism_replay_benchmark \
        --benchmark_filter='^BM_Replay_GoldenFixtures_Throughput$' \
        --benchmark_min_time=0.001s \
        --benchmark_repetitions=1
    run_container ./build/experimental_reserve_sweep_benchmark --benchmark_list_tests=true
    run_container ./build/core_hot_path_latency_benchmark \
        --output-dir=/tmp/docker-benchmark-sanity \
        --samples=2 \
        --warmup=1 \
        --trials=1

    log "Docker validation completed successfully"
}

main "$@"
