#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
OUTPUT_DIR="${OUTPUT_DIR:-"$ROOT_DIR/benchmarks"}"
PIN_CPU="${PIN_CPU:-0}"
LATENCY_SAMPLES="${LATENCY_SAMPLES:-1024}"
LATENCY_WARMUP_BATCHES="${LATENCY_WARMUP_BATCHES:-128}"
LATENCY_TRIALS="${LATENCY_TRIALS:-5}"
PERF_TEXT_FILE="$OUTPUT_DIR/perf_results.txt"
PERF_CSV_FILE="$OUTPUT_DIR/perf_results.csv"
BENCHMARK_STDOUT_FILE="$(mktemp)"

PERF_EVENTS=(
    cycles
    instructions
    branches
    branch-misses
    cache-references
    cache-misses
    L1-dcache-loads
    L1-dcache-load-misses
    LLC-loads
    LLC-load-misses
)

cleanup() {
    # Remove temporary benchmark stdout once both artifacts have been written.
    rm -f "$BENCHMARK_STDOUT_FILE"
}
trap cleanup EXIT

install_ubuntu_package() {
    local package="$1"

    # Keep dependency installation best-effort so unsupported hosts fail clearly.
    if command -v apt-get >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y "$package"
    fi
}

ensure_perf() {
    # perf ships through linux-tools on Ubuntu EC2 images, not through CMake.
    if command -v perf >/dev/null 2>&1; then
        return
    fi

    install_ubuntu_package "linux-tools-$(uname -r)" || true
    install_ubuntu_package linux-tools-common || true
    install_ubuntu_package linux-tools-generic || true

    if ! command -v perf >/dev/null 2>&1; then
        echo "error: perf is unavailable; install linux-tools for this Ubuntu kernel" >&2
        exit 1
    fi
}

ensure_taskset() {
    # CPU pinning keeps the aggregate counters tied to one scheduler context.
    if command -v taskset >/dev/null 2>&1; then
        return
    fi

    install_ubuntu_package util-linux || true

    if ! command -v taskset >/dev/null 2>&1; then
        echo "error: taskset is unavailable; install util-linux before running EC2 perf counters" >&2
        exit 1
    fi
}

find_batch_latency_benchmark() {
    local configured="${BENCHMARK_BIN:-}"
    local candidates=(
        "$configured"
        "$BUILD_DIR/core_hot_path_latency_benchmark"
        "$ROOT_DIR/build-release/core_hot_path_latency_benchmark"
        "$ROOT_DIR/build/core_hot_path_latency_benchmark"
        "$ROOT_DIR/build/benchmarks/core_hot_path_latency_benchmark"
        "$BUILD_DIR/latency_benchmark"
        "$ROOT_DIR/build-release/latency_benchmark"
    )

    # Accept the first executable path so the script works with local build-dir choices.
    for candidate in "${candidates[@]}"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done

    echo "error: core_hot_path_latency_benchmark was not found; run benchmarks/run_ec2_benchmarks.sh or set BENCHMARK_BIN" >&2
    exit 1
}

check_event_support() {
    local event="$1"
    local output

    # Probe each event individually because cloud PMUs may expose only a subset.
    if output="$(perf stat -x, -e "$event" -- true 2>&1 >/dev/null)"; then
        if grep -qiE "not supported|not counted|cannot find|unable to find|bad event|event syntax error|parser error|invalid event" <<<"$output"; then
            return 1
        fi
        return 0
    fi

    if grep -qiE "permission|operation not permitted|perf_event_paranoid|access to performance monitoring" <<<"$output"; then
        echo "error: perf cannot access hardware counters on this host" >&2
        echo "$output" >&2
        echo "hint: lower kernel.perf_event_paranoid or run with sufficient privileges on the EC2 host" >&2
        exit 1
    fi

    return 1
}

join_by_comma() {
    local IFS=,
    echo "$*"
}

