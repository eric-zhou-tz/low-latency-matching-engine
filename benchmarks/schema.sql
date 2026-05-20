CREATE TABLE IF NOT EXISTS benchmark_results (
    id INTEGER PRIMARY KEY,
    version TEXT,
    commit_hash TEXT,
    run_date TEXT,
    environment TEXT,
    compiler TEXT,
    build_type TEXT,
    build_flags TEXT,
    benchmark_name TEXT NOT NULL,
    benchmark_category TEXT,
    workload_size TEXT,
    workload_parameter TEXT,
    time_ns REAL,
    cpu_time_ns REAL,
    items_per_second REAL,
    p50_latency_ns REAL,
    p95_latency_ns REAL,
    p99_latency_ns REAL,
    p999_latency_ns REAL,
    max_latency_ns REAL,
    notes TEXT,
    source_doc TEXT,
    source_artifact TEXT
);

CREATE INDEX IF NOT EXISTS idx_benchmark_results_run_date
    ON benchmark_results (run_date);

CREATE INDEX IF NOT EXISTS idx_benchmark_results_name
    ON benchmark_results (benchmark_name);

CREATE INDEX IF NOT EXISTS idx_benchmark_results_items_per_second
    ON benchmark_results (items_per_second);
