# Benchmark History Database

The Markdown benchmark reports remain the primary reviewer-facing view:

- `../BENCHMARKS.md` summarizes the latest polished results.
- `../docs/benchmark_history.md` keeps the web-readable historical log.

This directory also includes a small SQLite copy for technical reviewers who want to query benchmark history directly.

The current focused core/realistic/std-toy comparison refresh is the native EC2
`c7i-flex.large` run from `2026-05-23T08:54:21Z` at local source commit
`7a5980e1` plus uncommitted benchmark-runner changes. The latest full suite
including stress and replay remains the `2026-05-23T08:10:34Z` run at commit
`53240e0`. Earlier v0.x benchmark rows were run on EC2 `t3.small` unless their
environment says otherwise; keep those rows as historical hardware context
rather than relabeling them.

## Files

| File | Purpose |
| --- | --- |
| `benchmark_history.db` | SQLite database populated from the benchmark docs and checked-in result artifacts. |
| `benchmark_history.sql` | Plain SQL dump that can recreate the database without opening the binary file. |
| `schema.sql` | Table and index definitions. |
| `results/std_toy_comparison_results.{txt,json}` | Focused optimized `OrderBook` versus std-toy direct-book comparison artifacts. |

## Open the Database

```bash
sqlite3 benchmarks/benchmark_history.db
```

## Recreate from the SQL Dump

```bash
rm -f /tmp/benchmark_history.db
sqlite3 /tmp/benchmark_history.db < benchmarks/benchmark_history.sql
```

## Example Queries

```sql
SELECT * FROM benchmark_results ORDER BY run_date DESC;
```

```sql
SELECT version, benchmark_name, items_per_second
FROM benchmark_results
ORDER BY items_per_second DESC
LIMIT 10;
```

```sql
SELECT version, benchmark_name, p50_latency_ns, p99_latency_ns
FROM benchmark_results
WHERE p99_latency_ns IS NOT NULL
ORDER BY run_date DESC;
```

## Data Notes

Missing historical fields are stored as `NULL` rather than inferred. The `source_doc` and `source_artifact` columns identify where each row came from, and notes carry short context when a version, commit, or benchmark interpretation is only available from surrounding documentation.

Official throughput and latency rows come from normal native benchmark runs.
Supplemental `perf stat` diagnostics are stored separately and may report
unsupported PMU counters; they should not be used as headline throughput rows.
