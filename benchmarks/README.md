# Benchmark History Database

The Markdown benchmark reports remain the primary reviewer-facing view:

- `../BENCHMARKS.md` summarizes the latest polished results.
- `../docs/benchmark_history.md` keeps the web-readable historical log.

This directory also includes a small SQLite copy for technical reviewers who want to query benchmark history directly.

The current v1 benchmark documentation carries forward the focused
core/realistic/std-toy comparison refresh from the native EC2 `c7i-flex.large`
run on `2026-05-23T08:54:21Z` at local source commit `7a5980e1`, plus
benchmark-runner changes incorporated before the v1 milestone. The latest full
suite including stress and replay remains the `2026-05-23T08:10:34Z` run at
commit `53240e0`. Earlier benchmark rows were run on EC2 `t3.small` unless
their environment says otherwise; keep those rows as historical hardware
context rather than relabeling them.

## Files

| File | Purpose |
| --- | --- |
| `benchmark_history.db` | SQLite database populated from the benchmark docs and checked-in result artifacts. |
| `benchmark_history.sql` | Plain SQL dump that can recreate the database without opening the binary file. |
| `schema.sql` | Table and index definitions. |
| `results/std_toy_comparison_results.{txt,json}` | Focused optimized `OrderBook` versus std-toy direct-book comparison artifacts. |

## Database Usage

Detailed instructions for opening, querying, exporting, recreating, and updating
the SQLite benchmark history now live in `../BENCHMARKS.md` under
`Benchmark History Database`.

## Data Notes

Missing historical fields are stored as `NULL` rather than inferred. The `source_doc` and `source_artifact` columns identify where each row came from, and notes carry short context when a version, commit, or benchmark interpretation is only available from surrounding documentation.

Official throughput and latency rows come from normal native benchmark runs.
Supplemental `perf stat` diagnostics are stored separately and may report
unsupported PMU counters; they should not be used as headline throughput rows.
