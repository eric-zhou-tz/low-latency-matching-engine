#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
OUTPUT_DIR="${OUTPUT_DIR:-"$ROOT_DIR/benchmarks/results"}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
RELEASE_FLAGS="${CMAKE_CXX_FLAGS_RELEASE:--O3 -DNDEBUG}"
THROUGHPUT_REPETITIONS="${THROUGHPUT_REPETITIONS:-5}"
LATENCY_TRIALS="${LATENCY_TRIALS:-5}"
LATENCY_SAMPLES="${LATENCY_SAMPLES:-1024}"
LATENCY_WARMUP_BATCHES="${LATENCY_WARMUP_BATCHES:-128}"
PIN_CPU="${PIN_CPU:-0}"
BENCHMARK_TARGETS="${BENCHMARK_TARGETS:-all}"
STRESS_BENCHMARK_FILTER="${STRESS_BENCHMARK_FILTER:-^BM_Stress_}"

mkdir -p "$OUTPUT_DIR"

if find "$ROOT_DIR" -name '._*' -print -quit | grep -q .; then
    echo "error: macOS resource-fork sidecar files found under $ROOT_DIR" >&2
    echo "do not transfer AppleDouble '._*' files to EC2; re-sync sources with those files excluded" >&2
    exit 1
fi

if command -v taskset >/dev/null 2>&1; then
    PIN_CMD=(taskset -c "$PIN_CPU")
else
    PIN_CMD=()
    echo "warning: taskset is unavailable; benchmark commands will run without CPU pinning" >&2
fi

get_ec2_instance_type() {
    # Query IMDSv2 first, then fall back to IMDSv1; both paths are best-effort.
    if ! command -v curl >/dev/null 2>&1; then
        echo "N/A"
        return
    fi

    local token
    token="$(curl -fsS --max-time 1 -X PUT \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" \
        "http://169.254.169.254/latest/api/token" 2>/dev/null || true)"

    if [[ -n "$token" ]]; then
        curl -fsS --max-time 1 \
            -H "X-aws-ec2-metadata-token: $token" \
            "http://169.254.169.254/latest/meta-data/instance-type" 2>/dev/null || echo "N/A"
    else
        curl -fsS --max-time 1 \
            "http://169.254.169.254/latest/meta-data/instance-type" 2>/dev/null || echo "N/A"
    fi
}

run_pinned() {
    # Apply CPU pinning when taskset is present; otherwise run the command directly.
    if ((${#PIN_CMD[@]})); then
        "${PIN_CMD[@]}" "$@"
    else
        "$@"
    fi
}

should_run() {
    # Allow focused EC2 passes without deleting the full benchmark workflow.
    local target="$1"
    [[ "$BENCHMARK_TARGETS" == "all" || ",$BENCHMARK_TARGETS," == *",$target,"* ]]
}

write_metadata() {
    # Capture environment details next to benchmark artifacts for reproducibility.
    local metadata_file="$OUTPUT_DIR/benchmark_environment.txt"
    {
        echo "date=$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
        if git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
            echo "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)"
            if [[ -z "$(git -C "$ROOT_DIR" status --porcelain)" ]]; then
                echo "git_status=clean"
            else
                echo "git_status=dirty"
                git -C "$ROOT_DIR" status --short
            fi
        else
            echo "git_commit=${SOURCE_COMMIT:-N/A}"
            echo "git_status=${SOURCE_STATUS:-unavailable; source transferred without .git metadata}"
        fi
        echo "ec2_instance_type=$(get_ec2_instance_type)"
        echo "uname=$(uname -a)"
        echo "build_type=$BUILD_TYPE"
        echo "release_flags=$RELEASE_FLAGS"
        echo "benchmark_targets=$BENCHMARK_TARGETS"
        echo "cmake_version=$(cmake --version | head -n 1)"
        echo "compiler_version=$(${CXX:-c++} --version | head -n 1)"
        echo
        echo "lscpu:"
        if command -v lscpu >/dev/null 2>&1; then
            lscpu
        else
            echo "N/A"
        fi
    } > "$metadata_file"
    echo "Wrote $metadata_file"
}

write_metadata

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_FLAGS_RELEASE="$RELEASE_FLAGS"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE"

ctest --test-dir "$BUILD_DIR" --output-on-failure -C "$BUILD_TYPE"

if should_run core_hot_path; then
    run_pinned "$BUILD_DIR/core_hot_path_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/core_hot_path_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/core_hot_path_results.txt"
fi

if should_run realistic_flow; then
    run_pinned "$BUILD_DIR/realistic_flow_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/realistic_flow_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/realistic_flow_results.txt"
fi

if should_run std_toy_comparison; then
    run_pinned "$BUILD_DIR/std_toy_comparison_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/std_toy_comparison_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/std_toy_comparison_results.txt"
fi

if should_run stress; then
    run_pinned "$BUILD_DIR/stress_benchmark" \
        --benchmark_filter="$STRESS_BENCHMARK_FILTER" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/stress_benchmark_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/stress_benchmark_results.txt"
fi

if should_run replay; then
    run_pinned "$BUILD_DIR/determinism_replay_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/determinism_replay_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/determinism_replay_results.txt"
fi

if [[ "$BENCHMARK_TARGETS" != "all" ]] && should_run reserve_sweep; then
    run_pinned "$BUILD_DIR/experimental_reserve_sweep_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/experimental_reserve_sweep_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/experimental_reserve_sweep_results.txt"
fi

if should_run batch_latency; then
    run_pinned "$BUILD_DIR/core_hot_path_latency_benchmark" \
        --output-dir="$OUTPUT_DIR" \
        --samples="$LATENCY_SAMPLES" \
        --warmup="$LATENCY_WARMUP_BATCHES" \
        --trials="$LATENCY_TRIALS"
fi
