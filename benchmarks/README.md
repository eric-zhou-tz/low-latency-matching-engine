# Benchmark History Database

The Markdown benchmark reports remain the primary reviewer-facing view:

- `../BENCHMARKS.md` summarizes the latest polished results.
- `../docs/benchmark_history.md` keeps the web-readable historical log.

This directory also includes a small SQLite copy for technical reviewers who want to query benchmark history directly.

The current benchmark documentation uses the native EC2 `c7i-flex.large`
full-suite run on `2026-05-31T21:41:35Z` at source commit `75d9181e` plus
local single-order latency benchmark and documentation updates. Earlier rows
were run on EC2 `t3.small` unless their environment says otherwise; keep those
rows as historical hardware context rather than relabeling them.

## Files

| File | Purpose |
| --- | --- |
| `benchmark_history.db` | SQLite database populated from the benchmark docs and checked-in result artifacts. |
| `benchmark_history.sql` | Plain SQL dump that can recreate the database without opening the binary file. |
| `schema.sql` | Table and index definitions. |
| `results/std_toy_comparison_results.{txt,json}` | Focused optimized `OrderBook` versus std-toy direct-book comparison artifacts. |
| `results/single_order_latency_results.{txt,json}` | Per-action `Exchange::process(action)` latency artifacts from the full EC2 suite. |

## Database Usage

Detailed instructions for opening, querying, exporting, recreating, and updating
the SQLite benchmark history now live in `../BENCHMARKS.md` under
`Benchmark History Database`.

## Data Notes

Missing historical fields are stored as `NULL` rather than inferred. The `source_doc` and `source_artifact` columns identify where each row came from, and notes carry short context when a version, commit, or benchmark interpretation is only available from surrounding documentation.

Official throughput and latency rows come from normal native benchmark runs.
Supplemental `perf stat` diagnostics are stored separately and may report
unsupported PMU counters; they should not be used as headline throughput rows.

Single-order latency rows use the existing `p50_latency_ns`, `p95_latency_ns`,
`p99_latency_ns`, `p999_latency_ns`, and `max_latency_ns` columns. The runner
also writes `timer_overhead_ns` in JSON artifacts as measurement context; keep
that value in `notes` or `source_artifact` provenance unless the schema is
expanded later.
