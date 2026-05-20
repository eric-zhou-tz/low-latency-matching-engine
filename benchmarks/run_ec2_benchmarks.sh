#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
OUTPUT_DIR="${OUTPUT_DIR:-"$ROOT_DIR/benchmarks"}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
RELEASE_FLAGS="${CMAKE_CXX_FLAGS_RELEASE:--O3 -DNDEBUG}"
THROUGHPUT_REPETITIONS="${THROUGHPUT_REPETITIONS:-5}"
LATENCY_TRIALS="${LATENCY_TRIALS:-5}"
LATENCY_SAMPLES="${LATENCY_SAMPLES:-1024}"
LATENCY_WARMUP_BATCHES="${LATENCY_WARMUP_BATCHES:-128}"
PIN_CPU="${PIN_CPU:-0}"
BENCHMARK_TARGETS="${BENCHMARK_TARGETS:-all}"
BENCHMARK_FILTER="${BENCHMARK_FILTER:-^BM_BestLevelChurn}"

mkdir -p "$OUTPUT_DIR"

if find "$ROOT_DIR" -name '._*' -print -quit | grep -q .; then
    echo "error: macOS resource-fork sidecar files found under $ROOT_DIR" >&2
    echo "exclude '._*' files before transferring sources to EC2, then rerun this script" >&2
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
            echo "git_commit=N/A"
            echo "git_status=unavailable; source transferred without .git metadata"
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

if should_run insert; then
    run_pinned "$BUILD_DIR/insert_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/insert_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/insert_results.txt"
fi

if should_run match; then
    run_pinned "$BUILD_DIR/match_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/match_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/match_results.txt"
fi

if should_run cancel; then
    run_pinned "$BUILD_DIR/cancel_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/cancel_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/cancel_results.txt"
fi

if should_run true_mixed; then
    run_pinned "$BUILD_DIR/true_mixed_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/true_mixed_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/true_mixed_results.txt"
fi

if should_run shallow_gtc_mixed; then
    run_pinned "$BUILD_DIR/shallow_gtc_mixed_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/shallow_gtc_mixed_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/shallow_gtc_mixed_results.txt"
fi

if should_run deep_sparse_gtc_mixed; then
    run_pinned "$BUILD_DIR/deep_sparse_gtc_mixed_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/deep_sparse_gtc_mixed_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/deep_sparse_gtc_mixed_results.txt"
fi

if should_run best_level_churn; then
    run_pinned "$BUILD_DIR/best_level_churn_benchmark" \
        --benchmark_filter="$BENCHMARK_FILTER" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/best_level_churn_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/best_level_churn_results.txt"
fi

if should_run level_create_delete_churn; then
    run_pinned "$BUILD_DIR/level_create_delete_churn_benchmark" \
        --benchmark_filter="^BM_LevelCreateDeleteChurn" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/level_create_delete_churn_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/level_create_delete_churn_results.txt"
fi

if should_run end_to_end; then
    run_pinned "$BUILD_DIR/end_to_end_benchmark" \
        --benchmark_repetitions="$THROUGHPUT_REPETITIONS" \
        --benchmark_out="$OUTPUT_DIR/end_to_end_results.json" \
        --benchmark_out_format=json \
        | tee "$OUTPUT_DIR/end_to_end_results.txt"
fi

if should_run latency; then
    run_pinned "$BUILD_DIR/latency_benchmark" \
        --output-dir="$OUTPUT_DIR" \
        --samples="$LATENCY_SAMPLES" \
        --warmup="$LATENCY_WARMUP_BATCHES" \
        --trials="$LATENCY_TRIALS"
fi
