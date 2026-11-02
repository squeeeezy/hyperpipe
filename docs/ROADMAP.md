# Roadmap

Correctness first, then simplicity, then speed, then features. Every change
is gated by `make test`, `make fuzz` and an A/B run with `bench/bench_ab.py`
showing no workload slower.

## Next

- Nested NDJSON values: pass objects and arrays through as JSON text instead
  of rejecting the line.
- MSVC: replace the GCC builtins and target attributes in the SIMD units.
- ARM: NEON versions of the CSV and NDJSON kernels.
- Positional parallel output on Windows: preallocate the file so chunks can
  be written at their offsets without NTFS zero-filling the gaps.
- Error positions: report the record number and byte offset of a malformed
  record.
- CI on Linux, macOS and Windows, x86-64 and ARM, with the fuzz target run
  for a fixed budget.

## Later

- zstd input and output through the run-time loaded compression layer.
- Parallel runs for gzip input by inflating ahead of the workers.