write_text_artifact() {
    local benchmark_bin="$1"
    local event_list="$2"
    shift 2
    local unsupported_events=("$@")

    {
        echo "Hardware performance counter results"
        echo "date=$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
        echo "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)"
        if [[ -z "$(git -C "$ROOT_DIR" status --porcelain)" ]]; then
            echo "git_status=clean"
        else
            echo "git_status=dirty"
            git -C "$ROOT_DIR" status --short
        fi
        echo "pin_cpu=$PIN_CPU"
        echo "latency_samples=$LATENCY_SAMPLES"
        echo "latency_warmup_batches=$LATENCY_WARMUP_BATCHES"
        echo "latency_trials=$LATENCY_TRIALS"
        echo "benchmark_binary=$benchmark_bin"
        echo "perf_events=$event_list"
        echo
        echo "command:"
        printf 'perf stat -x, -o %q -e %q -- taskset -c %q %q --output-dir=%q --samples=%q --warmup=%q --trials=%q\n' \
            "$PERF_CSV_FILE" "$event_list" "$PIN_CPU" "$benchmark_bin" "$OUTPUT_DIR" \
            "$LATENCY_SAMPLES" "$LATENCY_WARMUP_BATCHES" "$LATENCY_TRIALS"
        echo

        if ((${#unsupported_events[@]})); then
            echo "unsupported_counters:"
            printf -- "- %s\n" "${unsupported_events[@]}"
            echo
        fi

        echo "notes:"
        echo "- Aggregate counters explain why latency moves; they do not replace latency percentiles."
        echo "- Cache counters expose memory hierarchy pressure in deep cancel workloads."
        echo "- Branch counters expose unpredictable random lookup behavior."
        echo "- Cycles and instructions help separate memory-bound work from compute-bound work."
        echo
        echo "batch_latency_benchmark_stdout:"
        sed 's/^/  /' "$BENCHMARK_STDOUT_FILE"
        echo
        echo "perf_counter_summary:"
        if [[ -s "$PERF_CSV_FILE" ]]; then
            awk -F, '
                BEGIN {
                    printf "%-28s %20s %12s %s\n", "event", "count", "unit", "metric"
                    printf "%-28s %20s %12s %s\n", "-----", "-----", "----", "------"
                }
                /^[[:space:]]*#/ || NF < 3 {
                    next
                }
                /^status,event,/ {
                    next
                }
                /^unsupported,/ {
                    printf "%-28s %20s %12s %s\n", $2, "N/A", "N/A", "unsupported"
                    next
                }
                {
                    value = $1
                    unit = $2
                    event = $3
                    metric = ""
                    if (NF >= 7 && $6 != "") {
                        metric = $6 " " $7
                    }
                    printf "%-28s %20s %12s %s\n", event, value, unit, metric
                }
            ' "$PERF_CSV_FILE"
        else
            echo "No requested hardware counters were reported by perf."
        fi
    } > "$PERF_TEXT_FILE"
}

write_unsupported_csv() {
    local unsupported_events=("$@")

    {
        echo "status,event,count,unit,metric"
        for event in "${unsupported_events[@]}"; do
            echo "unsupported,$event,N/A,N/A,PMU event unavailable on this host"
        done
    } > "$PERF_CSV_FILE"
}

main() {
    mkdir -p "$OUTPUT_DIR"
    ensure_perf
    ensure_taskset

    local benchmark_bin
    benchmark_bin="$(find_batch_latency_benchmark)"

    local supported_events=()
    local unsupported_events=()
    for event in "${PERF_EVENTS[@]}"; do
        if check_event_support "$event"; then
            supported_events+=("$event")
        else
            unsupported_events+=("$event")
        fi
    done

    if ((${#supported_events[@]} == 0)); then
        # Still leave artifacts behind so EC2 PMU limitations are reproducible.
        : > "$BENCHMARK_STDOUT_FILE"
        write_unsupported_csv "${unsupported_events[@]}"
        write_text_artifact "$benchmark_bin" "none" "${unsupported_events[@]}"
        echo "warning: no requested perf counters are supported on this host" >&2
        echo "Wrote $PERF_TEXT_FILE"
        echo "Wrote $PERF_CSV_FILE"
        return 0
    fi

    local event_list
    event_list="$(join_by_comma "${supported_events[@]}")"

    # Run perf stat once and derive both artifacts from the same benchmark pass.
    perf stat \
        -x, \
        -o "$PERF_CSV_FILE" \
        -e "$event_list" \
        -- \
        taskset -c "$PIN_CPU" "$benchmark_bin" \
        --output-dir="$OUTPUT_DIR" \
        --samples="$LATENCY_SAMPLES" \
        --warmup="$LATENCY_WARMUP_BATCHES" \
        --trials="$LATENCY_TRIALS" \
        > "$BENCHMARK_STDOUT_FILE"

    write_text_artifact "$benchmark_bin" "$event_list" "${unsupported_events[@]}"

    echo "Wrote $PERF_TEXT_FILE"
    echo "Wrote $PERF_CSV_FILE"
}

main "$@"
