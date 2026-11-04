# Roadmap

Correctness first, then simplicity, then speed, then features. Every change
is gated by `make test`, `make fuzz` and an A/B run with `bench/bench_ab.py`
showing no workload slower.

## Next

- `--distinct` and `count_distinct(col)`; quantiles (`p50`, `p95`) from a
  mergeable sketch.
- Join with a smaller file held in memory.
- Paths into nested NDJSON values (`meta.x.y`) in filters, `--select` and
  `--group-by`.
- MSVC: replace the GCC builtins and target attributes in the SIMD units.
- ARM: NEON versions of the CSV and NDJSON kernels.
- Positional parallel output on Windows: preallocate the file so chunks can
  be written at their offsets without NTFS zero-filling the gaps.

## Later

- zstd input and output through the run-time loaded compression layer.
- Parallel runs for gzip input by inflating ahead of the workers.
